#include "xcompat/loader.hpp"

#include <hydra/proc.hpp>
#include <hydra/procstm.hpp>

namespace xcompat::loader {
    hy::mod map_img_file_to_proc(hy::proc& proc, const std::filesystem::path& path) {
        auto name = path.filename().string();

        hy::nt::hnd sec_hnd;
        hy::blk sec_blk{ nullptr, 0 };

        const hy::nt::hnd file(CreateFileA(
            path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr
        ));

        if (file == INVALID_HANDLE_VALUE)
            throw hy::nt::win32_error("failed to open image file");

        hy::nt::error::assert(NtCreateSection(
            &sec_hnd, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE,
            file
        ), "failed to create image section");

        hy::nt::error::assert(NtMapViewOfSection(
            sec_hnd, proc, &sec_blk.base.u, 0, 0, nullptr, &sec_blk.size, ViewUnmap,
            0, PAGE_READONLY
        ), "failed to map image in guest process");

        return hy::mod(proc, std::move(sec_hnd), sec_blk, name);
    }
}
