#include <phnt_windows.h>
#include <phnt.h>

#include <hydra/nt/hnd.hpp>

#include <iostream>
#include <optional>
#include <filesystem>
#include <string>

#include "hydra/blk.hpp"
#include "hydra/blkstm.hpp"
#include "hydra/mod.hpp"
#include "hydra/procstm.hpp"
#include "hydra/nt/err.hpp"

#pragma comment(lib, "ntdll.lib")

namespace xcompat {
    struct section {
        hy::nt::hnd handle;
        hy::blk block;
    };

    struct module {
        section mapping;
        hy::mod image;
    };

    class guest {
        static hy::proc create_process() {
            HANDLE handle;
            
            hy::nt::error::assert(NtCreateProcessEx(
                &handle,
                PROCESS_ALL_ACCESS,
                nullptr,
                NtCurrentProcess(),
                0,
                nullptr,
                nullptr,
                nullptr,
                0
            ), "Failed to create guest process");

            hy::proc proc;
            if (hy::proc::open_hnd(handle, &proc) != hy::STA_SUCCESS)
                throw std::runtime_error("cannot claim process");

            return proc;
        }

    public:
        hy::proc proc;
        std::unordered_map<std::string, std::unique_ptr<module>> modules;

        explicit guest() : proc(std::move(create_process())) {

        }

        [[nodiscard]] section load_image(const std::filesystem::path& image_path) {
            hy::nt::hnd sect;
            hy::blk sect_blk{ nullptr, 0 };

            const hy::nt::hnd file(CreateFileA(
                image_path.string().c_str(),
                GENERIC_READ,
                FILE_SHARE_READ,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr
            ));

            if (file == INVALID_HANDLE_VALUE)
                throw hy::nt::win32_error("failed to open image file");
            
            hy::nt::error::assert(
                NtCreateSection(&sect, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE, file),
                "failed to create image section"
            );

            hy::nt::error::assert(
                NtMapViewOfSection(sect, proc, &sect_blk.base.u, 0, 0, nullptr, &sect_blk.size, ViewUnmap, 0, PAGE_READONLY),
                "failed to map image in guest process"
            );
            
            section section{ std::move(sect), sect_blk };
            std::cout << "Mapped PE: " << section.block.base.u << "\n";

            // load extern pe
            auto image_name = image_path.filename().string();
            hy::procstm image_stm(proc, section.block);
            hy::mod image(image_name, image_stm, hy::mod_state::mapped);

            // parse pe
            if (image.parse() != hy::STA_SUCCESS)
                throw std::runtime_error("failed to parse pe");

            // relocate image
            if (image.reloc(section.block.base) != hy::STA_SUCCESS)
                throw std::runtime_error("relocation failed");

            hy::err error;
            for (auto import : image.imports(&error)) {
                std::cout << "import: " << import.name.value_or("ordinal") << " from " << import.module << "\n";
            }

            return section;
        }
    };    
}

int main(int argc, char* argv[]) {
    xcompat::guest guest{};
    std::cout << "Guest PID: " << GetProcessId(guest.proc) << "\n";

    auto section = guest.load_image(R"(C:\Users\nick\Documents\6StarTitle\game_scarlett_master.exe)");

    std::cin.get();

    return 0;
}
