#pragma once

#include "xcompat/guest.hpp"

#include <hydra/blk.hpp>
#include <hydra/nt/hnd.hpp>
#include <hydra/proc.hpp>

namespace xcompat::detail {
    struct launch_context;

    struct clone_control_storage {
        launch_context* context{};
        HANDLE metadata_ready_event{};
    };

    class guest_process {
        clone_control_storage control_;
        hy::nt::hnd metadata_ready_event_;
        hy::nt::hnd ready_event_;
        hy::nt::hnd run_event_;
        hy::nt::hnd process_handle_;
        hy::nt::hnd reflection_thread_;
        hy::proc process_;
        hy::blk remote_context_;
        bool exited_{};

    public:
        guest_process();
        ~guest_process();

        guest_process(const guest_process&) = delete;
        guest_process& operator=(const guest_process&) = delete;
        guest_process(guest_process&&) = delete;
        guest_process& operator=(guest_process&&) = delete;

        [[nodiscard]] hy::proc& process() noexcept { return process_; }
        [[nodiscard]] HANDLE handle() const noexcept { return process_handle_; }
        [[nodiscard]] DWORD run(const module& root, const guest_options& options);
        void terminate_and_wait() noexcept;
    };
}
