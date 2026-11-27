#pragma once

#include <hydra/proc.hpp>
#include <hydra/mod.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xcompat {
    struct section {
        hy::nt::hnd handle;
        hy::blk block;
    };

    struct export_symbol {
        hy::ptr address;
        std::optional<std::string> forwarder;
    };

    enum class module_state : std::uint8_t {
        mapped,
        resolving,
        resolved,
        failed
    };

    struct module {
        std::filesystem::path path;
        section mapping;
        hy::mod image;
        std::vector<hy::seg> segments;
        std::unordered_map<std::uint32_t, export_symbol> exports;
        std::unordered_map<std::string, std::uint32_t> export_names;
        module_state state{ module_state::mapped };
    };

    class guest {
        static hy::proc create_process();
        static std::string normalize_module_name(std::string_view name);
        static std::optional<std::string> resolve_api_set(std::string_view name);

        std::filesystem::path title_directory;
        std::unordered_map<std::string, module> modules;
        std::unordered_set<std::string> unavailable_modules;

        [[nodiscard]] module* find_module(std::string_view name);
        [[nodiscard]] std::optional<std::filesystem::path> locate_image(
            const module& importer,
            std::string_view name
        ) const;

        [[nodiscard]] module* load_dependency(module& importer, std::string_view name);
        [[nodiscard]] std::optional<hy::ptr> resolve_export(
            module& source,
            const std::optional<std::string>& name,
            const std::optional<std::uint32_t>& ordinal,
            bool& dependencies_resolved,
            std::unordered_set<std::string>& chain
        );

        static void index_exports(module& loaded);

    public:
        hy::proc proc;

        explicit guest() : proc(std::move(create_process())) {}

        [[nodiscard]] module& load_image(const std::filesystem::path& image_path);
        [[nodiscard]] bool resolve_imports(module& loaded);
        [[nodiscard]] std::size_t module_count() const { return modules.size(); }
    };
}
