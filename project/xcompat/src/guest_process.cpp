#include "guest_process.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace xcompat::detail {
    namespace {
        inline constexpr std::uint64_t context_magic = 0x585043544c41554eull;
        inline constexpr std::uint32_t context_version = 1;
        inline constexpr std::uint64_t minimum_stack_reserve = 1024ull * 1024;

        NTSTATUS nt_status_from_dos_error(const DWORD error) {
            if (!error) return STATUS_SUCCESS;
            const auto last_status = RtlGetLastNtStatus();
            if (!NT_SUCCESS(last_status) &&
                RtlNtStatusToDosErrorNoTeb(last_status) == error)
                return last_status;
            return static_cast<NTSTATUS>(0xC0070000u | (error & 0xffffu));
        }

        enum class child_stage : std::uint32_t {
            pending,
            thread_create,
            process_parameters,
            current_directory,
            ready,
            running
        };
    }

    struct launch_context {
        std::uint64_t magic{};
        std::uint32_t version{};
        std::uintptr_t root_base{};
        std::uint32_t entry_rva{};
        std::uint32_t image_size{};
        const wchar_t* image_path{};
        std::uint32_t image_path_length{};
        const wchar_t* working_directory{};
        std::uint32_t working_directory_length{};
        const wchar_t* command_line{};
        std::uint32_t command_line_length{};
        void* environment{};
        std::uint64_t stack_commit{};
        std::uint64_t stack_reserve{};
        HANDLE ready_event{};
        HANDLE run_event{};
        volatile child_stage stage{child_stage::pending};
        volatile NTSTATUS status{STATUS_PENDING};
    };

    namespace {
        std::string stage_name(const child_stage stage) {
            switch (stage) {
            case child_stage::pending: return "pending";
            case child_stage::thread_create: return "thread_create";
            case child_stage::process_parameters: return "process_parameters";
            case child_stage::current_directory: return "current_directory";
            case child_stage::ready: return "ready";
            case child_stage::running: return "running";
            }
            return "pending";
        }

        void set_failure(launch_context* context, const child_stage stage,
                         const NTSTATUS status) {
            context->status = status;
            context->stage = stage;
        }

        NTSTATUS wait_for_run(launch_context* context) {
            if (!SetEvent(context->ready_event))
                return nt_status_from_dos_error(GetLastError());
            const auto wait = WaitForSingleObject(context->run_event, INFINITE);
            if (wait == WAIT_OBJECT_0) return STATUS_SUCCESS;
            if (wait == WAIT_FAILED)
                return nt_status_from_dos_error(GetLastError());
            return STATUS_UNSUCCESSFUL;
        }

        NTSTATUS NTAPI title_entry(void* parameter) {
            auto* context = static_cast<launch_context*>(parameter);
            context->stage = child_stage::process_parameters;

            UNICODE_STRING image_path{
                static_cast<USHORT>(context->image_path_length * sizeof(wchar_t)),
                static_cast<USHORT>((context->image_path_length + 1) * sizeof(wchar_t)),
                const_cast<wchar_t*>(context->image_path)
            };
            UNICODE_STRING working_directory{
                static_cast<USHORT>(context->working_directory_length * sizeof(wchar_t)),
                static_cast<USHORT>((context->working_directory_length + 1) * sizeof(wchar_t)),
                const_cast<wchar_t*>(context->working_directory)
            };
            UNICODE_STRING command_line{
                static_cast<USHORT>(context->command_line_length * sizeof(wchar_t)),
                static_cast<USHORT>((context->command_line_length + 1) * sizeof(wchar_t)),
                const_cast<wchar_t*>(context->command_line)
            };

            PRTL_USER_PROCESS_PARAMETERS parameters{};
            auto status = RtlCreateProcessParametersEx(&parameters, &image_path, nullptr,
                &working_directory, &command_line, context->environment, nullptr, nullptr,
                nullptr, nullptr, RTL_USER_PROC_PARAMS_NORMALIZED);
            if (!NT_SUCCESS(status)) {
                set_failure(context, child_stage::process_parameters, status);
                wait_for_run(context);
                return status;
            }

            NtCurrentPeb()->ProcessParameters = parameters;
            NtCurrentPeb()->ImageBaseAddress = reinterpret_cast<void*>(context->root_base);
            context->stage = child_stage::current_directory;
            if (!SetCurrentDirectoryW(context->working_directory)) {
                status = nt_status_from_dos_error(GetLastError());
                set_failure(context, child_stage::current_directory, status);
                wait_for_run(context);
                return status;
            }

            context->status = STATUS_SUCCESS;
            context->stage = child_stage::ready;
            status = wait_for_run(context);
            if (!NT_SUCCESS(status)) {
                set_failure(context, child_stage::ready, status);
                return status;
            }

            context->stage = child_stage::running;
            const auto entry = reinterpret_cast<DWORD (WINAPI*)(PPEB)>(
                context->root_base + context->entry_rva);
            return static_cast<NTSTATUS>(entry(NtCurrentPeb()));
        }

        NTSTATUS NTAPI clone_entry(void* parameter) {
            auto* control = static_cast<clone_control_storage*>(parameter);
            const auto metadata_wait = WaitForSingleObject(
                control->metadata_ready_event, INFINITE);
            if (metadata_wait != WAIT_OBJECT_0) {
                const auto status = metadata_wait == WAIT_FAILED
                    ? nt_status_from_dos_error(GetLastError())
                    : STATUS_UNSUCCESSFUL;
                RtlExitUserProcess(status);
            }

            auto* context = control->context;
            if (!context || context->magic != context_magic ||
                context->version != context_version) {
                RtlExitUserProcess(STATUS_INVALID_PARAMETER);
            }

            HANDLE title_thread{};
            context->stage = child_stage::thread_create;
            const auto create_status = NtCreateThreadEx(&title_thread, THREAD_ALL_ACCESS,
                nullptr, NtCurrentProcess(), title_entry, context, 0, 0,
                static_cast<SIZE_T>(context->stack_commit),
                static_cast<SIZE_T>(context->stack_reserve), nullptr);
            if (!NT_SUCCESS(create_status)) {
                set_failure(context, child_stage::thread_create, create_status);
                wait_for_run(context);
                RtlExitUserProcess(create_status);
            }

            const auto thread_wait = WaitForSingleObject(title_thread, INFINITE);
            DWORD exit_code{};
            NTSTATUS exit_status = STATUS_UNSUCCESSFUL;
            if (thread_wait == WAIT_OBJECT_0 && GetExitCodeThread(title_thread, &exit_code))
                exit_status = static_cast<NTSTATUS>(exit_code);
            else if (thread_wait == WAIT_FAILED || thread_wait == WAIT_OBJECT_0)
                exit_status = nt_status_from_dos_error(GetLastError());
            CloseHandle(title_thread);
            RtlExitUserProcess(exit_status);
        }

        std::wstring quote_argument(const std::wstring_view argument) {
            if (argument.empty()) return L"\"\"";
            if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
                return std::wstring(argument);

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

        void validate_unicode_string(const std::wstring_view value) {
            constexpr auto maximum_characters =
                std::numeric_limits<USHORT>::max() / sizeof(wchar_t);
            if (value.size() + 1 > maximum_characters)
                throw std::runtime_error("process metadata string exceeds UNICODE_STRING capacity");
        }
    }

    guest_process::guest_process() {
        SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, TRUE };
        metadata_ready_event_.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        ready_event_.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        run_event_.reset(CreateEventW(&attributes, TRUE, FALSE, nullptr));
        if (!metadata_ready_event_.not_null() || !ready_event_.not_null() ||
            !run_event_.not_null())
            throw std::runtime_error(std::format(
                "cannot create guest events ({})", GetLastError()));

        control_.metadata_ready_event = metadata_ready_event_;
        PROCESS_REFLECTION_INFORMATION reflection{};
        const auto status = RtlCreateProcessReflection(NtCurrentProcess(),
            RTL_PROCESS_REFLECTION_FLAGS_INHERIT_HANDLES,
            reinterpret_cast<void*>(&clone_entry), &control_, nullptr, &reflection);
        if (!NT_SUCCESS(status))
            throw std::runtime_error(std::format(
                "process reflection failed: 0x{:08X}", static_cast<std::uint32_t>(status)));

        process_handle_.reset(reflection.ReflectionProcessHandle);
        reflection_thread_.reset(reflection.ReflectionThreadHandle);
        if (hy::proc::open_hnd(process_handle_, &process_) != hy::STA_SUCCESS) {
            terminate_and_wait();
            throw std::runtime_error("cannot claim reflected process");
        }
    }

    guest_process::~guest_process() {
        terminate_and_wait();
    }

    void guest_process::terminate_and_wait() noexcept {
        if (!process_handle_.not_null() || exited_) return;
        const auto wait = WaitForSingleObject(process_handle_, 0);
        if (wait == WAIT_TIMEOUT)
            TerminateProcess(process_handle_, STATUS_PROCESS_IS_TERMINATING);
        WaitForSingleObject(process_handle_, INFINITE);
        exited_ = true;
    }

    DWORD guest_process::run(const module& root, const guest_options& options) {
        if (!root.execution_.entry_rva ||
            root.execution_.entry_rva >= root.execution_.image_size)
            throw std::runtime_error("root entry point is outside image");

        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const auto round_up = [](const std::uint64_t value, const std::uint64_t alignment,
                                 const std::string_view field) {
            if (value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1))
                throw std::runtime_error(std::format("{} rounding overflow", field));
            return value ? (value + alignment - 1) & ~(alignment - 1) : 0;
        };
        const auto stack_reserve = round_up(
            std::max(root.execution_.stack_reserve, minimum_stack_reserve),
            system_info.dwAllocationGranularity, "root stack reserve");
        const auto stack_commit = round_up(
            root.execution_.stack_commit, system_info.dwPageSize, "root stack commit");
        if (stack_commit > stack_reserve)
            throw std::runtime_error("root stack commit exceeds reserve");

        const auto image_path = root.path().wstring();
        const auto working_directory = options.working_directory.empty()
            ? root.path().parent_path().wstring()
            : std::filesystem::absolute(options.working_directory).wstring();
        std::wstring command_line = quote_argument(image_path);
        for (const auto& argument : options.arguments) {
            command_line.push_back(L' ');
            command_line += quote_argument(argument);
        }
        validate_unicode_string(image_path);
        validate_unicode_string(working_directory);
        validate_unicode_string(command_line);

        const auto environment = GetEnvironmentStringsW();
        if (!environment)
            throw std::runtime_error(std::format(
                "cannot read process environment ({})", GetLastError()));
        std::size_t environment_characters = 1;
        while (environment[environment_characters - 1] ||
               environment[environment_characters])
            ++environment_characters;
        ++environment_characters;
        std::vector<wchar_t> environment_copy(
            environment, environment + environment_characters);
        FreeEnvironmentStringsW(environment);

        const auto align_up = [](const std::size_t value, const std::size_t alignment) {
            return (value + alignment - 1) & ~(alignment - 1);
        };
        std::size_t total = sizeof(launch_context);
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
        const auto allocation_status = NtAllocateVirtualMemory(process_handle_,
            &remote_base, 0, &remote_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!NT_SUCCESS(allocation_status)) {
            terminate_and_wait();
            throw std::runtime_error(std::format(
                "cannot allocate remote launch context: 0x{:08X}",
                static_cast<std::uint32_t>(allocation_status)));
        }
        remote_context_ = hy::blk(remote_base, remote_size);
        const auto remote_at = [&](const std::size_t offset) {
            return remote_context_.base.i + offset;
        };
        const auto copy_wide = [&](const std::wstring_view value, const std::size_t offset) {
            std::memcpy(block.data() + offset, value.data(), value.size() * sizeof(wchar_t));
        };
        copy_wide(image_path, image_offset);
        copy_wide(working_directory, working_offset);
        copy_wide(command_line, command_offset);
        std::memcpy(block.data() + environment_offset, environment_copy.data(),
            environment_copy.size() * sizeof(wchar_t));

        auto* context = reinterpret_cast<launch_context*>(block.data());
        context->magic = context_magic;
        context->version = context_version;
        context->root_base = root.base.i;
        context->entry_rva = root.execution_.entry_rva;
        context->image_size = root.execution_.image_size;
        context->image_path = reinterpret_cast<const wchar_t*>(remote_at(image_offset));
        context->image_path_length = static_cast<std::uint32_t>(image_path.size());
        context->working_directory = reinterpret_cast<const wchar_t*>(remote_at(working_offset));
        context->working_directory_length = static_cast<std::uint32_t>(working_directory.size());
        context->command_line = reinterpret_cast<const wchar_t*>(remote_at(command_offset));
        context->command_line_length = static_cast<std::uint32_t>(command_line.size());
        context->environment = reinterpret_cast<void*>(remote_at(environment_offset));
        context->stack_commit = stack_commit;
        context->stack_reserve = stack_reserve;
        context->ready_event = ready_event_;
        context->run_event = run_event_;

        if (process_.mm_write(remote_context_.base, block.data(), block.size()) != block.size()) {
            terminate_and_wait();
            throw std::runtime_error("cannot write remote launch context");
        }
        auto* remote_pointer = reinterpret_cast<launch_context*>(remote_context_.base.i);
        if (process_.mm_write(&control_.context, &remote_pointer, sizeof(remote_pointer)) !=
            sizeof(remote_pointer)) {
            terminate_and_wait();
            throw std::runtime_error("cannot publish remote launch context");
        }
        if (!SetEvent(metadata_ready_event_)) {
            const auto error = GetLastError();
            terminate_and_wait();
            throw std::runtime_error(std::format(
                "cannot publish guest metadata ({})", error));
        }

        const HANDLE initialization_waits[]{ ready_event_, process_handle_ };
        const auto initialization_wait = WaitForMultipleObjects(
            2, initialization_waits, FALSE, INFINITE);
        if (initialization_wait != WAIT_OBJECT_0) {
            DWORD exit_code{};
            if (!GetExitCodeProcess(process_handle_, &exit_code))
                exit_code = static_cast<DWORD>(STATUS_UNSUCCESSFUL);
            terminate_and_wait();
            throw std::runtime_error(std::format(
                "guest exited during initialization: 0x{:08X}", exit_code));
        }

        launch_context remote_status{};
        if (process_.mm_read(&remote_status, remote_context_.base) != sizeof(remote_status)) {
            terminate_and_wait();
            throw std::runtime_error("cannot read guest initialization status");
        }
        if (remote_status.stage != child_stage::ready ||
            !NT_SUCCESS(remote_status.status)) {
            const auto message = std::format(
                "guest initialization failed at {}: 0x{:08X}",
                stage_name(remote_status.stage),
                static_cast<std::uint32_t>(remote_status.status));
            terminate_and_wait();
            throw std::runtime_error(message);
        }

        if (!SetEvent(run_event_)) {
            const auto error = GetLastError();
            terminate_and_wait();
            throw std::runtime_error(std::format("cannot run guest ({})", error));
        }
        const auto process_wait = WaitForSingleObject(process_handle_, INFINITE);
        if (process_wait != WAIT_OBJECT_0) {
            const auto status = process_wait == WAIT_FAILED
                ? nt_status_from_dos_error(GetLastError())
                : STATUS_UNSUCCESSFUL;
            terminate_and_wait();
            throw std::runtime_error(std::format(
                "guest wait failed: 0x{:08X}", static_cast<std::uint32_t>(status)));
        }

        DWORD exit_code{};
        if (!GetExitCodeProcess(process_handle_, &exit_code)) {
            const auto error = GetLastError();
            terminate_and_wait();
            throw std::runtime_error(std::format(
                "cannot read guest exit code ({})", error));
        }
        exited_ = true;
        return exit_code;
    }
}
