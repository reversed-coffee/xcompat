#include "xcompat/guest.hpp"

#include <hydra/procstm.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cctype>
#include <span>
#include <stdexcept>
#include <system_error>

#include "xcompat/log.hpp"

namespace {
    [[nodiscard]] bool is_api_set(const std::string_view name) {
        return name.starts_with("api-") || name.starts_with("ext-");
    }

    [[nodiscard]] std::optional<std::wstring> widen_ascii(const std::string_view value) {
        std::wstring result;
        result.reserve(value.size());

        for (const unsigned char character : value) {
            if (character > 0x7F)
                return std::nullopt;

            result.push_back(character);
        }

        return result;
    }

    [[nodiscard]] std::optional<std::filesystem::path> module_path(const HMODULE module) {
        std::wstring path(512, L'\0');

        for (;;) {
            const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (!length)
                return std::nullopt;

            if (length < path.size()) {
                path.resize(length);
                return std::filesystem::path(std::move(path));
            }

            if (path.size() >= 32768)
                return std::nullopt;

            path.resize(path.size() * 2);
        }
    }

    class library_guard {
        HMODULE handle;

    public:
        explicit library_guard(const HMODULE handle) : handle(handle) {}

        ~library_guard() {
            FreeLibrary(handle);
        }

        library_guard(const library_guard&) = delete;
        library_guard& operator=(const library_guard&) = delete;
    };

    class writable_segments {
        hy::proc& process;
        std::span<const hy::seg> segments;
        std::size_t protected_count{};
        bool valid{ true };

    public:
        writable_segments(hy::proc& process, const std::span<const hy::seg> segments) :
            process(process),
            segments(segments) {
            for (; protected_count < segments.size(); ++protected_count) {
                const auto& segment = segments[protected_count];
                if (segment.size && !process.mm_protect(segment.base, segment.size, segment.mode | hy::mem_mode::write)) {
                    valid = false;
                    return;
                }
            }
        }

        ~writable_segments() {
            (void)restore();
        }

        writable_segments(const writable_segments&) = delete;
        writable_segments& operator=(const writable_segments&) = delete;

        [[nodiscard]] explicit operator bool() const {
            return valid;
        }

        [[nodiscard]] bool restore() {
            bool success = true;

            while (protected_count) {
                const auto& segment = segments[--protected_count];
                if (segment.size && !process.mm_protect(segment.base, segment.size, segment.mode))
                    success = false;
            }

            return success;
        }
    };
}

