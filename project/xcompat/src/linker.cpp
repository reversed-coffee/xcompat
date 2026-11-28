#include "linker.hpp"

#include "xcompat/loader.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {
    std::string symbol_key(const std::optional<std::string>& name,
                           const std::optional<std::uint16_t> ordinal) {
        return name ? "n:" + *name : std::format("o:{}", *ordinal);
    }

    void require_enumeration(const std::filesystem::path& path, const hy::err error,
                             const std::string_view subject) {
        if (error != hy::STA_SUCCESS)
            throw std::runtime_error(std::format("{}: failed to enumerate mapped PE {}",
                path.string(), subject));
    }
}

namespace xcompat::detail {
    linker::linker(const std::span<const std::filesystem::path> import_paths)
        : import_paths_(import_paths.begin(), import_paths.end()) {}

    linker::~linker() {
        for (auto item = native_modules_.rbegin(); item != native_modules_.rend(); ++item) {
            if (item->handle) FreeLibrary(item->handle);
        }
    }

    void linker::preload_native_modules(const std::span<const native_image_record> images) {
        struct module_guard {
            HMODULE handle{};

            ~module_guard() {
                if (handle) FreeLibrary(handle);
            }

            void release() noexcept { handle = nullptr; }
        };

        native_modules_.reserve(images.size());
        native_by_key_.reserve(images.size());
        for (const auto& image : images) {
            module_guard guard{ LoadLibraryExW(image.path.c_str(), nullptr,
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) };
            if (!guard.handle)
                throw std::runtime_error(std::format("{}: native load failed ({})",
                    image.path.string(), GetLastError()));

            loaded_native loaded{ image.path, image.key, guard.handle, {} };
            loaded.exports.reserve(image.symbols.size());
            for (const auto& symbol : image.symbols) {
                FARPROC address{};
                if (symbol.name)
                    address = GetProcAddress(guard.handle, symbol.name->c_str());
                else
                    address = GetProcAddress(guard.handle, MAKEINTRESOURCEA(*symbol.ordinal));
                loaded.exports.emplace(symbol_key(symbol.name, symbol.ordinal),
                    reinterpret_cast<std::uintptr_t>(address));
            }
            native_modules_.push_back(std::move(loaded));
            guard.release();
            native_by_key_.emplace(native_modules_.back().key, native_modules_.size() - 1);
        }
    }

