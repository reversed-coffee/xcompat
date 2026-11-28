#include "xcompat/guest.hpp"

#include "discovery.hpp"
#include "guest_process.hpp"
#include "linker.hpp"
#include "xcompat/loader.hpp"
#include "xcompat/log.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace xcompat {
    guest::guest(guest_options options) : options_(std::move(options)) {}

    guest::~guest() {
        process_.reset();
        linker_.reset();
    }

    module& guest::load_image(const std::filesystem::path& image_path) {
        if (state_ != state::empty)
            throw std::logic_error("load_image requires an empty guest");

        std::unique_ptr<detail::linker> new_linker;
        std::unique_ptr<detail::guest_process> new_process;
        try {
            auto discovered = detail::discover_images(image_path, options_.import_paths);
            if (discovered.manual_images.empty())
                throw std::runtime_error("root image was not discovered");

            const auto& root_metadata = discovered.manual_images.front().image.execution();
            if (root_metadata.stack_commit >
                std::max<std::uint64_t>(root_metadata.stack_reserve, 1024ull * 1024))
                throw std::runtime_error("root stack commit exceeds reserve");
            if (!root_metadata.entry_rva || root_metadata.entry_rva >= root_metadata.image_size)
                throw std::runtime_error("root entry point is outside image");

            new_linker = std::make_unique<detail::linker>(options_.import_paths);
            new_linker->preload_native_modules(discovered.native_images);
            new_process = std::make_unique<detail::guest_process>();

            modules_.reserve(discovered.manual_images.size());
            for (auto& image : discovered.manual_images) {
                auto mapped = loader::map_img_file_to_proc(new_process->process(), image.path);
                modules_.push_back(module(image.path, image.key, std::move(mapped),
                    image.image.execution(), image.image.take_delay_imports()));
                const auto& loaded = modules_.back();
                log::debug("Load \"{}\" -> {:X} (size: {:X})",
                    image.key, loaded.base.i, loaded.size);
            }

            linker_ = std::move(new_linker);
            process_ = std::move(new_process);
            state_ = state::loaded;
            return modules_.front();
        } catch (...) {
            modules_.clear();
            if (new_process) {
                new_process->terminate_and_wait();
                state_ = state::failed;
            } else {
                new_linker.reset();
                state_ = state::empty;
            }
            throw;
        }
    }

    link_report guest::link() {
        if (state_ != state::loaded || link_attempted_)
            throw std::logic_error("link requires an unlinked loaded guest");
        link_attempted_ = true;

        try {
            auto report = linker_->bind(process_->process(), modules_);
            if (report.complete())
                state_ = state::linked;
            else
                state_ = state::failed;
            return report;
        } catch (...) {
            process_->terminate_and_wait();
            state_ = state::failed;
            throw;
        }
    }

    DWORD guest::run() {
        if (state_ != state::linked)
            throw std::logic_error("run requires a linked guest");

        try {
            const auto exit_code = process_->run(modules_.front(), options_);
            state_ = state::exited;
            return exit_code;
        } catch (...) {
            process_->terminate_and_wait();
            state_ = state::failed;
            throw;
        }
    }

    hy::proc& guest::process() {
        if (!process_)
            throw std::logic_error("process unavailable before reflection");
        return process_->process();
    }
}
