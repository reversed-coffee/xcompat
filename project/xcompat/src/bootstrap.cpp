#include "bootstrap.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace xcompat::bootstrap {
    context* active_context = nullptr;
}

namespace {
    using namespace xcompat::bootstrap;

    struct tls_state {
        void** original_vector;
        void** expanded_vector;
        void** allocations;
        std::uint32_t vector_count;
    };

    struct thread_start_context {
        context* bootstrap;
        LPTHREAD_START_ROUTINE start;
        void* parameter;
        tls_state tls;
    };

    void set_failure(context* value, const stage current, const NTSTATUS status,
                     const std::uint32_t module_index = invalid_module) {
        value->module_index = module_index;
        value->status = status;
        value->current_stage = current;
    }

    void free_tls(context* value, tls_state& state) {
        if (state.allocations) {
            for (std::uint32_t i = 0; i < value->module_count; ++i) {
                if (state.allocations[i]) HeapFree(GetProcessHeap(), 0, state.allocations[i]);
            }
            HeapFree(GetProcessHeap(), 0, state.allocations);
        }
        if (state.expanded_vector) HeapFree(GetProcessHeap(), 0, state.expanded_vector);
        state = {};
    }

    bool prepare_tls(context* value, tls_state& state, const bool copy_native) {
        if (value->module_count > std::numeric_limits<std::uint32_t>::max() - value->native_tls_count)
            return false;
        state.vector_count = value->native_tls_count + value->module_count;
        state.original_vector = static_cast<void**>(NtCurrentTeb()->ThreadLocalStoragePointer);
        state.expanded_vector = static_cast<void**>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, static_cast<SIZE_T>(state.vector_count) * sizeof(void*)));
        state.allocations = static_cast<void**>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, static_cast<SIZE_T>(value->module_count) * sizeof(void*)));
        if (!state.expanded_vector || !state.allocations) {
            free_tls(value, state);
            return false;
        }
        if (copy_native && state.original_vector && value->native_tls_count) {
            std::memcpy(state.expanded_vector, state.original_vector,
                static_cast<SIZE_T>(value->native_tls_count) * sizeof(void*));
        }

        for (std::uint32_t i = 0; i < value->module_count; ++i) {
            const auto& module = value->modules[i];
            if (!module.tls_alignment) continue;
            const auto data_size = static_cast<SIZE_T>(module.tls_template_size) + module.tls_zero_fill;
            const auto allocation_size = std::max<SIZE_T>(data_size, 1) + module.tls_alignment - 1;
            auto allocation = static_cast<std::byte*>(HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, allocation_size));
            if (!allocation) {
                free_tls(value, state);
                return false;
            }
            state.allocations[i] = allocation;
            const auto aligned = (reinterpret_cast<std::uintptr_t>(allocation) + module.tls_alignment - 1) &
                ~(static_cast<std::uintptr_t>(module.tls_alignment) - 1);
            auto block = reinterpret_cast<void*>(aligned);
            if (module.tls_template_size) {
                std::memcpy(block, reinterpret_cast<const void*>(module.tls_template),
                    module.tls_template_size);
            }
            if (module.tls_index >= state.vector_count) {
                free_tls(value, state);
                return false;
            }
            state.expanded_vector[module.tls_index] = block;
        }
        return true;
    }

    void install_tls(tls_state& state, const bool copy_native) {
        state.original_vector = static_cast<void**>(NtCurrentTeb()->ThreadLocalStoragePointer);
        if (copy_native && state.original_vector && state.expanded_vector) {
            std::memcpy(state.expanded_vector, state.original_vector,
                static_cast<SIZE_T>(active_context->native_tls_count) * sizeof(void*));
        }
        NtCurrentTeb()->ThreadLocalStoragePointer = state.expanded_vector;
    }

    void restore_tls(tls_state& state) {
        NtCurrentTeb()->ThreadLocalStoragePointer = state.original_vector;
    }

    void notify_tls(const module_record& module, const DWORD reason, void* reserved) {
        for (std::uint32_t i = 0; i < module.callback_count; ++i) {
            const auto callback = reinterpret_cast<PIMAGE_TLS_CALLBACK>(module.callbacks[i]);
            callback(reinterpret_cast<void*>(module.base), reason, reserved);
        }
    }

    bool notify_entry(const module_record& module, const DWORD reason, void* reserved) {
        if (!module.entry_rva) return true;
        const auto entry = reinterpret_cast<BOOL (WINAPI*)(HINSTANCE, DWORD, void*)>(
            module.base + module.entry_rva);
        return entry(reinterpret_cast<HINSTANCE>(module.base), reason, reserved) != FALSE;
    }

    void unregister_exceptions(context* value, const std::uint32_t count) {
        for (std::uint32_t i = count; i-- > 0;) {
            const auto& module = value->modules[i];
            if (module.exception_count) {
                RtlDeleteFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(module.exception_table));
            }
        }
    }

    void detach_dlls(context* value, const std::uint32_t count, void* reserved) {
        for (std::uint32_t position = count; position-- > 0;) {
            const auto index = value->dll_order[position];
            const auto& module = value->modules[index];
            notify_tls(module, DLL_PROCESS_DETACH, nullptr);
            notify_entry(module, DLL_PROCESS_DETACH, reserved);
        }
    }

    bool initialize_process(context* value, tls_state& title_tls, std::uint32_t& registered) {
        value->current_stage = stage::process_parameters;
        UNICODE_STRING image_path{
            static_cast<USHORT>(value->image_path_length * sizeof(wchar_t)),
            static_cast<USHORT>((value->image_path_length + 1) * sizeof(wchar_t)),
            const_cast<wchar_t*>(value->image_path)
        };
        UNICODE_STRING working_directory{
            static_cast<USHORT>(value->working_directory_length * sizeof(wchar_t)),
            static_cast<USHORT>((value->working_directory_length + 1) * sizeof(wchar_t)),
            const_cast<wchar_t*>(value->working_directory)
        };
        UNICODE_STRING command_line{
            static_cast<USHORT>(value->command_line_length * sizeof(wchar_t)),
            static_cast<USHORT>((value->command_line_length + 1) * sizeof(wchar_t)),
            const_cast<wchar_t*>(value->command_line)
        };
        PRTL_USER_PROCESS_PARAMETERS parameters{};
        auto status = RtlCreateProcessParametersEx(&parameters, &image_path, nullptr,
            &working_directory, &command_line, value->environment, nullptr, nullptr,
            nullptr, nullptr, RTL_USER_PROC_PARAMS_NORMALIZED);
        if (!NT_SUCCESS(status)) {
            set_failure(value, stage::process_parameters, status);
            return false;
        }
        NtCurrentPeb()->ProcessParameters = parameters;
        NtCurrentPeb()->ImageBaseAddress = reinterpret_cast<void*>(
            value->modules[value->root_index].base);
        value->current_stage = stage::current_directory;
        if (!SetCurrentDirectoryW(value->working_directory)) {
            set_failure(value, stage::current_directory, HRESULT_FROM_WIN32(GetLastError()));
            return false;
        }

        value->current_stage = stage::exception_table;
        for (std::uint32_t i = 0; i < value->module_count; ++i) {
            const auto& module = value->modules[i];
            if (!module.exception_count) {
                ++registered;
                continue;
            }
            if (!RtlAddFunctionTable(reinterpret_cast<PRUNTIME_FUNCTION>(module.exception_table),
                                     module.exception_count, module.base)) {
                set_failure(value, stage::exception_table, STATUS_INVALID_IMAGE_FORMAT, i);
                return false;
            }
            ++registered;
        }

        value->current_stage = stage::tls_setup;
        value->module_index = invalid_module;
        if (!prepare_tls(value, title_tls, true)) {
            set_failure(value, stage::tls_setup, STATUS_NO_MEMORY);
            return false;
        }
        install_tls(title_tls, false);

        for (std::uint32_t position = 0; position < value->dll_order_count; ++position) {
            const auto index = value->dll_order[position];
            const auto& module = value->modules[index];
            value->module_index = index;
            value->current_stage = stage::dll_tls_attach;
            notify_tls(module, DLL_PROCESS_ATTACH, nullptr);
            value->current_stage = stage::dll_entry_attach;
            if (!notify_entry(module, DLL_PROCESS_ATTACH, reinterpret_cast<void*>(1))) {
                notify_tls(module, DLL_PROCESS_DETACH, nullptr);
                notify_entry(module, DLL_PROCESS_DETACH, nullptr);
                detach_dlls(value, position, nullptr);
                restore_tls(title_tls);
                free_tls(value, title_tls);
                unregister_exceptions(value, registered);
                registered = 0;
                set_failure(value, stage::dll_entry_attach, STATUS_DLL_INIT_FAILED, index);
                return false;
            }
            value->initialized_dll_count = position + 1;
        }

        const auto& root = value->modules[value->root_index];
        value->module_index = value->root_index;
        value->current_stage = stage::root_tls_attach;
        notify_tls(root, DLL_PROCESS_ATTACH, nullptr);
        return true;
    }

    NTSTATUS NTAPI title_entry(void* parameter) {
        auto* value = static_cast<context*>(parameter);
        tls_state title_tls{};
        std::uint32_t registered = 0;
        if (!initialize_process(value, title_tls, registered)) {
            SetEvent(value->initialized_event);
            WaitForSingleObject(value->run_event, INFINITE);
            return value->status;
        }

        value->module_index = invalid_module;
        value->status = STATUS_SUCCESS;
        value->current_stage = stage::ready;
        SetEvent(value->initialized_event);
        WaitForSingleObject(value->run_event, INFINITE);
        value->current_stage = stage::running;

        const auto& root = value->modules[value->root_index];
        const auto entry = reinterpret_cast<DWORD (WINAPI*)(PPEB)>(root.base + root.entry_rva);
        const auto result = entry(NtCurrentPeb());

        notify_tls(root, DLL_PROCESS_DETACH, nullptr);
        detach_dlls(value, value->initialized_dll_count, reinterpret_cast<void*>(1));
        restore_tls(title_tls);
        free_tls(value, title_tls);
        unregister_exceptions(value, registered);
        value->current_stage = stage::finished;
        return static_cast<NTSTATUS>(result);
    }

    DWORD WINAPI secondary_entry(void* parameter) {
        auto* wrapped = static_cast<thread_start_context*>(parameter);
        auto* value = wrapped->bootstrap;
        install_tls(wrapped->tls, true);
        for (std::uint32_t position = 0; position < value->initialized_dll_count; ++position) {
            const auto& module = value->modules[value->dll_order[position]];
            notify_tls(module, DLL_THREAD_ATTACH, nullptr);
            notify_entry(module, DLL_THREAD_ATTACH, nullptr);
        }

        const auto result = wrapped->start(wrapped->parameter);

        for (std::uint32_t position = value->initialized_dll_count; position-- > 0;) {
            const auto& module = value->modules[value->dll_order[position]];
            notify_tls(module, DLL_THREAD_DETACH, nullptr);
            notify_entry(module, DLL_THREAD_DETACH, nullptr);
        }
        restore_tls(wrapped->tls);
        free_tls(value, wrapped->tls);
        HeapFree(GetProcessHeap(), 0, wrapped);
        return result;
    }
}

