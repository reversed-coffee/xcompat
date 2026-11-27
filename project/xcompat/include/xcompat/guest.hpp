#pragma once

#include <hydra/proc.hpp>
#include <hydra/img.hpp>

#include <filesystem>

namespace xcompat {
    struct section {
        hy::nt::hnd handle;
        hy::blk block;
    };

    class guest {
        static hy::proc create_process();

    public:
        hy::proc proc;

        explicit guest() : proc(std::move(create_process())) {}

        [[nodiscard]] hy::img&& load_image(const std::filesystem::path& image_path);
    };
}
