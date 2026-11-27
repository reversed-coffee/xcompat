#include <iostream>
#include <filesystem>
#include <string>

#include "xcompat/guest.hpp"

int usage() {
    std::cerr << "usage: xcompat.exe <exe_path>\n";
    return 1;
}

int main(const int argc, char* argv[]) {
    SetConsoleTitleA("xcompat");
    if (argc != 2) return usage();

    // verify executable path
    if (!std::filesystem::exists(argv[1])) {
        std::cerr << "invalid executable path provided\n";
        return usage();
    }

    // get canonical path
    const auto exe_path = std::filesystem::canonical(argv[1]);

    // start guest process
    xcompat::guest guest{};
    std::cout << "Guest PID: " << GetProcessId(guest.proc) << "\n";

    // load title image
    auto& title_module = guest.load_image(exe_path);

    std::cout << "Module:    0x" << std::hex << title_module.image.base.u << " (size: 0x" << title_module.image.size << ")\n";

    std::cout << "Press enter to close.";
    std::cin.get();

    return 0;
}
