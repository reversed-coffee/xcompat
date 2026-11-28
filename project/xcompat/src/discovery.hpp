#pragma once

#include "image_file.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xcompat::detail {
    struct symbol_reference {
        std::optional<std::string> name;
        std::optional<std::uint16_t> ordinal;
    };

    struct manual_image_record {
        std::filesystem::path path;
        std::string key;
        image_file image;
    };

    struct native_image_record {
        std::filesystem::path path;
        std::string key;
        std::vector<symbol_reference> symbols;
    };

    struct discovery_result {
        std::deque<manual_image_record> manual_images;
        std::vector<native_image_record> native_images;
    };

    struct resolved_import_target {
        std::string resolved_name;
        std::filesystem::path path;
        std::string key;
    };

    [[nodiscard]] resolved_import_target resolve_import_target(
        std::string_view raw_name,
        const std::filesystem::path& importer_path,
        std::string_view importer_identity,
        std::span<const std::filesystem::path> import_paths
    );

    [[nodiscard]] discovery_result discover_images(
        const std::filesystem::path& root,
        std::span<const std::filesystem::path> import_paths
    );
}
