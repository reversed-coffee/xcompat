#include "xcompat/guest.hpp"
#include "xcompat/log.hpp"

#include <hydra/procstm.hpp>
#include <hydra/mod.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>

#include "xcompat/loader.hpp"

namespace xcompat {
    hy::proc guest::create_process() {
        hy::proc proc;
        HANDLE handle;

        hy::nt::error::assert(NtCreateProcessEx(
            &handle, PROCESS_ALL_ACCESS, nullptr, NtCurrentProcess(), 0,
            nullptr, nullptr, nullptr, 0
        ), "failed to create guest process");

        if (hy::proc::open_hnd(handle, &proc) != hy::STA_SUCCESS)
            throw std::runtime_error("cannot claim process");

        return proc;
    }

    hy::mod& guest::load_image(const std::filesystem::path& image_path, load_ctx* ctx) {
        const auto image_name = image_path.filename().string();
        if (modules.contains(image_name))
            throw std::runtime_error("image already loaded");

        auto image_module = loader::map_img_file_to_proc(proc, image_path);
        log::debug("Load \"{}\" -> {:X} (size: {:X})", image_name, image_module.base.i, image_module.size);

        auto [pair, success] = modules.emplace(image_name, std::move(image_module));
        if (!success)
            throw std::runtime_error("failed to register image");

        /* load deps */

        hy::err error = hy::STA_SUCCESS;

        for (auto&& import : pair->second.imports(&error)) {
            auto name = import.module;

            if (loader::is_api_set(name))
                name = loader::resolve_api_set(name, image_name);

            if (modules.contains(name))
                continue;

            const auto path = loader::resolve_literal_import(name, image_path.parent_path());
            if (path.empty()) {
                log::trace("Not found: \"{}\"", import.module);

                ctx->incomplete = true;
                ctx->missing_modules.insert(import.module);

                continue;
            }

            if (!modules.contains(path.filename().string()))
                load_image(path, ctx);

            if (error != hy::STA_SUCCESS)
                throw std::runtime_error("failed to enumerate imports");
        }

        return pair->second;
    }
}
