#pragma once

#include "discovery.hpp"
#include "xcompat/guest.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace xcompat::detail {
    class linker {
        struct loaded_native {
            std::filesystem::path path;
            std::string key;
            HMODULE handle{};
            std::unordered_map<std::string, std::uintptr_t> exports;
        };

        std::vector<std::filesystem::path> import_paths_;
        std::vector<loaded_native> native_modules_;
        std::unordered_map<std::string, std::size_t> native_by_key_;

    public:
        explicit linker(std::span<const std::filesystem::path> import_paths);
        ~linker();

        linker(const linker&) = delete;
        linker& operator=(const linker&) = delete;
        linker(linker&&) = delete;
        linker& operator=(linker&&) = delete;

        void preload_native_modules(std::span<const native_image_record> images);
        [[nodiscard]] link_report bind(hy::proc& process, std::span<module> modules);
    };
}
