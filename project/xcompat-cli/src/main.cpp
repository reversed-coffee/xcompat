#include <iostream>
#include <filesystem>
#include <string>

#include "xcompat/guest.hpp"

int main(int argc, char* argv[]) {
    xcompat::guest guest{};
    std::cout << "Guest PID: " << GetProcessId(guest.proc) << "\n";

    auto section = guest.load_image(R"(C:\Users\nick\Documents\6StarTitle\game_scarlett_master.exe)");

    std::cin.get();

    return 0;
}
