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

    hy::mod& guest::load_image(const std::filesystem::path& image_path) {
        const auto image_name = image_path.filename().string();
        if (modules.contains(image_name))
            throw std::runtime_error("image already loaded");

        auto image_module = loader::map_img_file_to_proc(proc, image_path);
        log::debug("Load image: \"{}\" -> {:X} (size: {:X})", image_name, image_module.base.i, image_module.size);

        if (image_module.parse() != hy::STA_SUCCESS)
            throw std::runtime_error("failed to parse pe");

        if (image_module.reloc(image_module.base) != hy::STA_SUCCESS)
            throw std::runtime_error("relocation failed");

        auto [pair, success] = modules.emplace(image_name, std::move(image_module));
        if (!success)
            throw std::runtime_error("failed to register image");

        return pair->second;
    }
}
