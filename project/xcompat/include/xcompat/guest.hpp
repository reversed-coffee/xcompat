#pragma once

#include <hydra/proc.hpp>
#include <hydra/img.hpp> // NOLINT

#include <filesystem>

namespace xcompat {
    class guest {
        static hy::proc create_process();

    public:
        hy::proc proc;

        explicit guest() : proc(std::move(create_process())) {}

        [[nodiscard]] hy::mod load_image(const std::filesystem::path& image_path);
    };
}
