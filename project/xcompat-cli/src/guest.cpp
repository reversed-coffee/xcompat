#include "xcompat/guest.hpp"

#include <hydra/procstm.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cctype>
#include <span>
#include <stdexcept>
#include <system_error>

#include "xcompat/log.hpp"

namespace {
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

    std::optional<std::string> guest::resolve_api_set(const std::string_view name) {
        auto contract = normalize_module_name(name);
        if (!contract.starts_with("api-") && !contract.starts_with("ext-"))
            return std::nullopt;

        contract.resize(contract.size() - std::string_view(".dll").size());

        const auto* schema = static_cast<const API_SET_NAMESPACE*>(NtCurrentPeb()->ApiSetMap);
        if (!schema || schema->Version != API_SET_SCHEMA_VERSION_V6 || schema->Size < sizeof(*schema))
            return std::nullopt;

        const auto* base = reinterpret_cast<const std::byte*>(schema);
        const auto valid = [schema](const std::size_t offset, const std::size_t size) {
            return offset <= schema->Size && size <= schema->Size - offset;
        };

        if (!valid(schema->EntryOffset, static_cast<std::size_t>(schema->Count) * sizeof(API_SET_NAMESPACE_ENTRY)))
            return std::nullopt;

        const auto* entries = reinterpret_cast<const API_SET_NAMESPACE_ENTRY*>(base + schema->EntryOffset);
        for (ULONG entry_index = 0; entry_index < schema->Count; ++entry_index) {
            const auto& entry = entries[entry_index];
            if (entry.NameLength != contract.size() * sizeof(wchar_t) ||
                !valid(entry.NameOffset, entry.NameLength)) {
                continue;
            }

            const auto* entry_name = reinterpret_cast<const wchar_t*>(base + entry.NameOffset);
            bool matches = true;

            for (std::size_t index = 0; index < contract.size(); ++index) {
                if (entry_name[index] != static_cast<unsigned char>(contract[index])) {
                    matches = false;
                    break;
                }
            }

            if (!matches || !entry.ValueCount ||
                !valid(entry.ValueOffset, static_cast<std::size_t>(entry.ValueCount) * sizeof(API_SET_VALUE_ENTRY))) {
                continue;
            }

            const auto* values = reinterpret_cast<const API_SET_VALUE_ENTRY*>(base + entry.ValueOffset);
            const API_SET_VALUE_ENTRY* selected = &values[0];

            for (ULONG value_index = 0; value_index < entry.ValueCount; ++value_index) {
                if (!values[value_index].NameLength) {
                    selected = &values[value_index];
                    break;
                }
            }

            if (!selected->ValueLength || selected->ValueLength % sizeof(wchar_t) ||
                !valid(selected->ValueOffset, selected->ValueLength)) {
                return std::nullopt;
            }

            const auto* value = reinterpret_cast<const wchar_t*>(base + selected->ValueOffset);
            std::string host;
            host.reserve(selected->ValueLength / sizeof(wchar_t));

            for (std::size_t index = 0; index < selected->ValueLength / sizeof(wchar_t); ++index) {
                if (value[index] > 0x7F)
                    return std::nullopt;

                host.push_back(static_cast<char>(value[index]));
            }

            return host;
        }

        return std::nullopt;
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

        std::array<wchar_t, MAX_PATH> system_directory{};
        const auto length = GetSystemDirectoryW(system_directory.data(), system_directory.size());
        if (!length || length >= system_directory.size())
            return std::nullopt;

        error.clear();
        const auto system_path = std::filesystem::path(system_directory.data()) / filename;
        if (std::filesystem::is_regular_file(system_path, error))
            return system_path;

        return std::nullopt;
    }

    module* guest::load_dependency(module& importer, const std::string_view name) {
        auto module_name = normalize_module_name(name);
        if (auto* loaded = find_module(module_name))
            return loaded;

        if (const auto host = resolve_api_set(module_name)) {
            module_name = normalize_module_name(*host);

            if (auto* loaded = find_module(module_name))
                return loaded;
        }

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
        auto* target = load_dependency(source, forward_module);
        if (!target)
            return std::nullopt;

        if (!resolve_imports(*target))
            dependencies_resolved = false;

        if (forward_symbol.front() != '#')
            return resolve_export(
                *target,
                std::optional<std::string>(forward_symbol),
                std::nullopt,
                dependencies_resolved,
                chain
            );

        std::uint32_t forward_ordinal;
        const auto [end, error] = std::from_chars(
            forward_symbol.data() + 1,
            forward_symbol.data() + forward_symbol.size(),
            forward_ordinal
        );
        if (error != std::errc{} || end != forward_symbol.data() + forward_symbol.size())
            return std::nullopt;

        return resolve_export(*target, std::nullopt, forward_ordinal, dependencies_resolved, chain);
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
            auto* dependency = load_dependency(loaded, imported.module);
            if (!dependency) {
                success = false;
                continue;
            }

            if (!resolve_imports(*dependency))
                success = false;

            std::unordered_set<std::string> chain;
            const auto ordinal = imported.ordinal
                ? std::optional<std::uint32_t>(*imported.ordinal)
                : std::nullopt;
            const auto address = resolve_export(*dependency, imported.name, ordinal, success, chain);
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
