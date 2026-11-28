#include "xcompat/guest.hpp"

#include "bootstrap.hpp"
#include "pe.hpp"
#include "xcompat/loader.hpp"
#include "xcompat/log.hpp"

#include <hydra/nt/err.hpp>

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
    enum class guest_state { empty, loaded, linked, initialized, exited, failed };

    struct staged_image {
        std::filesystem::path path;
        std::string key;
        xcompat::detail::pe_image metadata;
        bool native{};
        HMODULE native_base{};
        std::unordered_map<std::string, std::uintptr_t> native_exports;

        staged_image(std::filesystem::path path, std::string key,
                     xcompat::detail::pe_image&& metadata, const bool native)
            : path(std::move(path)), key(std::move(key)), metadata(std::move(metadata)), native(native) {}
    };

    struct resolved_symbol {
        std::uintptr_t address{};
        std::uintptr_t module_base{};
        std::string terminal_module;
        xcompat::link_error error{xcompat::link_error::export_not_found};
        bool success{};
    };

    struct patch_record {
        std::uintptr_t address;
        std::uintptr_t value;
        xcompat::unresolved_import source;
        bool report_failure;
    };

    std::wstring lowercase_path(const std::filesystem::path& path) {
        auto value = path.lexically_normal().wstring();
        std::ranges::transform(value, value.begin(), [](const wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
        return lowercase_path(left) == lowercase_path(right);
    }

    bool below_path(const std::filesystem::path& child, const std::filesystem::path& parent) {
        const auto child_value = lowercase_path(child);
        auto parent_value = lowercase_path(parent);
        if (!parent_value.ends_with(L'\\')) parent_value.push_back(L'\\');
        return child_value.starts_with(parent_value);
    }

    std::string symbol_key(const std::optional<std::string>& name,
                           const std::optional<std::uint16_t> ordinal) {
        return name ? "n:" + *name : std::format("o:{}", *ordinal);
    }

    std::wstring quote_argument(const std::wstring_view argument) {
        if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) return std::wstring(argument);
        std::wstring result(1, L'\"');
        std::size_t backslashes = 0;
        for (const auto ch : argument) {
            if (ch == L'\\') {
                ++backslashes;
                continue;
            }
            if (ch == L'\"') {
                result.append(backslashes * 2 + 1, L'\\');
                result.push_back(L'\"');
                backslashes = 0;
                continue;
            }
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(ch);
        }
        result.append(backslashes * 2, L'\\');
        result.push_back(L'\"');
        return result;
    }

    std::string stage_name(const xcompat::bootstrap::stage stage) {
        using enum xcompat::bootstrap::stage;
        switch (stage) {
        case pending: return "pending";
        case thread_create: return "thread_create";
        case process_parameters: return "process_parameters";
        case current_directory: return "current_directory";
        case exception_table: return "exception_table";
        case tls_setup: return "tls_setup";
        case dll_tls_attach: return "dll_tls_attach";
        case dll_entry_attach: return "dll_entry_attach";
        case root_tls_attach: return "root_tls_attach";
        case ready: return "ready";
        case running: return "running";
        case finished: return "finished";
        }
        return "unknown";
    }
}

namespace xcompat {
    struct guest::impl {
        guest_options options;
        guest_state state{guest_state::empty};
        std::vector<std::unique_ptr<staged_image>> staged;
        std::unordered_map<std::string, staged_image*> staged_by_key;
        std::vector<std::unique_ptr<module>> modules;
        std::unordered_map<std::string, module*> modules_by_key;
        module* root{};
        std::vector<HMODULE> native_references;
        std::filesystem::path windows_directory;
        std::uint32_t native_tls_count{};

        std::unique_ptr<bootstrap::clone_control> control;
        hy::nt::hnd metadata_ready_event;
        hy::nt::hnd initialized_event;
        hy::nt::hnd run_event;
        hy::nt::hnd process_handle;
        hy::nt::hnd reflection_thread;
        hy::proc process;
        hy::blk remote_context;
        link_report cached_report;
        bool link_attempted{};

        explicit impl(guest_options options) : options(std::move(options)) {
            wchar_t buffer[MAX_PATH]{};
            const auto length = GetWindowsDirectoryW(buffer, MAX_PATH);
            if (!length || length >= MAX_PATH)
                throw std::runtime_error("cannot resolve Windows directory");
            windows_directory = std::filesystem::canonical(buffer);
        }

