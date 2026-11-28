#include <iostream>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "xcompat/log.hpp"
#include "xcompat/guest.hpp"

static int usage() {
    std::wcerr << L"usage: xcompat <exe_path> <args...>\n";
    return 1;
}

static void init() {
    SetConsoleTitleW(L"xcompat");

    xcompat::log::init();
    xcompat::log::set_level(xcompat::log::level::debug);
}

static std::filesystem::path runtime_directory() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length == buffer.size())
        throw std::runtime_error(std::format("cannot resolve CLI path ({})", GetLastError()));
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path() / L"runtime";
}

static std::string_view reason(const xcompat::link_error error) {
    using enum xcompat::link_error;
    switch (error) {
    case module_not_found: return "module not found";
    case export_not_found: return "export not found";
    case malformed_forwarder: return "malformed forwarder";
    case forwarder_cycle: return "forwarder cycle";
    case forwarder_depth: return "forwarder depth exceeded";
    case iat_write_failed: return "IAT write failed";
    }
    return "unknown";
}

static int spawn_guest(const std::filesystem::path& path, std::vector<std::wstring> arguments) {
    xcompat::guest guest({
        .arguments = std::move(arguments),
        .working_directory = path.parent_path(),
        .import_paths = { runtime_directory() }
    });

    auto& title = guest.load_image(path);
    xcompat::log::info("Guest PID: 0x{:X}", GetProcessId(guest.process()));
    xcompat::log::info("Title module: 0x{:08X} (size: 0x{:X})", title.base().i, title.size());

    const auto report = guest.link();
    if (!report.complete()) {
        for (const auto& issue : report.unresolved) {
            const auto symbol = issue.name ? *issue.name : std::format("#{}", *issue.ordinal);
            xcompat::log::error("{}: unresolved {} import {}!{} ({})",
                issue.importer.string(),
                issue.kind == xcompat::import_kind::normal ? "normal" : "delay",
                issue.module, symbol, reason(issue.error));
        }
        return 1;
    }

    guest.initialize();
    return static_cast<int>(guest.run());
}

int wmain(const int argc, wchar_t* argv[]) {
    init();
    try {
        if (argc < 2) return usage();
        if (!std::filesystem::is_regular_file(argv[1])) {
            std::wcerr << L"invalid executable path provided\n";
            return usage();
        }
        const auto title_path = std::filesystem::canonical(argv[1]);
        xcompat::log::debug("Title path: {}", title_path.string());
        const std::vector<std::wstring> arguments(argv + 2, argv + argc);
        const auto status = spawn_guest(title_path, arguments);
        xcompat::log::shutdown();
        return status;
    } catch (const std::exception& error) {
        xcompat::log::error("{}", error.what());
        xcompat::log::shutdown();
        return 1;
    }
}
