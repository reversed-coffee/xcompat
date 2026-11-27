#include "xcompat/guest.hpp"
#include "xcompat/log.hpp"

#include <hydra/procstm.hpp>
#include <hydra/img.hpp>

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

    hy::img&& guest::load_image(const std::filesystem::path& image_path) {
        const auto full_path = std::filesystem::canonical(image_path);
        const auto image_name = full_path.filename().string();

        hy::nt::hnd sect;
        hy::blk sect_blk{ nullptr, 0 };

        const hy::nt::hnd file(CreateFileW(
            full_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr
        ));

        if (file == INVALID_HANDLE_VALUE)
            throw hy::nt::win32_error("failed to open image file");

        hy::nt::error::assert(NtCreateSection(
            &sect, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE,
            file
        ), "failed to create image section");

        hy::nt::error::assert(NtMapViewOfSection(
            sect, proc, &sect_blk.base.u, 0, 0, nullptr, &sect_blk.size, ViewUnmap,
            0, PAGE_READONLY
        ), "failed to map image in guest process");

        log::debug("Load image: \"{}\" -> {:X} (size: {:X})", image_name, sect_blk.base.i, sect_blk.size);
        section section{ std::move(sect), sect_blk };

        // load extern pe
        hy::procstm image_stm(proc, section.block);
        hy::img image(image_name, std::move(image_stm), hy::img_state::mapped);

        // parse pe
        if (image.parse() != hy::STA_SUCCESS)
            throw std::runtime_error("failed to parse pe");

        // relocate image
        if (image.reloc(section.block.base) != hy::STA_SUCCESS)
            throw std::runtime_error("relocation failed");

        return std::move(image);
    }
}