        ~impl() {
            if (process_handle.not_null() && state != guest_state::exited) {
                TerminateProcess(process_handle, STATUS_PROCESS_IS_TERMINATING);
                WaitForSingleObject(process_handle, INFINITE);
            }
            for (auto it = native_references.rbegin(); it != native_references.rend(); ++it)
                FreeLibrary(*it);
        }

        [[noreturn]] void hard_failure(const std::string_view message) {
            if (process_handle.not_null()) {
                TerminateProcess(process_handle, STATUS_UNSUCCESSFUL);
                WaitForSingleObject(process_handle, INFINITE);
                state = guest_state::failed;
            }
            throw std::runtime_error(std::string(message));
        }

        std::string normalized_module(const std::string_view name, const std::string_view importer) const {
            if (loader::is_api_set(name)) {
                const auto host = loader::resolve_api_set(std::string(name), std::string(importer));
                return host.empty() ? loader::module_key(name) : loader::module_key(host);
            }
            return loader::module_key(name);
        }

        staged_image& stage_image(const std::filesystem::path& input_path, const bool root_image) {
            const auto path = std::filesystem::canonical(input_path);
            const auto key = loader::module_key(path.filename().string());
            if (const auto found = staged_by_key.find(key); found != staged_by_key.end()) {
                if (!same_path(found->second->path, path))
                    throw std::runtime_error("module name collision: " + key);
                return *found->second;
            }

            detail::pe_image metadata(path);
            const auto native = !root_image && below_path(path, windows_directory);
            auto image = std::make_unique<staged_image>(path, key, std::move(metadata), native);
            auto* result = image.get();
            staged_by_key.emplace(key, result);
            staged.push_back(std::move(image));

            if (native) {
                result->native_base = LoadLibraryExW(path.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
                if (!result->native_base)
                    throw std::runtime_error(std::format("{}: native load failed ({})",
                        path.string(), GetLastError()));
                native_references.push_back(result->native_base);
                return *result;
            }

            const auto stage_dependency = [&](const std::string_view raw_name) {
                const auto normalized = normalized_module(raw_name, key);
                if (staged_by_key.contains(normalized)) return;
                const auto dependency = loader::resolve_literal_import(
                    normalized, path.parent_path(), options.import_paths);
                if (!dependency.empty()) stage_image(dependency, false);
            };
            for (const auto& descriptor : result->metadata.imports)
                stage_dependency(descriptor.module);
            for (const auto& descriptor : result->metadata.delay_imports)
                stage_dependency(descriptor.module);
            for (const auto& exported : result->metadata.exports) {
                if (!exported.forwarder) continue;
                const auto separator = exported.forwarder->rfind('.');
                if (separator == std::string::npos || separator == 0) continue;
                stage_dependency(std::string_view(*exported.forwarder).substr(0, separator));
            }
            return *result;
        }

        void cache_native_export(staged_image& image, const std::optional<std::string>& name,
                                 const std::optional<std::uint16_t> ordinal) {
            const auto key = symbol_key(name, ordinal);
            if (image.native_exports.contains(key)) return;
            FARPROC address{};
            if (name) address = GetProcAddress(image.native_base, name->c_str());
            else address = GetProcAddress(image.native_base, MAKEINTRESOURCEA(*ordinal));
            image.native_exports.emplace(key, reinterpret_cast<std::uintptr_t>(address));
        }

        void cache_preloaded_exports() {
            for (const auto& image : staged) {
                if (image->native) continue;
                const auto cache_descriptor = [&](const auto& descriptor) {
                    const auto target_key = normalized_module(descriptor.module, image->key);
                    const auto found = staged_by_key.find(target_key);
                    if (found == staged_by_key.end() || !found->second->native) return;
                    for (const auto& thunk : descriptor.thunks)
                        cache_native_export(*found->second, thunk.name, thunk.ordinal);
                };
                for (const auto& descriptor : image->metadata.imports) cache_descriptor(descriptor);
                for (const auto& descriptor : image->metadata.delay_imports) cache_descriptor(descriptor);
                for (const auto& exported : image->metadata.exports) {
                    if (!exported.forwarder) continue;
                    const auto separator = exported.forwarder->rfind('.');
                    if (separator == std::string::npos || !separator || separator + 1 == exported.forwarder->size())
                        continue;
                    const auto target_key = normalized_module(
                        std::string_view(*exported.forwarder).substr(0, separator), image->key);
                    const auto found = staged_by_key.find(target_key);
                    if (found == staged_by_key.end() || !found->second->native) continue;
                    const auto suffix = std::string_view(*exported.forwarder).substr(separator + 1);
                    if (suffix.starts_with('#')) {
                        std::uint32_t value{};
                        const auto [end, error] = std::from_chars(suffix.data() + 1,
                            suffix.data() + suffix.size(), value);
                        if (error == std::errc{} && end == suffix.data() + suffix.size() &&
                            value <= std::numeric_limits<std::uint16_t>::max())
                            cache_native_export(*found->second, std::nullopt,
                                static_cast<std::uint16_t>(value));
                    } else {
                        cache_native_export(*found->second, std::string(suffix), std::nullopt);
                    }
                }
            }
        }

        void calculate_native_tls_count() {
            const auto* peb = NtCurrentPeb();
            const auto* head = &peb->Ldr->InLoadOrderModuleList;
            for (auto* link = head->Flink; link != head; link = link->Flink) {
                const auto* entry = CONTAINING_RECORD(link, LDR_DATA_TABLE_ENTRY, InLoadOrderLinks);
                const auto base = static_cast<const std::byte*>(entry->DllBase);
                if (!base) continue;
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) continue;
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
                if (nt->Signature != IMAGE_NT_SIGNATURE ||
                    nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_TLS ||
                    !nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress ||
                    entry->TlsIndex == std::numeric_limits<USHORT>::max())
                    continue;
                native_tls_count = std::max(native_tls_count,
                    static_cast<std::uint32_t>(entry->TlsIndex) + 1);
            }
        }