namespace xcompat::bootstrap {
    NTSTATUS NTAPI clone_entry(void* parameter) {
        auto* control = static_cast<clone_control*>(parameter);
        WaitForSingleObject(control->metadata_ready_event, INFINITE);
        active_context = control->bootstrap_context;
        if (!active_context || active_context->magic != context_magic ||
            active_context->version != context_version) {
            RtlExitUserProcess(STATUS_INVALID_PARAMETER);
        }

        HANDLE title_thread{};
        const auto status = NtCreateThreadEx(&title_thread, THREAD_ALL_ACCESS, nullptr,
            NtCurrentProcess(), title_entry, active_context, 0, 0,
            static_cast<SIZE_T>(active_context->stack_commit),
            static_cast<SIZE_T>(active_context->stack_reserve), nullptr);
        if (!NT_SUCCESS(status)) {
            set_failure(active_context, stage::thread_create, status);
            SetEvent(active_context->initialized_event);
            WaitForSingleObject(active_context->run_event, INFINITE);
            RtlExitUserProcess(status);
        }

        WaitForSingleObject(title_thread, INFINITE);
        DWORD exit_code{};
        GetExitCodeThread(title_thread, &exit_code);
        CloseHandle(title_thread);
        RtlExitUserProcess(static_cast<NTSTATUS>(exit_code));
    }

