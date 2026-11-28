#include "xcompat/module.hpp"

#include <utility>

namespace xcompat {
    module::module(std::filesystem::path path, std::string key, hy::mod&& mapped,
                   detail::execution_metadata execution,
                   std::vector<detail::delay_import_descriptor> delay_imports)
        : hy::mod(std::move(mapped)), path_(std::move(path)), key_(std::move(key)),
          execution_(execution), delay_imports_(std::move(delay_imports)) {}

    const std::filesystem::path& module::path() const noexcept { return path_; }
    std::string_view module::key() const noexcept { return key_; }
}