        void create_reflection() {
            SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, TRUE };
            metadata_ready_event.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
            initialized_event.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
            run_event.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
            if (!metadata_ready_event.not_null() || !initialized_event.not_null() || !run_event.not_null())
                throw std::runtime_error(std::format("cannot create bootstrap events ({})", GetLastError()));

            control = std::make_unique<bootstrap::clone_control>();
            control->metadata_ready_event = metadata_ready_event;
            PROCESS_REFLECTION_INFORMATION reflection{};
            const auto status = RtlCreateProcessReflection(NtCurrentProcess(),
                RTL_PROCESS_REFLECTION_FLAGS_INHERIT_HANDLES,
                reinterpret_cast<void*>(&bootstrap::clone_entry), control.get(), nullptr, &reflection);
            if (!NT_SUCCESS(status))
                throw std::runtime_error(std::format("process reflection failed: 0x{:08X}",
                    static_cast<std::uint32_t>(status)));
            process_handle.reset(reflection.ReflectionProcessHandle);
            reflection_thread.reset(reflection.ReflectionThreadHandle);
            if (hy::proc::open_hnd(process_handle, &process) != hy::STA_SUCCESS)
                hard_failure("cannot claim reflected process");
        }

        void map_images() {
            for (auto& image : staged) {
                if (image->native) continue;
                auto mapped = loader::map_img_file_to_proc(process, image->path);
                auto owned = std::unique_ptr<module>(new module(
                    image->path, image->key, std::move(mapped), std::move(image->metadata)));
                auto* pointer = owned.get();
                modules_by_key.emplace(image->key, pointer);
                modules.push_back(std::move(owned));
                if (!root) root = pointer;
                log::debug("Load \"{}\" -> {:X} (size: {:X})",
                    image->key, pointer->base().i, pointer->size());
            }
        }

        module& load(const std::filesystem::path& path) {
            if (state != guest_state::empty) throw std::logic_error("load_image requires an empty guest");
            try {
                auto& root_stage = stage_image(path, true);
                if (root_stage.metadata.stack_commit > std::max<std::uint64_t>(
                    root_stage.metadata.stack_reserve, 1024ull * 1024))
                    throw std::runtime_error("root stack commit exceeds reserve");
                cache_preloaded_exports();
                calculate_native_tls_count();
                create_reflection();
                map_images();
                state = guest_state::loaded;
                return *root;
            } catch (...) {
                if (process_handle.not_null()) {
                    TerminateProcess(process_handle, STATUS_UNSUCCESSFUL);
                    WaitForSingleObject(process_handle, INFINITE);
                    state = guest_state::failed;
                } else {
                    for (auto it = native_references.rbegin(); it != native_references.rend(); ++it)
                        FreeLibrary(*it);
                    native_references.clear();
                    staged.clear();
                    staged_by_key.clear();
                    state = guest_state::empty;
                }
                throw;
            }
        }

