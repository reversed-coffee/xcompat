#pragma once

#include <hydra/proc.hpp>

#include <filesystem>

namespace xcompat::loader {
    hy::mod map_img_file_to_proc(hy::proc& proc, const std::filesystem::path& path);
}
