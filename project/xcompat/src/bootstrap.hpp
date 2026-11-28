#pragma once

#include <hydra/ost.hpp>

#include <cstddef>
#include <cstdint>

namespace xcompat::bootstrap {
    inline constexpr std::uint64_t context_magic = 0x58504354424f4f54ull;
    inline constexpr std::uint32_t context_version = 2;
    inline constexpr std::uint32_t invalid_module = 0xffff'ffffu;

    enum class stage : std::uint32_t {
        pending,
        thread_create,
        process_parameters,
        current_directory,
        exception_table,
        tls_setup,
        dll_tls_attach,
        dll_entry_attach,
        root_tls_attach,
        ready,
        running,
        finished
    };

    struct module_record {
        std::uintptr_t base;
        std::uint32_t size;
        std::uint32_t entry_rva;
        std::uintptr_t exception_table;
        std::uint32_t exception_count;
        std::uint32_t tls_index;
        std::uintptr_t tls_template;
        std::uint32_t tls_template_size;
        std::uint32_t tls_zero_fill;
        std::uint32_t tls_alignment;
        std::uint32_t callback_count;
        const std::uintptr_t* callbacks;
        std::uint32_t is_dll;
    };

    struct context {
        std::uint64_t magic;
        std::uint32_t version;
        std::uint32_t module_count;
        module_record* modules;
        std::uint32_t root_index;
        std::uint32_t dll_order_count;
        std::uint32_t* dll_order;
        std::uint32_t native_tls_count;
        std::uint32_t initialized_dll_count;
        const wchar_t* image_path;
        std::uint32_t image_path_length;
        const wchar_t* working_directory;
        std::uint32_t working_directory_length;
        const wchar_t* command_line;
        std::uint32_t command_line_length;
        void* environment;
        std::uint64_t stack_commit;
        std::uint64_t stack_reserve;
        HANDLE initialized_event;
        HANDLE run_event;
        std::uintptr_t real_create_thread;
        std::uintptr_t real_pc_to_file_header;
        volatile stage current_stage;
        volatile NTSTATUS status;
        volatile std::uint32_t module_index;
    };

    struct clone_control {
        context* bootstrap_context;
        HANDLE metadata_ready_event;
    };

    extern context* active_context;

    NTSTATUS NTAPI clone_entry(void* parameter);

    HANDLE WINAPI create_thread(
        LPSECURITY_ATTRIBUTES attributes,
        SIZE_T stack_size,
        LPTHREAD_START_ROUTINE start_routine,
        void* parameter,
        DWORD creation_flags,
        DWORD* thread_id
    );

    void* NTAPI pc_to_file_header(void* address, void** image_base);
}