namespace xcompat {
    std::string guest::normalize_module_name(const std::string_view name) {
        const auto separator = name.find_last_of("\\/");
        std::string normalized(name.substr(separator == std::string_view::npos ? 0 : separator + 1));

        std::ranges::transform(normalized, normalized.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

        if (!normalized.contains('.'))
            normalized += ".dll";

        return normalized;
    }

    std::optional<std::filesystem::path> guest::resolve_system_image(const std::string_view name) {
        const auto wide_name = widen_ascii(name);
        if (!wide_name)
            return std::nullopt;

        const auto handle = LoadLibraryExW(
            wide_name->c_str(),
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32 | DONT_RESOLVE_DLL_REFERENCES
        );
        if (!handle)
            return std::nullopt;

        const library_guard guard(handle);
        return module_path(handle);
    }

    hy::proc guest::create_process() {
        HANDLE handle;

        hy::nt::error::assert(NtCreateProcessEx(
            &handle,
            PROCESS_ALL_ACCESS,
            nullptr,
            NtCurrentProcess(),
            0,
            nullptr,
            nullptr,
            nullptr,
            0
        ), "failed to create guest process");

        hy::proc proc;
        if (hy::proc::open_hnd(handle, &proc) != hy::STA_SUCCESS)
            throw std::runtime_error("cannot claim process");

        return proc;
    }

    module* guest::find_module(const std::string_view name) {
        const auto found = modules.find(normalize_module_name(name));
        return found == modules.end() ? nullptr : &found->second;
    }

    std::optional<std::filesystem::path> guest::locate_image(
        const module& importer,
        const std::string_view name
    ) const {
        const std::filesystem::path filename(name);
        std::error_code error;

        const auto local_path = importer.path.parent_path() / filename;
        if (std::filesystem::is_regular_file(local_path, error))
            return local_path;

        error.clear();
        const auto title_path = title_directory / filename;
        if (title_path != local_path && std::filesystem::is_regular_file(title_path, error))
            return title_path;

        return resolve_system_image(name);
    }

    module* guest::load_dependency(module& importer, const std::string_view name) {
        auto module_name = normalize_module_name(name);
        if (auto* loaded = find_module(module_name))
            return loaded;

        if (unavailable_modules.contains(module_name))
            return nullptr;

        const auto path = locate_image(importer, module_name);
        if (!path) {
            unavailable_modules.emplace(module_name);
            log::error("No module '{}' required by '{}'", name, importer.image.name);
            return nullptr;
        }

        try {
            return &load_image(*path);
        }
        catch (const std::exception& error) {
            unavailable_modules.emplace(module_name);
            log::error("Failed to load module '{}' required by '{}': {}", name, importer.image.name, error.what());
            return nullptr;
        }
    }

    void guest::index_exports(module& loaded) {
        hy::err error = hy::STA_SUCCESS;

        for (const auto& exported : loaded.image.exports(&error)) {
            const auto inserted = loaded.exports.emplace(
                exported.ordinal,
                export_symbol{ exported.base, exported.forwarder }
            ).second;

            if (!inserted)
                throw std::runtime_error("duplicate export ordinal");

            if (exported.name && !loaded.export_names.emplace(*exported.name, exported.ordinal).second)
                throw std::runtime_error("duplicate export name");
        }

        if (error != hy::STA_SUCCESS)
            throw std::runtime_error("failed to enumerate exports");
    }

    module& guest::load_image(const std::filesystem::path& image_path) {
        const auto full_path = std::filesystem::canonical(image_path);
        const auto image_name = full_path.filename().string();
        const auto module_name = normalize_module_name(image_name);

        if (const auto found = modules.find(module_name); found != modules.end())
            return found->second;

        if (modules.empty())
            title_directory = full_path.parent_path();

        hy::nt::hnd sect;
        hy::blk sect_blk{ nullptr, 0 };

        const hy::nt::hnd file(CreateFileW(
            full_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        ));

        if (file == INVALID_HANDLE_VALUE)
            throw hy::nt::win32_error("failed to open image file");

        hy::nt::error::assert(
            NtCreateSection(&sect, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE, file),
            "failed to create image section"
        );

        hy::nt::error::assert(
            NtMapViewOfSection(sect, proc, &sect_blk.base.u, 0, 0, nullptr, &sect_blk.size, ViewUnmap, 0, PAGE_READONLY),
            "failed to map image in guest process"
        );

        log::debug("Load image: \"{}\" -> {:X} (size: {:X})", image_name, sect_blk.base.i, sect_blk.size);
        section section{ std::move(sect), sect_blk };

        // load extern pe
        hy::procstm image_stm(proc, section.block);
        hy::mod image(image_name, std::move(image_stm), hy::mod_state::mapped);

        // parse pe
        if (image.parse() != hy::STA_SUCCESS)
            throw std::runtime_error("failed to parse pe");

        std::vector<hy::seg> segments;
        hy::err segment_error = hy::STA_SUCCESS;

        for (const auto& segment : image.segments(&segment_error))
            segments.push_back(segment);

        if (segment_error != hy::STA_SUCCESS)
            throw std::runtime_error("failed to enumerate image sections");

        // relocate image
        writable_segments writable(proc, segments);
        if (!writable)
            throw std::runtime_error("failed to make image sections writable");

        const auto relocation_error = image.reloc(section.block.base);
        const auto protections_restored = writable.restore();

        if (relocation_error != hy::STA_SUCCESS)
            throw std::runtime_error("relocation failed");

        if (!protections_restored)
            throw std::runtime_error("failed to restore image section protections");

        auto loaded = module{ full_path, std::move(section), std::move(image), std::move(segments) };
        index_exports(loaded);

        const auto [pair, success] = modules.emplace(module_name, std::move(loaded));
        if (!success)
            throw std::runtime_error("failed to add to registry");

        return pair->second;
    }

    std::optional<hy::ptr> guest::resolve_system_export(
        module& source,
        const std::string_view module_name,
        const std::optional<std::string>& name,
        const std::optional<std::uint32_t>& ordinal,
        bool& dependencies_resolved
    ) {
        const auto wide_name = widen_ascii(module_name);
        if (!wide_name)
            return std::nullopt;

        const auto handle = LoadLibraryExW(wide_name->c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!handle) {
            if (unavailable_modules.emplace(normalize_module_name(module_name)).second)
                log::error("No module '{}' required by '{}'", module_name, source.image.name);

            return std::nullopt;
        }

        const library_guard guard(handle);
        FARPROC procedure = nullptr;

        if (name) {
            procedure = GetProcAddress(handle, name->c_str());
        }
        else if (ordinal && *ordinal <= UINT16_MAX) {
            procedure = GetProcAddress(handle, MAKEINTRESOURCEA(*ordinal));
        }

        if (!procedure)
            return std::nullopt;

        HMODULE implementation;
        if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(procedure),
            &implementation
        )) {
            return std::nullopt;
        }

        const auto path = module_path(implementation);
        if (!path)
            return std::nullopt;

        module* target;

        try {
            target = &load_image(*path);
        }
        catch (const std::exception& error) {
            log::error("Failed to load module '{}' required by '{}': {}", path->string(), source.image.name, error.what());
            return std::nullopt;
        }

        if (!resolve_imports(*target))
            dependencies_resolved = false;

        const auto implementation_base = reinterpret_cast<std::uintptr_t>(implementation);
        const auto procedure_address = reinterpret_cast<std::uintptr_t>(procedure);
        if (procedure_address < implementation_base)
            return std::nullopt;

