#pragma once

#include <hydra/proc.hpp>

#include <filesystem>

namespace xcompat::loader {
    void add_supplemental_import_path(const std::filesystem::path& path);
    void del_supplemental_import_path(const std::filesystem::path& path);

    std::filesystem::path resolve_literal_import(
        std::string_view module_name,
        const std::filesystem::path& root
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
}