    HANDLE WINAPI create_thread(
        LPSECURITY_ATTRIBUTES attributes,
        const SIZE_T stack_size,
        LPTHREAD_START_ROUTINE start_routine,
        void* parameter,
        const DWORD creation_flags,
        DWORD* thread_id
    ) {
        if (!active_context || !start_routine || !active_context->real_create_thread) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }

        auto* wrapped = static_cast<thread_start_context*>(HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(thread_start_context)));
        if (!wrapped) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        wrapped->bootstrap = active_context;
        wrapped->start = start_routine;
        wrapped->parameter = parameter;
        if (!prepare_tls(active_context, wrapped->tls, false)) {
            HeapFree(GetProcessHeap(), 0, wrapped);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }

        const auto real_create_thread = reinterpret_cast<decltype(&CreateThread)>(
            active_context->real_create_thread);
        const auto handle = real_create_thread(attributes, stack_size, secondary_entry,
            wrapped, creation_flags, thread_id);
        if (!handle) {
            const auto error = GetLastError();
            free_tls(active_context, wrapped->tls);
            HeapFree(GetProcessHeap(), 0, wrapped);
            SetLastError(error);
        }
        return handle;
    }

    void* NTAPI pc_to_file_header(void* address, void** image_base) {
        if (active_context) {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            for (std::uint32_t i = 0; i < active_context->module_count; ++i) {
                const auto& module = active_context->modules[i];
                if (value >= module.base && value - module.base < module.size) {
                    auto* base = reinterpret_cast<void*>(module.base);
                    if (image_base) *image_base = base;
                    return base;
                }
            }
        }
        if (!active_context || !active_context->real_pc_to_file_header) {
            if (image_base) *image_base = nullptr;
            return nullptr;
        }
        const auto real_pc_to_file_header = reinterpret_cast<decltype(&RtlPcToFileHeader)>(
            active_context->real_pc_to_file_header);
        return real_pc_to_file_header(address, image_base);
    }
}