        const auto rva = procedure_address - implementation_base;
        if (rva >= target->mapping.block.size)
            return std::nullopt;

        return target->image.base + rva;
    }

    std::optional<hy::ptr> guest::resolve_export(
        module& source,
        const std::optional<std::string>& name,
        const std::optional<std::uint32_t>& ordinal,
        bool& dependencies_resolved,
        std::unordered_set<std::string>& chain
    ) {
        const auto symbol_name = name ? *name : '#' + std::to_string(ordinal.value_or(0));
        const auto chain_entry = normalize_module_name(source.image.name) + '!' + symbol_name;
        if (!chain.emplace(chain_entry).second)
            return std::nullopt;

        const export_symbol* symbol = nullptr;

        if (name) {
            const auto named = source.export_names.find(*name);
            if (named != source.export_names.end()) {
                const auto exported = source.exports.find(named->second);
                if (exported != source.exports.end())
                    symbol = &exported->second;
            }
        }
        else if (ordinal) {
            const auto exported = source.exports.find(*ordinal);
            if (exported != source.exports.end())
                symbol = &exported->second;
        }

        if (!symbol)
            return std::nullopt;

        if (!symbol->forwarder)
            return symbol->address;

        const auto separator = symbol->forwarder->rfind('.');
        if (separator == std::string::npos || separator == 0 || separator + 1 == symbol->forwarder->size())
            return std::nullopt;

        const std::string_view forward_module(symbol->forwarder->data(), separator);
        const std::string_view forward_symbol(symbol->forwarder->data() + separator + 1);
        std::optional<std::string> forward_name;
        std::optional<std::uint32_t> forward_ordinal;

        if (forward_symbol.front() != '#') {
            forward_name = forward_symbol;
        }
        else {
            std::uint32_t value;
            const auto [end, error] = std::from_chars(
                forward_symbol.data() + 1,
                forward_symbol.data() + forward_symbol.size(),
                value
            );
            if (error != std::errc{} || end != forward_symbol.data() + forward_symbol.size())
                return std::nullopt;

            forward_ordinal = value;
        }

        if (is_api_set(forward_module)) {
            return resolve_system_export(
                source,
                forward_module,
                forward_name,
                forward_ordinal,
                dependencies_resolved
            );
        }

        auto* target = load_dependency(source, forward_module);
        if (!target)
            return std::nullopt;

        if (!resolve_imports(*target))
            dependencies_resolved = false;

        return resolve_export(*target, forward_name, forward_ordinal, dependencies_resolved, chain);
    }

    bool guest::resolve_imports(module& loaded) {
        if (loaded.state == module_state::resolved || loaded.state == module_state::resolving)
            return true;

        if (loaded.state == module_state::failed)
            return false;

        loaded.state = module_state::resolving;
        bool success = true;
        hy::err error = hy::STA_SUCCESS;
        writable_segments writable(proc, loaded.segments);

        if (!writable) {
            log::error("Failed to make image sections writable for '{}'", loaded.image.name);
            loaded.state = module_state::failed;
            return false;
        }

        for (const auto& imported : loaded.image.imports(&error)) {
            const auto ordinal = imported.ordinal
                ? std::optional<std::uint32_t>(*imported.ordinal)
                : std::nullopt;
            std::optional<hy::ptr> address;

            if (is_api_set(imported.module)) {
                address = resolve_system_export(loaded, imported.module, imported.name, ordinal, success);

                if (!address && unavailable_modules.contains(normalize_module_name(imported.module))) {
                    success = false;
                    continue;
                }
            }
            else {
                auto* dependency = load_dependency(loaded, imported.module);
                if (!dependency) {
                    success = false;
                    continue;
                }

                if (!resolve_imports(*dependency))
                    success = false;

                std::unordered_set<std::string> chain;
                address = resolve_export(*dependency, imported.name, ordinal, success, chain);
            }

            if (!address) {
                if (imported.name) {
                    log::error(
                        "No import '{}!{}' from '{}'",
                        imported.module,
                        *imported.name,
                        loaded.image.name
                    );
                }
                else {
                    log::error(
                        "No import '{}!#{}' from '{}'",
                        imported.module,
                        imported.ordinal.value_or(0),
                        loaded.image.name
                    );
                }

                success = false;
                continue;
            }

            const auto value = address->i;
            if (imported.size != sizeof(value) ||
                proc.mm_write(imported.base, &value, sizeof(value)) != sizeof(value)) {
                log::error("Failed to patch import in '{}' at 0x{:X}", loaded.image.name, imported.base.i);
                success = false;
            }
        }

        if (error != hy::STA_SUCCESS) {
            log::error("Failed to enumerate imports for '{}'", loaded.image.name);
            success = false;
        }

        if (!writable.restore()) {
            log::error("Failed to restore image section protections for '{}'", loaded.image.name);
            success = false;
        }

        loaded.state = success ? module_state::resolved : module_state::failed;
        return success;
    }
}
