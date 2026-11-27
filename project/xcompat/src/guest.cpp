#include "xcompat/guest.hpp"
#include "xcompat/log.hpp"

#include <hydra/procstm.hpp>
#include <hydra/mod.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>

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

    hy::mod guest::load_image(const std::filesystem::path& image_path) {
        const auto full_path = std::filesystem::canonical(image_path);
        const auto image_name = full_path.filename().string();

        hy::nt::hnd block_handle;
        hy::blk img_block{ nullptr, 0 };

        const hy::nt::hnd file(CreateFileW(
            full_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr
        ));

        if (file == INVALID_HANDLE_VALUE)
            throw hy::nt::win32_error("failed to open image file");

        hy::nt::error::assert(NtCreateSection(
            &block_handle, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE,
            file
        ), "failed to create image section");

        hy::nt::error::assert(NtMapViewOfSection(
            block_handle, proc, &img_block.base.u, 0, 0, nullptr, &img_block.size, ViewUnmap,
            0, PAGE_READONLY
        ), "failed to map image in guest process");

        log::debug("Load image: \"{}\" -> {:X} (size: {:X})", image_name, img_block.base.i, img_block.size);

        hy::procstm img_stm(proc, img_block);
        hy::img image(std::move(img_stm), hy::img_state::mapped);

        if (image.parse() != hy::STA_SUCCESS)
            throw std::runtime_error("failed to parse pe");

        if (image.reloc(img_block.base) != hy::STA_SUCCESS)
            throw std::runtime_error("relocation failed");

        return hy::mod(image_name, std::move(image));
    }
}
