#include <iostream>
#include <filesystem>
#include <string>

#include "xcompat/log.hpp"
#include "xcompat/guest.hpp"

int usage() {
    std::cerr << "usage: xcompat.exe <exe_path>\n";
    return 1;
}

int main(const int argc, char* argv[]) {
    SetConsoleTitleA("xcompat");
    xcompat::log::init();

    if (argc != 2)
        return usage();

    // verify executable path
    if (!std::filesystem::exists(argv[1])) {
        std::cerr << "invalid executable path provided\n";
        return usage();
    }

    // get canonical path
    const auto title_path = std::filesystem::canonical(argv[1]);
    xcompat::log::debug("Title path: {}", title_path.string());

    // start guest process
    xcompat::guest guest{};
    xcompat::log::info("Guest PID: 0x{:X}", GetProcessId(guest.proc));

    // load title image
    auto& title = guest.load_image(title_path);
    xcompat::log::info("Title module: 0x{:08X} (size: 0x{:X})", title.image.base.i, title.image.size);

    if (!guest.resolve_imports(title)) {
        xcompat::log::fatal("Failed to resolve title imports");
        return 1;
    }

    xcompat::log::info("Resolved {} modules", guest.module_count());

    std::cin.get();
    return 0;
}