        resolved_symbol resolve_symbol(
            const std::string& target_key,
            const std::optional<std::string>& name,
            const std::optional<std::uint16_t> ordinal,
            std::unordered_set<std::string>& visited,
            const std::uint32_t depth
        ) {
            if (depth >= 32) return { .error = link_error::forwarder_depth };
            const auto identity = target_key + "!" + symbol_key(name, ordinal);
            if (!visited.insert(identity).second) return { .error = link_error::forwarder_cycle };

            if (const auto manual = modules_by_key.find(target_key); manual != modules_by_key.end()) {
                const auto* exported = name
                    ? manual->second->find_export(*name)
                    : manual->second->find_export(*ordinal);
                if (!exported) return { .error = link_error::export_not_found };
                if (!exported->forwarder) {
                    return { manual->second->base().i + exported->function_rva,
                        manual->second->base().i, target_key, link_error::export_not_found, true };
                }
                const auto separator = exported->forwarder->rfind('.');
                if (separator == std::string::npos || !separator ||
                    separator + 1 == exported->forwarder->size())
                    return { .error = link_error::malformed_forwarder };
                const auto forward_module = normalized_module(
                    std::string_view(*exported->forwarder).substr(0, separator), target_key);
                const auto suffix = std::string_view(*exported->forwarder).substr(separator + 1);
                if (suffix.starts_with('#')) {
                    std::uint32_t value{};
                    const auto [end, error] = std::from_chars(
                        suffix.data() + 1, suffix.data() + suffix.size(), value);
                    if (error != std::errc{} || end != suffix.data() + suffix.size() ||
                        value > std::numeric_limits<std::uint16_t>::max())
                        return { .error = link_error::malformed_forwarder };
                    return resolve_symbol(forward_module, std::nullopt,
                        static_cast<std::uint16_t>(value), visited, depth + 1);
                }
                return resolve_symbol(forward_module, std::string(suffix), std::nullopt,
                    visited, depth + 1);
            }

            const auto staged_target = staged_by_key.find(target_key);
            if (staged_target == staged_by_key.end()) return { .error = link_error::module_not_found };
            if (!staged_target->second->native) return { .error = link_error::module_not_found };
            const auto found = staged_target->second->native_exports.find(symbol_key(name, ordinal));
            if (found == staged_target->second->native_exports.end() || !found->second)
                return { .error = link_error::export_not_found };
            return { found->second, reinterpret_cast<std::uintptr_t>(staged_target->second->native_base),
                target_key, link_error::export_not_found, true };
        }

        void bind_descriptor(module& importer, const std::string& raw_module,
                             const std::span<const detail::import_thunk> thunks,
                             const import_kind kind, std::vector<patch_record>& patches,
                             bool& all_resolved) {
            const auto target_key = normalized_module(raw_module, importer.name());
            for (const auto& thunk : thunks) {
                unresolved_import issue{ importer.path(), raw_module, thunk.name, thunk.ordinal,
                    kind, link_error::export_not_found };
                std::unordered_set<std::string> visited;
                auto resolved = resolve_symbol(target_key, thunk.name, thunk.ordinal, visited, 0);
                if (!resolved.success) {
                    issue.error = resolved.error;
                    cached_report.unresolved.push_back(std::move(issue));
                    all_resolved = false;
                    continue;
                }
                if (thunk.name && *thunk.name == "CreateThread" &&
                    (target_key == "kernel32.dll" || target_key == "kernelbase.dll")) {
                    resolved.address = reinterpret_cast<std::uintptr_t>(&bootstrap::create_thread);
                }
                if (thunk.name && *thunk.name == "RtlPcToFileHeader" &&
                    (target_key == "kernel32.dll" || target_key == "kernelbase.dll" ||
                     target_key == "ntdll.dll")) {
                    resolved.address = reinterpret_cast<std::uintptr_t>(
                        &bootstrap::pc_to_file_header);
                }
                patches.push_back({ importer.base().i + thunk.iat_rva, resolved.address,
                    std::move(issue), true });
            }
        }

