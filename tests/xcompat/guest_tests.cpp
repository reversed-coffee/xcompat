#include "xcompat/guest.hpp"
#include "pe.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << __func__ << ':' << __LINE__ << ": check failed: " #condition << '\n'; \
            return false; \
        } \
    } while (false)

    std::filesystem::path harness_directory() {
        std::wstring path(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size()) throw std::runtime_error("cannot resolve harness path");
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    xcompat::guest_options fixture_options(const std::filesystem::path& fixture) {
        return {
            .arguments = { L"arg with spaces", L"quote\"inside", L"trailing\\" },
            .working_directory = fixture.parent_path(),
            .import_paths = { fixture.parent_path(), harness_directory() }
        };
    }

    void print_report(const xcompat::link_report& report) {
        for (const auto& issue : report.unresolved) {
            std::cerr << issue.importer.string() << ": " << issue.module << '!';
            if (issue.name) std::cerr << *issue.name;
            else std::cerr << '#' << *issue.ordinal;
            std::cerr << " error=" << static_cast<int>(issue.error) << '\n';
        }
    }

    bool run_fixture(const std::filesystem::path& fixture) {
        xcompat::guest guest(fixture_options(fixture));
        guest.load_image(fixture);
        const auto report = guest.link();
        if (!report.complete()) {
            print_report(report);
            return false;
        }
        guest.initialize();
        const auto result = guest.run();
        if (result) std::cerr << fixture.filename().string() << " exited " << result << '\n';
        return result == 0;
    }

    std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream) throw std::runtime_error("cannot read fixture");
        const auto size = static_cast<std::size_t>(stream.tellg());
        std::vector<std::byte> bytes(size);
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        return bytes;
    }

    std::size_t raw_rva_offset(std::vector<std::byte>& bytes, const DWORD rva) {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(bytes.data());
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(bytes.data() + dos->e_lfanew);
        auto* section = IMAGE_FIRST_SECTION(nt);
        if (rva < nt->OptionalHeader.SizeOfHeaders) return rva;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (rva >= section[i].VirtualAddress &&
                rva - section[i].VirtualAddress < section[i].SizeOfRawData)
                return section[i].PointerToRawData + rva - section[i].VirtualAddress;
        }
        throw std::runtime_error("fixture RVA is not file backed");
    }

    bool parser_case(const std::filesystem::path& fixture) {
        xcompat::detail::pe_image entry(fixture);
        CHECK(!entry.is_dll);
        CHECK(entry.entry_rva != 0);
        CHECK(entry.image_size != 0);

        const auto directory = fixture.parent_path();
        xcompat::detail::pe_image binding(directory / "binding_root.exe");
        CHECK(!binding.imports.empty());
        CHECK(!binding.delay_imports.empty());
        CHECK(binding.delay_imports.front().module_handle_rva != 0);

        xcompat::detail::pe_image cxx(directory / "cxx_fixture.dll");
        for (const auto& descriptor : cxx.imports) {
            std::string name = descriptor.module;
            std::ranges::transform(name, name.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            CHECK(name.find("140d.dll") == std::string::npos);
            CHECK(name.find("ucrtbased.dll") == std::string::npos);
        }

        auto malformed = read_bytes(directory / "binding_root.exe");
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(malformed.data());
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(malformed.data() + dos->e_lfanew);
        const auto delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        CHECK(delay.VirtualAddress != 0);
        auto* attributes = reinterpret_cast<DWORD*>(malformed.data() + raw_rva_offset(malformed, delay.VirtualAddress));
        *attributes |= 2;
        bool rejected = false;
        try {
            xcompat::detail::pe_image invalid(L"synthetic-unknown-delay.exe", malformed);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        CHECK(rejected);
        return true;
    }

    bool state_case(const std::filesystem::path& fixture) {
        xcompat::guest guest(fixture_options(fixture));
        bool threw = false;
        try { static_cast<void>(guest.process()); } catch (const std::logic_error&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { static_cast<void>(guest.link()); } catch (const std::logic_error&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { guest.initialize(); } catch (const std::logic_error&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { static_cast<void>(guest.run()); } catch (const std::logic_error&) { threw = true; }
        CHECK(threw);

        guest.load_image(fixture);
        threw = false;
        try { guest.load_image(fixture); } catch (const std::logic_error&) { threw = true; }
        CHECK(threw);
        const auto first = guest.link();
        const auto second = guest.link();
        CHECK(first.complete());
        CHECK(second.complete());
        CHECK(first.unresolved.size() == second.unresolved.size());
        return true;
    }

    bool binding_case(const std::filesystem::path& fixture) {
        xcompat::detail::pe_image parsed(fixture);
        CHECK(!parsed.imports.empty());
        xcompat::guest guest(fixture_options(fixture));
        auto& title = guest.load_image(fixture);
        MEMORY_BASIC_INFORMATION before{};
        const auto slot = title.base().i + parsed.imports.front().thunks.front().iat_rva;
        CHECK(VirtualQueryEx(guest.process(), reinterpret_cast<void*>(slot), &before, sizeof(before)) == sizeof(before));
        const auto report = guest.link();
        if (!report.complete()) print_report(report);
        CHECK(report.complete());
        MEMORY_BASIC_INFORMATION after{};
        CHECK(VirtualQueryEx(guest.process(), reinterpret_cast<void*>(slot), &after, sizeof(after)) == sizeof(after));
        CHECK(before.Protect == after.Protect);
        guest.initialize();
        CHECK(guest.run() == 0);
        return true;
    }

    bool unresolved_case(const std::filesystem::path& directory) {
        struct expected_case {
            const wchar_t* file;
            xcompat::link_error error;
        };
        const expected_case cases[]{
            { L"missing_provider_root.exe", xcompat::link_error::module_not_found },
            { L"forward_cycle_root.exe", xcompat::link_error::forwarder_cycle },
            { L"missing_export_root.exe", xcompat::link_error::export_not_found }
        };
        for (const auto& item : cases) {
            const auto path = directory / item.file;
            xcompat::guest guest(fixture_options(path));
            guest.load_image(path);
            const auto report = guest.link();
            CHECK(!report.complete());
            CHECK(std::ranges::any_of(report.unresolved, [&](const auto& issue) {
                return issue.error == item.error;
            }));
        }
        return true;
    }
}

int main(const int argc, const char* const* argv) {
    if (argc != 3) {
        std::cerr << "usage: xcompat-guest-tests <case> <fixture>\n";
        return 2;
    }
    try {
        const std::string_view requested = argv[1];
        const std::filesystem::path fixture = argv[2];
        bool result = false;
        if (requested == "parser") result = parser_case(fixture);
        else if (requested == "state") result = state_case(fixture);
        else if (requested == "binding") result = binding_case(fixture);
        else if (requested == "unresolved") result = unresolved_case(fixture);
        else if (requested == "cycle" || requested == "entry" || requested == "import" ||
                 requested == "constructor" || requested == "tls" || requested == "exception" ||
                 requested == "thread" || requested == "stack" || requested == "xmem" ||
                 requested == "kernel-mmu") result = run_fixture(fixture);
        else {
            std::cerr << "unknown case: " << requested << '\n';
            return 2;
        }
        if (!result) return 1;
        std::cout << requested << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
