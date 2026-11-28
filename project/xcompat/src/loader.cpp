#include "xcompat/loader.hpp"

#include <hydra/proc.hpp>
#include <hydra/procstm.hpp>

#include <array>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <utility>

namespace xcompat::loader {
    std::string module_key(const std::string_view module_name) {
        auto key = std::filesystem::path(module_name).filename().string();
        if (!std::filesystem::path(key).has_extension()) key += ".dll";
        std::ranges::transform(key, key.begin(), [](const unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        return key;
    }

    std::filesystem::path resolve_literal_import(
        std::string_view module_name,
        const std::filesystem::path& root,
        const std::span<const std::filesystem::path> supplemental_paths
    ) {
        if (module_name.empty())
            return {};

        std::filesystem::path requested(module_name);
        const auto absolute = requested.is_absolute();
        if (!requested.has_extension()) requested += ".dll";
        if (absolute)
            return std::filesystem::is_regular_file(requested)
                ? std::filesystem::canonical(requested)
                : std::filesystem::path{};

        const auto& name = requested;

        if (const auto path = root / name; std::filesystem::is_regular_file(path))
            return std::filesystem::canonical(path);

        static const auto directories = [] {
            wchar_t system_dir[MAX_PATH]{};
            wchar_t windows_dir[MAX_PATH]{};

            GetSystemDirectoryW(system_dir, MAX_PATH);
            GetWindowsDirectoryW(windows_dir, MAX_PATH);

            return std::array{
                std::filesystem::path(system_dir),
                std::filesystem::path(windows_dir)
            };
        }();

        for (const auto& dir : directories) {
            if (dir.empty())
                continue;

            const auto path = dir / name;
            if (std::filesystem::is_regular_file(path))
                return std::filesystem::canonical(path);
        }

        for (const auto& dir : supplemental_paths) {
            if (dir.empty())
                continue;

            const auto path = dir / name;
            if (std::filesystem::is_regular_file(path))
                return std::filesystem::canonical(path);
        }

        return {};
    }

    std::string resolve_api_set(
        const std::string& module_name,
        const std::string& importer
    ) {
        const auto map = static_cast<const API_SET_NAMESPACE*>(
            NtCurrentPeb()->ApiSetMap
            );

        if (!map || map->Version != 6)
            return {};

        auto name = std::filesystem::path(module_name).stem().wstring();
        const auto parent = std::filesystem::path(importer).filename().wstring();

        const auto end = name.rfind(L'-');
        if (end == std::wstring::npos)
            return {};

        name.resize(end);

        ULONG hash = 0;
        for (auto& ch : name) {
            ch = static_cast<wchar_t>(std::towlower(ch));
            hash = hash * map->HashFactor + ch;
        }

        const auto base = reinterpret_cast<const std::byte*>(map);

        const auto entries = reinterpret_cast<const API_SET_NAMESPACE_ENTRY*>(
            base + map->EntryOffset
        );

        const auto hashes = reinterpret_cast<const API_SET_HASH_ENTRY*>(
            base + map->HashOffset
        );

        auto it = std::lower_bound(
            hashes, hashes + map->Count, hash,
            [](const API_SET_HASH_ENTRY& entry, ULONG hash) {
                return entry.Hash < hash;
            }
        );

        for (; it != hashes + map->Count && it->Hash == hash; ++it) {
            const auto& entry = entries[it->Index];

            const std::wstring_view contract(
                reinterpret_cast<const wchar_t*>(base + entry.NameOffset),
                entry.HashedLength / sizeof(wchar_t)
            );

            if (name != contract)
                continue;

            const auto values = reinterpret_cast<const API_SET_VALUE_ENTRY*>(
                base + entry.ValueOffset
            );

            const API_SET_VALUE_ENTRY* selected = nullptr;

            for (ULONG i = 0; i < entry.ValueCount; ++i) {
                const auto& value = values[i];

                if (!value.NameLength) {
                    selected = &value;
                    continue;
                }

                const auto alias = reinterpret_cast<const wchar_t*>(
                    base + value.NameOffset
                );

                if (parent.size() == value.NameLength / sizeof(wchar_t) &&
                    _wcsnicmp(parent.c_str(), alias, parent.size()) == 0) {
                    selected = &value;
                    break;
                }
            }

            if (!selected || !selected->ValueLength)
                return {};

            const std::wstring_view host(
                reinterpret_cast<const wchar_t*>(base + selected->ValueOffset),
                selected->ValueLength / sizeof(wchar_t)
            );

            return std::string(host.begin(), host.end());
        }

        return {};
    }

    hy::mod map_img_file_to_proc(
        hy::proc& proc,
        const std::filesystem::path& path
    ) {
        auto name = path.filename().string();

        hy::nt::hnd sec_hnd;
        hy::blk sec_blk{ nullptr, 0 };

        const hy::nt::hnd file(CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr
        ));

        if (file == INVALID_HANDLE_VALUE)
            throw hy::nt::win32_error("failed to open image file");

        hy::nt::error::assert(NtCreateSection(
            &sec_hnd, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_READONLY, SEC_IMAGE,
            file
        ), "failed to create image section");

        const auto map_status = NtMapViewOfSection(
            sec_hnd, proc, &sec_blk.base.u, 0, 0, nullptr, &sec_blk.size, ViewUnmap,
            0, PAGE_READONLY
        );

        if (!NT_SUCCESS(map_status) && map_status != STATUS_IMAGE_NOT_AT_BASE)
            throw hy::nt::error(map_status, "failed to map section to virtual memory");

        hy::mod img_mod(proc, std::move(sec_hnd), sec_blk, name);
        if (img_mod.parse() != hy::STA_SUCCESS)
            throw std::runtime_error("failed to parse pe");

        if (map_status == STATUS_IMAGE_NOT_AT_BASE) {
            const auto writer = [&](hy::ptr base, const void* data, std::size_t size) {
                return write_remote(proc, base, {
                    static_cast<const std::byte*>(data), size
                });
            };

            if (img_mod.reloc(img_mod.base, writer) != hy::STA_SUCCESS)
                throw std::runtime_error(path.string() + ": relocation failed");
        }
        else {
            hy::nt::error::assert(map_status, "failed to map image in guest process");
        }

        return img_mod;
    }

    bool write_remote(
        hy::proc& proc,
        const hy::ptr address,
        const std::span<const std::byte> bytes
    ) {
        if (bytes.empty()) return true;

        struct protection_guard {
            HANDLE process;
            PVOID& base;
            SIZE_T& size;
            ULONG protection;
            NTSTATUS& status;

            ~protection_guard() {
                ULONG ignored{};
                status = NtProtectVirtualMemory(
                    process, &base, &size, protection, &ignored);
            }
        };

        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const auto page_size = static_cast<std::uintptr_t>(system_info.dwPageSize);
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto current = address.i + offset;
            const auto page_remaining = page_size - current % page_size;
            const auto chunk = std::min<std::size_t>(
                bytes.size() - offset, page_remaining);

            PVOID protected_base = reinterpret_cast<void*>(current);
            SIZE_T protected_size = chunk;
            ULONG old_protection{};
            const auto status = NtProtectVirtualMemory(
                proc, &protected_base, &protected_size, PAGE_READWRITE, &old_protection);
            if (!NT_SUCCESS(status)) return false;

            NTSTATUS restore_status = STATUS_UNSUCCESSFUL;
            std::size_t written{};
            {
                const protection_guard restore{
                    proc, protected_base, protected_size, old_protection, restore_status };
                written = proc.mm_write(current, bytes.data() + offset, chunk);
            }
            if (written != chunk || !NT_SUCCESS(restore_status)) return false;
            offset += chunk;
        }
        return true;
    }
}