        link_report bind() {
            if (state != guest_state::loaded && state != guest_state::linked) {
                if (link_attempted) return cached_report;
                throw std::logic_error("link requires a loaded guest");
            }
            if (link_attempted) return cached_report;
            link_attempted = true;

            std::vector<patch_record> patches;
            for (const auto& importer : modules) {
                for (const auto& descriptor : importer->metadata().imports) {
                    bool resolved = true;
                    bind_descriptor(*importer, descriptor.module, descriptor.thunks,
                        import_kind::normal, patches, resolved);
                }
                for (const auto& descriptor : importer->metadata().delay_imports) {
                    bool resolved = true;
                    const auto before = patches.size();
                    bind_descriptor(*importer, descriptor.module, descriptor.thunks,
                        import_kind::delay, patches, resolved);
                    if (resolved) {
                        const auto target_key = normalized_module(descriptor.module, importer->name());
                        std::uintptr_t module_base{};
                        if (const auto manual = modules_by_key.find(target_key); manual != modules_by_key.end())
                            module_base = manual->second->base().i;
                        else if (const auto native = staged_by_key.find(target_key);
                                 native != staged_by_key.end() && native->second->native)
                            module_base = reinterpret_cast<std::uintptr_t>(native->second->native_base);
                        if (module_base) {
                            unresolved_import source{ importer->path(), descriptor.module,
                                std::nullopt, std::nullopt, import_kind::delay,
                                link_error::iat_write_failed };
                            patches.push_back({ importer->base().i + descriptor.module_handle_rva,
                                module_base, std::move(source), false });
                        } else {
                            patches.resize(before);
                        }
                    }
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
                    (patches[end].address & ~(static_cast<std::uintptr_t>(info.dwPageSize) - 1)) == page)
                    ++end;
                std::vector<std::byte> bytes((end - begin) * sizeof(std::uintptr_t));
                for (std::size_t i = begin; i < end; ++i) {
                    std::memcpy(bytes.data() + (i - begin) * sizeof(std::uintptr_t),
                        &patches[i].value, sizeof(std::uintptr_t));
                }
                if (!loader::write_remote(process, patches[begin].address, bytes)) {
                    for (std::size_t i = begin; i < end; ++i) {
                        if (!patches[i].report_failure) continue;
                        patches[i].source.error = link_error::iat_write_failed;
                        cached_report.unresolved.push_back(patches[i].source);
                    }
                }
                begin = end;
            }
            NtFlushInstructionCache(process_handle, nullptr, 0);
            if (cached_report.complete()) state = guest_state::linked;
            return cached_report;
        }

        std::vector<std::uint32_t> initialization_order() const {
            const auto count = modules.size();
            std::vector<std::vector<std::uint32_t>> edges(count);
            const auto add_target = [&](const std::uint32_t source, const std::string_view raw) {
                const auto key = normalized_module(raw, modules[source]->name());
                const auto found = modules_by_key.find(key);
                if (found == modules_by_key.end()) return;
                const auto target = static_cast<std::uint32_t>(std::ranges::find_if(modules,
                    [&](const auto& item) { return item.get() == found->second; }) - modules.begin());
                if (target != source && !std::ranges::contains(edges[source], target))
                    edges[source].push_back(target);
            };
            for (std::uint32_t i = 0; i < count; ++i) {
                for (const auto& descriptor : modules[i]->metadata().imports) add_target(i, descriptor.module);
                for (const auto& descriptor : modules[i]->metadata().delay_imports) add_target(i, descriptor.module);
                for (const auto& exported : modules[i]->metadata().exports) {
                    if (!exported.forwarder) continue;
                    const auto separator = exported.forwarder->rfind('.');
                    if (separator != std::string::npos && separator) add_target(i,
                        std::string_view(*exported.forwarder).substr(0, separator));
                }
            }

            std::vector<int> index(count, -1), low(count), stack;
            std::vector<bool> on_stack(count);
            std::vector<std::vector<std::uint32_t>> components;
            int next = 0;
            std::function<void(std::uint32_t)> visit = [&](const std::uint32_t node) {
                index[node] = low[node] = next++;
                stack.push_back(node);
                on_stack[node] = true;
                for (const auto target : edges[node]) {
                    if (index[target] < 0) {
                        visit(target);
                        low[node] = std::min(low[node], low[target]);
                    } else if (on_stack[target]) {
                        low[node] = std::min(low[node], index[target]);
                    }
                }
                if (low[node] != index[node]) return;
                auto& component = components.emplace_back();
                while (true) {
                    const auto value = static_cast<std::uint32_t>(stack.back());
                    stack.pop_back();
                    on_stack[value] = false;
                    component.push_back(value);
                    if (value == node) break;
                }
                std::ranges::sort(component);
            };
            for (std::uint32_t i = 0; i < count; ++i) if (index[i] < 0) visit(i);

            std::vector<std::uint32_t> component_of(count);
            for (std::uint32_t i = 0; i < components.size(); ++i)
                for (const auto node : components[i]) component_of[node] = i;
            std::vector<std::vector<std::uint32_t>> component_edges(components.size());
            for (std::uint32_t source = 0; source < count; ++source) {
                for (const auto target : edges[source]) {
                    const auto from = component_of[source];
                    const auto to = component_of[target];
                    if (from != to && !std::ranges::contains(component_edges[from], to))
                        component_edges[from].push_back(to);
                }
            }
            std::vector<bool> emitted(components.size());
            std::vector<std::uint32_t> result;
            std::function<void(std::uint32_t)> emit = [&](const std::uint32_t component) {
                if (emitted[component]) return;
                emitted[component] = true;
                for (const auto dependency : component_edges[component]) emit(dependency);
                result.insert(result.end(), components[component].begin(), components[component].end());
            };
            for (std::uint32_t i = 0; i < components.size(); ++i) emit(i);
            return result;
        }

