#pragma once

#include <hydra/proc.hpp>
#include <hydra/img.hpp> // NOLINT

#include <filesystem>

namespace xcompat {
    class guest {
        static hy::proc create_process();

    public:
        hy::proc proc;
        std::unordered_map<std::string, hy::mod> modules;

        explicit guest() : proc(std::move(create_process())) {}

        hy::mod& load_image(const std::filesystem::path& image_path);
    };
}
