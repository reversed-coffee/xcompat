#pragma once

#include <hydra/mod.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xcompat {
    namespace detail {
        class linker;
        class guest_process;

        struct execution_metadata {
            std::uint64_t preferred_base{};
            std::uint32_t image_size{};
            std::uint32_t entry_rva{};
            std::uint64_t stack_reserve{};
            std::uint64_t stack_commit{};
        };

        struct delay_import_thunk {
            std::optional<std::string> name;
            std::optional<std::uint16_t> ordinal;
            std::uint32_t iat_rva{};
        };

        struct delay_import_descriptor {
            std::string module;
            std::uint32_t module_handle_rva{};
            std::vector<delay_import_thunk> thunks;
        };
    }

    class guest;

    class module : public hy::mod {
        std::filesystem::path path_;
        std::string key_;
        detail::execution_metadata execution_;
        std::vector<detail::delay_import_descriptor> delay_imports_;

        module(std::filesystem::path path, std::string key, hy::mod&& mapped,
            detail::execution_metadata execution,
            std::vector<detail::delay_import_descriptor> delay_imports);

        friend class guest;
        friend class detail::linker;
        friend class detail::guest_process;

    public:
        module(const module&) = delete;
        module& operator=(const module&) = delete;
        module(module&&) noexcept = default;
        module& operator=(module&&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept;
        [[nodiscard]] std::string_view key() const noexcept;
    };
}