        void initialize_guest() {
            if (state != guest_state::linked)
                throw std::logic_error("initialize requires a completely linked guest");

            try {
                auto order = initialization_order();
                const auto root_position = static_cast<std::uint32_t>(std::ranges::find_if(modules,
                    [&](const auto& item) { return item.get() == root; }) - modules.begin());
                std::vector<std::uint32_t> dll_order;
                for (const auto index : order) {
                    if (index != root_position && modules[index]->metadata().is_dll)
                        dll_order.push_back(index);
                }

                std::uint32_t next_tls = native_tls_count;
                for (const auto& item : modules) {
                    auto& tls = item->metadata().tls;
                    if (!tls.present) continue;
                    const auto index = next_tls++;
                    const std::span bytes{
                        reinterpret_cast<const std::byte*>(&index), sizeof(index) };
                    if (!loader::write_remote(process, item->base() + tls.index_rva, bytes))
                        hard_failure("failed to write static TLS index");
                }

                std::wstring image_path = root->path().wstring();
                auto working_directory = options.working_directory.empty()
                    ? root->path().parent_path().wstring()
                    : std::filesystem::absolute(options.working_directory).wstring();
                std::wstring command_line = quote_argument(image_path);
                for (const auto& argument : options.arguments) {
                    command_line.push_back(L' ');
                    command_line += quote_argument(argument);
                }
                if (image_path.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t) - 1 ||
                    working_directory.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t) - 1 ||
                    command_line.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t) - 1)
                    hard_failure("process metadata string exceeds UNICODE_STRING capacity");
                const auto environment = GetEnvironmentStringsW();
                if (!environment) hard_failure("cannot read process environment");
                std::size_t environment_chars = 1;
                while (environment[environment_chars - 1] || environment[environment_chars])
                    ++environment_chars;
                ++environment_chars;
                std::vector<wchar_t> environment_copy(environment, environment + environment_chars);
                FreeEnvironmentStringsW(environment);

                const auto align_up = [](const std::size_t value, const std::size_t alignment) {
                    return (value + alignment - 1) & ~(alignment - 1);
                };
                std::size_t total = sizeof(bootstrap::context);
                total = align_up(total, alignof(bootstrap::module_record));
                const auto modules_offset = total;
                total += modules.size() * sizeof(bootstrap::module_record);
                total = align_up(total, alignof(std::uint32_t));
                const auto order_offset = total;
                total += dll_order.size() * sizeof(std::uint32_t);
                std::vector<std::size_t> callback_offsets(modules.size());
                for (std::size_t i = 0; i < modules.size(); ++i) {
                    total = align_up(total, alignof(std::uintptr_t));
                    callback_offsets[i] = total;
                    total += modules[i]->metadata().tls.callback_rvas.size() * sizeof(std::uintptr_t);
                }
                const auto append_wide = [&](const std::wstring_view value, std::size_t& offset) {
                    total = align_up(total, alignof(wchar_t));
                    offset = total;
                    total += (value.size() + 1) * sizeof(wchar_t);
                };
                std::size_t image_offset{}, working_offset{}, command_offset{};
                append_wide(image_path, image_offset);
                append_wide(working_directory, working_offset);
                append_wide(command_line, command_offset);
                total = align_up(total, alignof(wchar_t));
                const auto environment_offset = total;
                total += environment_copy.size() * sizeof(wchar_t);

                std::vector<std::byte> block(total);
                PVOID remote_base{};
                SIZE_T remote_size = total;
                const auto allocation_status = NtAllocateVirtualMemory(process_handle,
                    &remote_base, 0, &remote_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!NT_SUCCESS(allocation_status))
                    hard_failure(std::format("cannot allocate remote bootstrap context: 0x{:08X}",
                        static_cast<std::uint32_t>(allocation_status)));
                remote_context = hy::blk(remote_base, remote_size);
                const auto remote_at = [&](const std::size_t offset) {
                    return remote_context.base.i + offset;
                };

                auto* records = reinterpret_cast<bootstrap::module_record*>(block.data() + modules_offset);
                next_tls = native_tls_count;
                for (std::size_t i = 0; i < modules.size(); ++i) {
                    const auto& item = modules[i];
                    const auto& metadata = item->metadata();
                    auto& record = records[i];
                    record.base = item->base().i;
                    record.size = static_cast<std::uint32_t>(item->size());
                    record.entry_rva = metadata.entry_rva;
                    record.exception_table = metadata.exceptions.count
                        ? item->base().i + metadata.exceptions.rva : 0;
                    record.exception_count = metadata.exceptions.count;
                    record.tls_index = metadata.tls.present ? next_tls++ : 0;
                    record.tls_template = metadata.tls.template_size
                        ? item->base().i + metadata.tls.template_rva : 0;
                    record.tls_template_size = metadata.tls.template_size;
                    record.tls_zero_fill = metadata.tls.zero_fill;
                    record.tls_alignment = metadata.tls.present ? metadata.tls.alignment : 0;
                    record.callback_count = static_cast<std::uint32_t>(metadata.tls.callback_rvas.size());
                    record.callbacks = record.callback_count
                        ? reinterpret_cast<const std::uintptr_t*>(remote_at(callback_offsets[i])) : nullptr;
                    record.is_dll = metadata.is_dll;
                    auto* callbacks = reinterpret_cast<std::uintptr_t*>(block.data() + callback_offsets[i]);
                    for (std::size_t callback = 0; callback < metadata.tls.callback_rvas.size(); ++callback)
                        callbacks[callback] = item->base().i + metadata.tls.callback_rvas[callback];
                }
                if (!dll_order.empty()) std::memcpy(block.data() + order_offset,
                    dll_order.data(), dll_order.size() * sizeof(std::uint32_t));
                const auto copy_wide = [&](const std::wstring_view value, const std::size_t offset) {
                    std::memcpy(block.data() + offset, value.data(), value.size() * sizeof(wchar_t));
                };
                copy_wide(image_path, image_offset);
                copy_wide(working_directory, working_offset);
                copy_wide(command_line, command_offset);
                std::memcpy(block.data() + environment_offset, environment_copy.data(),
                    environment_copy.size() * sizeof(wchar_t));

                SYSTEM_INFO system_info{};
                GetSystemInfo(&system_info);
                const auto& root_metadata = root->metadata();
                const auto round_up = [](const std::uint64_t value, const std::uint64_t alignment) {
                    return value ? (value + alignment - 1) & ~(alignment - 1) : 0;
                };
                auto* context = reinterpret_cast<bootstrap::context*>(block.data());
                context->magic = bootstrap::context_magic;
                context->version = bootstrap::context_version;
                context->module_count = static_cast<std::uint32_t>(modules.size());
                context->modules = reinterpret_cast<bootstrap::module_record*>(remote_at(modules_offset));
                context->root_index = root_position;
                context->dll_order_count = static_cast<std::uint32_t>(dll_order.size());
                context->dll_order = reinterpret_cast<std::uint32_t*>(remote_at(order_offset));
                context->native_tls_count = native_tls_count;
                context->image_path = reinterpret_cast<const wchar_t*>(remote_at(image_offset));
                context->image_path_length = static_cast<std::uint32_t>(image_path.size());
                context->working_directory = reinterpret_cast<const wchar_t*>(remote_at(working_offset));
                context->working_directory_length = static_cast<std::uint32_t>(working_directory.size());
                context->command_line = reinterpret_cast<const wchar_t*>(remote_at(command_offset));
                context->command_line_length = static_cast<std::uint32_t>(command_line.size());
                context->environment = reinterpret_cast<void*>(remote_at(environment_offset));
                context->stack_commit = round_up(root_metadata.stack_commit, system_info.dwPageSize);
                context->stack_reserve = round_up(std::max<std::uint64_t>(
                    root_metadata.stack_reserve, 1024ull * 1024), system_info.dwAllocationGranularity);
                context->initialized_event = initialized_event;
                context->run_event = run_event;
                context->real_create_thread = reinterpret_cast<std::uintptr_t>(
                    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateThread"));
                context->real_pc_to_file_header = reinterpret_cast<std::uintptr_t>(
                    GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlPcToFileHeader"));
                context->current_stage = bootstrap::stage::pending;
                context->status = STATUS_PENDING;
                context->module_index = bootstrap::invalid_module;
                if (!context->real_create_thread || !context->real_pc_to_file_header)
                    hard_failure("cannot resolve native bootstrap functions");

                if (process.mm_write(remote_context.base, block.data(), block.size()) != block.size())
                    hard_failure("cannot write remote bootstrap context");
                auto* remote_pointer = reinterpret_cast<bootstrap::context*>(remote_context.base.i);
                if (process.mm_write(control.get(), &remote_pointer, sizeof(remote_pointer)) != sizeof(remote_pointer))
                    hard_failure("cannot publish remote bootstrap context");
                SetEvent(metadata_ready_event);

                const HANDLE waits[]{ initialized_event, process_handle };
                const auto wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait != WAIT_OBJECT_0) {
                    DWORD exit_code{};
                    GetExitCodeProcess(process_handle, &exit_code);
                    hard_failure(std::format(
                        "guest exited during initialization: 0x{:08X}", exit_code));
                }
                bootstrap::context remote_status{};
                if (process.mm_read(&remote_status, remote_context.base) != sizeof(remote_status))
                    hard_failure("cannot read bootstrap status");
                if (remote_status.current_stage != bootstrap::stage::ready ||
                    !NT_SUCCESS(remote_status.status)) {
                    std::string module_name = "none";
                    if (remote_status.module_index < modules.size())
                        module_name = modules[remote_status.module_index]->path().string();
                    hard_failure(std::format("guest initialization failed at {} for {}: 0x{:08X}",
                        stage_name(remote_status.current_stage), module_name,
                        static_cast<std::uint32_t>(remote_status.status)));
                }
                state = guest_state::initialized;
            } catch (...) {
                if (state != guest_state::failed) {
                    TerminateProcess(process_handle, STATUS_UNSUCCESSFUL);
                    WaitForSingleObject(process_handle, INFINITE);
                    state = guest_state::failed;
                }
                throw;
            }
        }

        DWORD execute() {
            if (state != guest_state::initialized) throw std::logic_error("run requires an initialized guest");
            SetEvent(run_event);
            WaitForSingleObject(process_handle, INFINITE);
            DWORD exit_code{};
            if (!GetExitCodeProcess(process_handle, &exit_code)) {
                state = guest_state::failed;
                throw std::runtime_error(std::format("cannot read guest exit code ({})", GetLastError()));
            }
            state = guest_state::exited;
            return exit_code;
        }
    };

    guest::guest(guest_options options) : impl_(std::make_unique<impl>(std::move(options))) {}
    guest::~guest() = default;

    module& guest::load_image(const std::filesystem::path& image_path) {
        return impl_->load(image_path);
    }

    link_report guest::link() { return impl_->bind(); }
    void guest::initialize() { impl_->initialize_guest(); }
    DWORD guest::run() { return impl_->execute(); }

    hy::proc& guest::process() {
        if (!impl_->process_handle.not_null()) throw std::logic_error("process unavailable before reflection");
        return impl_->process;
    }
}
