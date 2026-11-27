#include <iostream>
#include <filesystem>
#include <string>

#include "xcompat/log.hpp"
#include "xcompat/guest.hpp"

static int usage() {
    std::cerr << "usage: xcompat <exe_path> <args...>\n";
    return 1;
}

static void init() {
    SetConsoleTitleA("xcompat");
    xcompat::log::init();
}

static void spawn_guest(const std::filesystem::path& canon_path, std::vector<std::string> args) {
    // initialize guest process
    xcompat::guest guest{};
    xcompat::log::info("Guest PID: 0x{:X}", GetProcessId(guest.proc));

    // load portable executable in process
    auto& title = guest.load_image(canon_path);
    xcompat::log::info("Title module: 0x{:08X} (size: 0x{:X})", title.base.i, title.size);

    // resolve system imports
    /*if (!guest.resolve_imports(title)) {
        xcompat::log::fatal("Failed to resolve title imports");
        return;
    }

    xcompat::log::info("Resolved {} modules", guest.module_count());*/
}

int main(const int argc, char* argv[]) {
    init();

    // xcompat.exe <exe_path> <args...>
    if (argc < 2) return usage();
    
    // extract arguments
    const auto title_path_rel = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    // verify executable path
    if (!std::filesystem::exists(title_path_rel)) {
        std::cerr << "invalid executable path provided\n";
        return usage();
    }

    // get canonical path
    const auto title_path = std::filesystem::canonical(title_path_rel);
    xcompat::log::debug("Title path: {}", title_path.string());

    // start compatibility layer w/ proc
    spawn_guest(title_path, args);

    std::cin.get();
    return 0;
}
