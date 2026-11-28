#include "xcompat/guest.hpp"
#include "xcompat/loader.hpp"

#include <hydra/nt/hnd.hpp>

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

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
        const auto length = GetModuleFileNameW(nullptr, path.data(),
            static_cast<DWORD>(path.size()));
        if (!length || length == path.size())
            throw std::runtime_error("cannot resolve harness path");
        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    xcompat::guest_options fixture_options(const std::filesystem::path& fixture) {
        return {
            .arguments = { L"arg with spaces", L"quote\"inside", L"trailing\\",
                L"", L"after-empty" },
            .working_directory = fixture.parent_path(),
            .import_paths = { fixture.parent_path(), harness_directory() }
        };
    }

    template <typename Callback>
    bool throws_logic(Callback&& callback, const std::string_view expected) {
        try {
            callback();
        } catch (const std::logic_error& error) {
            return error.what() == expected;
        }
        return false;
    }

    void print_report(const xcompat::link_report& report) {
        for (const auto& issue : report.unresolved) {
            std::cerr << issue.importer.string() << ": " << issue.module << '!';
            if (issue.name) std::cerr << *issue.name;
            else if (issue.ordinal) std::cerr << '#' << *issue.ordinal;
            std::cerr << " kind=" << static_cast<int>(issue.kind)
                      << " error=" << static_cast<int>(issue.error) << '\n';
        }
    }

    bool run_fixture(const std::filesystem::path& fixture,
                     std::optional<xcompat::guest_options> options = std::nullopt) {
        xcompat::guest guest(options ? std::move(*options) : fixture_options(fixture));
        guest.load_image(fixture);
        const auto report = guest.link();
        if (!report.complete()) print_report(report);
        return report.complete() && guest.run() == 0;
    }

    std::uintptr_t find_import_slot(xcompat::module& image,
                                    const std::string_view module_name,
                                    const std::string_view symbol_name) {
        hy::err error = hy::STA_SUCCESS;
        std::uintptr_t result{};
        for (const auto& imported : image.imports(&error)) {
            if (imported.module == module_name && imported.name == symbol_name)
                result = imported.base.i;
        }
        if (error != hy::STA_SUCCESS)
            throw std::runtime_error("failed to enumerate mapped test imports");
        if (!result)
            throw std::runtime_error("expected mapped import was not found");
        return result;
    }

    std::uintptr_t read_pointer(hy::proc& process, const std::uintptr_t address) {
        std::uintptr_t value{};
        if (process.mm_read(&value, address) != sizeof(value))
            throw std::runtime_error("failed to read mapped import slot");
        return value;
    }

    bool paths_case(const std::filesystem::path& directory) {
        const auto provider = std::filesystem::canonical(directory / L"provider.dll");
        const auto resolved = xcompat::loader::resolve_literal_import(
            provider.string(), directory,
            std::span<const std::filesystem::path>{});
        CHECK(resolved == provider);
        const auto nested_provider = std::filesystem::canonical(
            directory / L"unsearched" / L"absent_provider.dll");
        const auto resolved_nested = xcompat::loader::resolve_literal_import(
            "unsearched/absent_provider.dll", directory,
            std::span<const std::filesystem::path>{});
        CHECK(resolved_nested == nested_provider);
        const auto missing_nested = xcompat::loader::resolve_literal_import(
            "missing/provider.dll", directory,
            std::span<const std::filesystem::path>{});
        CHECK(missing_nested.empty());
        CHECK(xcompat::loader::module_key("missing/provider.dll") ==
            xcompat::loader::module_key("provider.dll"));

        const auto root = directory / L"entry_root.exe";
        xcompat::guest guest(fixture_options(root));
        CHECK(throws_logic([&] { static_cast<void>(guest.process()); },
            "process unavailable before reflection"));
        CHECK(throws_logic([&] { static_cast<void>(guest.link()); },
            "link requires an unlinked loaded guest"));
        CHECK(throws_logic([&] { static_cast<void>(guest.run()); },
            "run requires a linked guest"));

        bool missing_rejected = false;
        try {
            guest.load_image(directory / L"definitely-missing-root.exe");
        } catch (const std::runtime_error&) {
            missing_rejected = true;
        }
        CHECK(missing_rejected);

        guest.load_image(root);
        CHECK(throws_logic([&] { guest.load_image(root); },
            "load_image requires an empty guest"));
        const auto report = guest.link();
        if (!report.complete()) print_report(report);
        CHECK(report.complete());
        CHECK(guest.run() == 0);
        return true;
    }

    bool binding_case(const std::filesystem::path& fixture) {
        xcompat::guest guest(fixture_options(fixture));
        auto& title = guest.load_image(fixture);
        const auto slot = find_import_slot(title, "provider.dll", "NamedValue");

        MEMORY_BASIC_INFORMATION before{};
        SIZE_T before_length{};
        CHECK(NT_SUCCESS(NtQueryVirtualMemory(guest.process(),
            reinterpret_cast<void*>(slot), MemoryBasicInformation, &before,
            sizeof(before), &before_length)));
        CHECK(before_length == sizeof(before));

        const auto report = guest.link();
        if (!report.complete()) print_report(report);
        CHECK(report.complete());

        MEMORY_BASIC_INFORMATION after{};
        SIZE_T after_length{};
        CHECK(NT_SUCCESS(NtQueryVirtualMemory(guest.process(),
            reinterpret_cast<void*>(slot), MemoryBasicInformation, &after,
            sizeof(after), &after_length)));
        CHECK(after_length == sizeof(after));
        CHECK(before.Protect == after.Protect);
        CHECK(throws_logic([&] { static_cast<void>(guest.link()); },
            "link requires an unlinked loaded guest"));
        CHECK(guest.run() == 0);
        return true;
    }

    bool unresolved_case(const std::filesystem::path& directory) {
        struct expected_case {
            const wchar_t* file;
            const char* module;
            const char* symbol;
            xcompat::link_error error;
        };
        const expected_case cases[]{
            { L"missing_provider_root.exe", "absent_provider.dll", "AbsentValue",
                xcompat::link_error::module_not_found },
            { L"forward_cycle_root.exe", "forward_cycle_a.dll", "Cycle",
                xcompat::link_error::forwarder_cycle },
            { L"missing_export_root.exe", "missing_export_forward.dll", "Missing",
                xcompat::link_error::export_not_found }
        };

        for (const auto& expected : cases) {
            hy::nt::hnd observed_process;
            {
                const auto fixture = std::filesystem::canonical(directory / expected.file);
                xcompat::guest guest(fixture_options(fixture));
                auto& title = guest.load_image(fixture);
                observed_process.reset(OpenProcess(SYNCHRONIZE, FALSE,
                    GetProcessId(guest.process())));
                CHECK(observed_process.not_null());

                const auto slot = find_import_slot(title, expected.module, expected.symbol);
                const auto before = read_pointer(guest.process(), slot);
                const auto report = guest.link();
                CHECK(report.unresolved.size() == 1);
                const auto& issue = report.unresolved.front();
                CHECK(issue.importer == fixture);
                CHECK(issue.module == expected.module);
                CHECK(issue.name == expected.symbol);
                CHECK(!issue.ordinal);
                CHECK(issue.kind == xcompat::import_kind::normal);
                CHECK(issue.error == expected.error);
                CHECK(read_pointer(guest.process(), slot) == before);
                CHECK(throws_logic([&] { static_cast<void>(guest.link()); },
                    "link requires an unlinked loaded guest"));
                CHECK(throws_logic([&] { static_cast<void>(guest.run()); },
                    "run requires a linked guest"));
            }
            CHECK(WaitForSingleObject(observed_process, 5000) == WAIT_OBJECT_0);
        }
        return true;
    }

    bool entry_case(const std::filesystem::path& fixture) {
        CHECK(run_fixture(fixture));

        auto options = fixture_options(fixture);
        options.working_directory = fixture.parent_path() /
            std::format(L"missing-working-directory-{}", GetCurrentProcessId()) / L"child";
        CHECK(!std::filesystem::exists(options.working_directory));
        const auto missing_directory = options.working_directory;

        hy::nt::hnd observed_process;
        xcompat::guest guest(std::move(options));
        guest.load_image(fixture);
        const auto report = guest.link();
        if (!report.complete()) print_report(report);
        CHECK(report.complete());
        observed_process.reset(OpenProcess(SYNCHRONIZE, FALSE,
            GetProcessId(guest.process())));
        CHECK(observed_process.not_null());

        CHECK(!SetCurrentDirectoryW(missing_directory.c_str()));
        CHECK(GetLastError() == ERROR_PATH_NOT_FOUND);
        const auto expected_status = RtlGetLastNtStatus();
        CHECK(RtlNtStatusToDosErrorNoTeb(expected_status) == ERROR_PATH_NOT_FOUND);
        const auto expected_message = std::format(
            "guest initialization failed at current_directory: 0x{:08X}",
            static_cast<std::uint32_t>(expected_status));
        bool exact_failure = false;
        try {
            static_cast<void>(guest.run());
        } catch (const std::runtime_error& error) {
            exact_failure = error.what() == expected_message;
        }
        CHECK(exact_failure);
        CHECK(WaitForSingleObject(observed_process, 0) == WAIT_OBJECT_0);
        return true;
    }

    bool stack_case(const std::filesystem::path& fixture) {
        return run_fixture(fixture);
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
        bool result{};
        if (requested == "paths") result = paths_case(fixture);
        else if (requested == "binding") result = binding_case(fixture);
        else if (requested == "unresolved") result = unresolved_case(fixture);
        else if (requested == "entry") result = entry_case(fixture);
        else if (requested == "stack") result = stack_case(fixture);
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