    link_report linker::bind(hy::proc& process, const std::span<module> modules) {
        struct export_record {
            std::optional<std::string> name;
            std::uint32_t ordinal{};
            std::uintptr_t address{};
            std::optional<std::string> forwarder;
        };
        struct module_index {
            module* image{};
            std::vector<export_record> exports;
            std::unordered_map<std::string, std::size_t> named;
            std::unordered_map<std::uint32_t, std::size_t> ordinal;
        };
        struct resolved_symbol {
            std::uintptr_t address{};
            link_error error{link_error::export_not_found};
            bool success{};
        };
        struct patch_record {
            std::uintptr_t address{};
            std::uintptr_t value{};
            unresolved_import source;
            bool report_failure{};
        };

        std::vector<module_index> indexes;
        indexes.reserve(modules.size());
        std::unordered_map<std::string, std::size_t> manual_by_key;
        manual_by_key.reserve(modules.size());
        for (auto& image : modules) {
            module_index index{ .image = &image };
            hy::err export_error = hy::STA_SUCCESS;
            for (const auto& exported : image.exports(&export_error)) {
                index.exports.push_back({ exported.name, exported.ordinal, exported.base.i,
                    exported.forwarder });
            }
            require_enumeration(image.path(), export_error, "exports");
            index.named.reserve(index.exports.size());
            index.ordinal.reserve(index.exports.size());
            for (std::size_t export_index = 0; export_index < index.exports.size(); ++export_index) {
                const auto& exported = index.exports[export_index];
                if (exported.name) index.named.emplace(*exported.name, export_index);
                index.ordinal.emplace(exported.ordinal, export_index);
            }
            manual_by_key.emplace(std::string(image.key()), indexes.size());
            indexes.push_back(std::move(index));
        }

        const auto module_base = [&](const std::string& key) -> std::uintptr_t {
            if (const auto manual = manual_by_key.find(key); manual != manual_by_key.end())
                return indexes[manual->second].image->base.i;
            if (const auto native = native_by_key_.find(key); native != native_by_key_.end())
                return reinterpret_cast<std::uintptr_t>(native_modules_[native->second].handle);
            return 0;
        };

        std::function<resolved_symbol(
            const std::string&, const std::optional<std::string>&,
            const std::optional<std::uint16_t>&, std::unordered_set<std::string>&,
            std::uint32_t)> resolve_symbol;
        resolve_symbol = [&](const std::string& target_key,
                             const std::optional<std::string>& name,
                             const std::optional<std::uint16_t>& ordinal,
                             std::unordered_set<std::string>& visited,
                             const std::uint32_t depth) -> resolved_symbol {
            if (depth >= 32) return { .error = link_error::forwarder_depth };
            const auto identity = target_key + "!" + symbol_key(name, ordinal);
            if (!visited.insert(identity).second)
                return { .error = link_error::forwarder_cycle };

            if (const auto manual = manual_by_key.find(target_key);
                manual != manual_by_key.end()) {
                auto& index = indexes[manual->second];
                const export_record* exported{};
                if (name) {
                    const auto found = index.named.find(*name);
                    if (found != index.named.end()) exported = &index.exports[found->second];
                } else {
                    const auto found = index.ordinal.find(*ordinal);
                    if (found != index.ordinal.end()) exported = &index.exports[found->second];
                }
                if (!exported) return { .error = link_error::export_not_found };
                if (!exported->forwarder)
                    return { exported->address, link_error::export_not_found, true };

                const auto separator = exported->forwarder->rfind('.');
                if (separator == std::string::npos || !separator ||
                    separator + 1 == exported->forwarder->size())
                    return { .error = link_error::malformed_forwarder };
                const auto raw_module = std::string_view(*exported->forwarder).substr(0, separator);
                const auto target = resolve_import_target(raw_module, index.image->path(),
                    index.image->key(), import_paths_);
                const auto suffix = std::string_view(*exported->forwarder).substr(separator + 1);
                if (!suffix.starts_with('#')) {
                    if (target.path.empty())
                        return { .error = link_error::module_not_found };
                    return resolve_symbol(target.key, std::string(suffix), std::nullopt,
                        visited, depth + 1);
                }

                std::uint32_t value{};
                const auto [end, error] = std::from_chars(
                    suffix.data() + 1, suffix.data() + suffix.size(), value);
                if (error != std::errc{} || end != suffix.data() + suffix.size() ||
                    value > std::numeric_limits<std::uint16_t>::max())
                    return { .error = link_error::malformed_forwarder };
                if (target.path.empty())
                    return { .error = link_error::module_not_found };
                return resolve_symbol(target.key, std::nullopt,
                    static_cast<std::uint16_t>(value), visited, depth + 1);
            }

            const auto native = native_by_key_.find(target_key);
            if (native == native_by_key_.end())
                return { .error = link_error::module_not_found };
            const auto& image = native_modules_[native->second];
            const auto exported = image.exports.find(symbol_key(name, ordinal));
            if (exported == image.exports.end() || !exported->second)
                return { .error = link_error::export_not_found };
            return { exported->second, link_error::export_not_found, true };
        };

        link_report report;
        std::vector<patch_record> patches;
        const auto resolve_patch = [&](module& importer, const std::string& raw_module,
                                       const std::optional<std::string>& name,
                                       const std::optional<std::uint16_t>& ordinal,
                                       const import_kind kind, const std::uintptr_t slot) {
            unresolved_import issue{
                importer.path(), raw_module, name, ordinal, kind, link_error::export_not_found };
            const auto target = resolve_import_target(
                raw_module, importer.path(), importer.key(), import_paths_);
            if (target.path.empty()) {
                issue.error = link_error::module_not_found;
                report.unresolved.push_back(std::move(issue));
                return false;
            }
            std::unordered_set<std::string> visited;
            const auto resolved = resolve_symbol(target.key, name, ordinal, visited, 0);
            if (!resolved.success) {
                issue.error = resolved.error;
                report.unresolved.push_back(std::move(issue));
                return false;
            }
            patches.push_back({ slot, resolved.address, std::move(issue), true });
            return true;
        };

        for (auto& importer : modules) {
            hy::err import_error = hy::STA_SUCCESS;
            for (const auto& imported : importer.imports(&import_error)) {
                resolve_patch(importer, imported.module, imported.name, imported.ordinal,
                    import_kind::normal, imported.base.i);
            }
            require_enumeration(importer.path(), import_error, "imports");

            for (const auto& descriptor : importer.delay_imports_) {
                bool all_resolved = true;
                for (const auto& thunk : descriptor.thunks) {
                    if (!resolve_patch(importer, descriptor.module, thunk.name, thunk.ordinal,
                        import_kind::delay, importer.base.i + thunk.iat_rva))
                        all_resolved = false;
                }
                if (!all_resolved) continue;

                const auto target = resolve_import_target(
                    descriptor.module, importer.path(), importer.key(), import_paths_);
                const auto base = module_base(target.key);
                if (!base) continue;
                unresolved_import source{ importer.path(), descriptor.module, std::nullopt,
                    std::nullopt, import_kind::delay, link_error::iat_write_failed };
                patches.push_back({ importer.base.i + descriptor.module_handle_rva,
                    base, std::move(source), false });
            }
        }

        std::ranges::sort(patches, {}, &patch_record::address);
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        for (std::size_t begin = 0; begin < patches.size();) {
            auto end = begin + 1;
            const auto page = patches[begin].address &
                ~(static_cast<std::uintptr_t>(info.dwPageSize) - 1);
            while (end < patches.size() &&
                patches[end].address == patches[end - 1].address + sizeof(std::uintptr_t) &&
                (patches[end].address &
                    ~(static_cast<std::uintptr_t>(info.dwPageSize) - 1)) == page)
                ++end;

            std::vector<std::byte> bytes((end - begin) * sizeof(std::uintptr_t));
            for (std::size_t item = begin; item < end; ++item) {
                std::memcpy(bytes.data() + (item - begin) * sizeof(std::uintptr_t),
                    &patches[item].value, sizeof(std::uintptr_t));
            }
            if (!loader::write_remote(process, patches[begin].address, bytes)) {
                bool reported = false;
                for (std::size_t item = begin; item < end; ++item) {
                    if (!patches[item].report_failure) continue;
                    patches[item].source.error = link_error::iat_write_failed;
                    report.unresolved.push_back(patches[item].source);
                    reported = true;
                }
                if (!reported)
                    throw std::runtime_error(std::format(
                        "{}: delay module-handle write failed",
                        patches[begin].source.importer.string()));
            }
            begin = end;
        }

        const auto flush_status = NtFlushInstructionCache(process, nullptr, 0);
        if (!NT_SUCCESS(flush_status))
            throw std::runtime_error(std::format(
                "failed to flush guest instruction cache: 0x{:08X}",
                static_cast<std::uint32_t>(flush_status)));
        return report;
    }
}
