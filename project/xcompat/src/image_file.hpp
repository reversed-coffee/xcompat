#pragma once

#include "xcompat/module.hpp"

#include <hydra/img.hpp>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xcompat::detail {
    class image_file {
        struct segment_span {
            std::size_t raw_offset{};
            std::size_t raw_size{};
            std::uint32_t mapped_rva{};
            std::size_t mapped_size{};
        };

        std::filesystem::path path_;
        std::vector<std::byte> bytes_;
        hy::img image_;
        execution_metadata execution_;
        std::uint32_t headers_size_{};
        std::vector<segment_span> segments_;
        std::vector<delay_import_descriptor> delay_imports_;

        [[noreturn]] void fail(std::string_view reason) const;
        [[nodiscard]] std::size_t rva_offset(std::uint32_t rva, std::size_t size) const;
        [[nodiscard]] std::size_t containing_limit(std::size_t offset) const;
        [[nodiscard]] std::string string_at_rva(std::uint32_t rva) const;
        [[nodiscard]] std::uint32_t va_to_rva(std::uint64_t va, std::string_view field) const;
        void parse();

    public:
        explicit image_file(const std::filesystem::path& path);

        image_file(const image_file&) = delete;
        image_file& operator=(const image_file&) = delete;
        image_file(image_file&&) noexcept = default;
        image_file& operator=(image_file&&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
        [[nodiscard]] hy::img& image() noexcept { return image_; }
        [[nodiscard]] const execution_metadata& execution() const noexcept { return execution_; }
        [[nodiscard]] const std::vector<delay_import_descriptor>& delay_imports() const noexcept {
            return delay_imports_;
        }
        [[nodiscard]] std::vector<delay_import_descriptor> take_delay_imports() noexcept {
            return std::move(delay_imports_);
        }
    };
}
