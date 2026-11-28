#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <hydra/mod.hpp>

namespace xcompat {
    namespace detail {
        class pe_image;
        struct export_record;
    }

    class guest;

    class module {
        struct impl;
        std::unique_ptr<impl> impl_;

        module(std::filesystem::path path, std::string name, hy::mod&& mapped,
            detail::pe_image&& metadata);
        [[nodiscard]] detail::pe_image& metadata() noexcept;
        [[nodiscard]] const detail::pe_image& metadata() const noexcept;
        [[nodiscard]] const detail::export_record* find_export(std::string_view name) const noexcept;
        [[nodiscard]] const detail::export_record* find_export(std::uint16_t ordinal) const noexcept;

        friend class guest;

    public:
        ~module();
        module(const module&) = delete;
        module& operator=(const module&) = delete;
        module(module&&) = delete;
        module& operator=(module&&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept;
        [[nodiscard]] std::string_view name() const noexcept;
        [[nodiscard]] hy::ptr base() const noexcept;
        [[nodiscard]] std::size_t size() const noexcept;
    };
}
