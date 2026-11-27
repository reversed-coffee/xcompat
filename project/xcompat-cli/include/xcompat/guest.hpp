#pragma once

#include <hydra/proc.hpp>
#include <hydra/mod.hpp>

#include <filesystem>

namespace xcompat {
    struct section {
        hy::nt::hnd handle;
        hy::blk block;
    };

    struct module {
        section mapping;
        hy::mod image;
    };

    class guest {
        static hy::proc create_process();

    public:
        hy::proc proc;
        std::unordered_map<std::string, std::unique_ptr<module>> modules;

        explicit guest() : proc(std::move(create_process())) {}

        [[nodiscard]] section load_image(const std::filesystem::path& image_path);
    };
}
