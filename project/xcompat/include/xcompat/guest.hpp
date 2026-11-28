#pragma once

#include "xcompat/module.hpp"

#include <hydra/proc.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace xcompat {
    namespace detail {
        class linker;
        class guest_process;
    }

    enum class import_kind { normal, delay };

    enum class link_error {
        module_not_found,
        export_not_found,
        malformed_forwarder,
        forwarder_cycle,
        forwarder_depth,
        iat_write_failed
    };

    struct unresolved_import {
        std::filesystem::path importer;
        std::string module;
        std::optional<std::string> name;
        std::optional<std::uint16_t> ordinal;
        import_kind kind;
        link_error error;
    };

    struct link_report {
        std::vector<unresolved_import> unresolved;
        [[nodiscard]] bool complete() const noexcept { return unresolved.empty(); }
    };

    struct guest_options {
        std::vector<std::wstring> arguments;
        std::filesystem::path working_directory;
        std::vector<std::filesystem::path> import_paths;
    };

    class guest {
        enum class state { empty, loaded, linked, exited, failed };

        guest_options options_;
        std::vector<module> modules_;
        std::unique_ptr<detail::linker> linker_;
        std::unique_ptr<detail::guest_process> process_;
        state state_{state::empty};
        bool link_attempted_{};

    public:
        explicit guest(guest_options options = {});
        ~guest();

        guest(const guest&) = delete;
        guest& operator=(const guest&) = delete;
        guest(guest&&) = delete;
        guest& operator=(guest&&) = delete;

        module& load_image(const std::filesystem::path& image_path);
        link_report link();
        DWORD run();

        hy::proc& process();
    };
}
