#include "xcompat/loader.hpp"

#include <hydra/proc.hpp>
#include <hydra/procstm.hpp>

#include <array>
#include <cwctype>
#include <unordered_set>

namespace {
    std::unordered_set<std::filesystem::path> supplemental_import_directories{};
}

namespace xcompat::loader {
    void add_supplemental_import_path(const std::filesystem::path& path) {
        supplemental_import_directories.insert(path);
    }

    void del_supplemental_import_path(const std::filesystem::path& path) {
        supplemental_import_directories.erase(path);
    }

    std::filesystem::path resolve_literal_import(
        std::string_view module_name,
        const std::filesystem::path& root
    ) {
        if (module_name.empty())
            return {};

        std::filesystem::path name(module_name);
        if (!name.has_extension())
            name += ".dll";

        if (name.is_absolute())
            return std::filesystem::is_regular_file(name)
                ? std::filesystem::canonical(name)
                : std::filesystem::path{};

        if (const auto path = root / name; std::filesystem::is_regular_file(path))
            return std::filesystem::canonical(path);

        static const auto directories = [] {
            char system_dir[MAX_PATH]{};
            char windows_dir[MAX_PATH]{};

            GetSystemDirectoryA(system_dir, MAX_PATH);
            GetWindowsDirectoryA(windows_dir, MAX_PATH);

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

        for (const auto& dir : supplemental_import_directories) {
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

        const hy::nt::hnd file(CreateFileA(
            path.string().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
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
                const auto protection = proc.mm_protect(
                    base, size, hy::mem_mode::read | hy::mem_mode::write
                );

                hy::nt::error::assert(hy::nt::last_status, "failed to unprotect relocation target");

                const auto written = proc.mm_write(base, data, size);

                proc.mm_protect(base, size, protection);
                hy::nt::error::assert(hy::nt::last_status, "failed to restore protection");

                return written == size;
            };

            if (img_mod.reloc(img_mod.base, writer) != hy::STA_SUCCESS)
                throw std::runtime_error("relocation failed");
        }
        else {
            hy::nt::error::assert(map_status, "failed to map image in guest process");
        }

        return img_mod;
    }
}
