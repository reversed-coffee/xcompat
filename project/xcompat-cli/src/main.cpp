#include <iostream>
#include <filesystem>
#include <string>

#include "xcompat/log.hpp"
#include "xcompat/guest.hpp"
#include "xcompat/loader.hpp"

static int usage() {
    std::cerr << "usage: xcompat <exe_path> <args...>\n";
    return 1;
}

static void init() {
    SetConsoleTitleA("xcompat");

    xcompat::log::init();
    xcompat::log::set_level(xcompat::log::level::debug);

    xcompat::loader::add_supplemental_import_path("runtime");
}

static int spawn_guest(const std::filesystem::path& canon_path, std::vector<std::string> args) {
    // initialize guest process
    xcompat::guest guest{};
    xcompat::log::info("Guest PID: 0x{:X}", GetProcessId(guest.proc));

    // load portable executable in process
    xcompat::load_ctx title_load;

    auto& title = guest.load_image(canon_path, &title_load);
    xcompat::log::info("Title module: 0x{:08X} (size: 0x{:X})", title.base.i, title.size);

    if (title_load.incomplete) {
        xcompat::log::error("Unable to resolve {} imports", title_load.missing_modules.size());

        std::stringstream list_stm{};
        for (auto& import : title_load.missing_modules)
            list_stm << import << ", ";

        auto list_str = list_stm.str();
        if (list_str.length() >= 2)
            list_str.erase(list_str.length() - 2);

        xcompat::log::warn("Missing modules: {}", list_str);
        return 1;
    }

    return 0;
}

int main(const int argc, char* argv[]) {
    init();

    // xcompat.exe <exe_path> <args...>
    if (argc < 2) return usage();
    
    // extract arguments
    const auto title_path_rel = argv[1];
    const std::vector<std::string> args(argv + 2, argv + argc);

    // verify executable path
    if (!std::filesystem::exists(title_path_rel)) {
        std::cerr << "invalid executable path provided\n";
        return usage();
    }

    // get canonical path
    const auto title_path = std::filesystem::canonical(title_path_rel);
    xcompat::log::debug("Title path: {}", title_path.string());

    // start compatibility layer w/ proc
    const auto status = spawn_guest(title_path, args);

    std::cin.get();
    return status;
}
