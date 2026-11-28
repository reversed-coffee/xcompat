#pragma once

#include <hydra/proc.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace xcompat::loader {
    [[nodiscard]] std::string module_key(std::string_view module_name);

    std::filesystem::path resolve_literal_import(
        std::string_view module_name,
        const std::filesystem::path& root,
        const std::vector<std::filesystem::path>& supplemental_paths
    );

    inline bool is_api_set(const std::string_view name) {
        return name.size() >= 4
            && (_strnicmp(name.data(), "api-", 4) == 0
            || _strnicmp(name.data(), "ext-", 4) == 0);
    }

    std::string resolve_api_set(
        const std::string& module_name,
        const std::string& importer = {}
    );

    hy::mod map_img_file_to_proc(
        hy::proc& proc,
        const std::filesystem::path& path
    );

    bool write_remote(
        hy::proc& proc,
        hy::ptr address,
        std::span<const std::byte> bytes
    );
}
