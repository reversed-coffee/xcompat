#include "xcompat/module.hpp"
#include "pe.hpp"

#include <limits>
#include <unordered_map>
#include <utility>

namespace xcompat {
    struct module::impl {
        std::filesystem::path path;
        std::string name;
        hy::mod mapped;
        detail::pe_image metadata;
        std::unordered_map<std::string, const detail::export_record*> named_exports;
        std::unordered_map<std::uint16_t, const detail::export_record*> ordinal_exports;

        impl(std::filesystem::path path, std::string name, hy::mod&& mapped,
             detail::pe_image&& metadata)
            : path(std::move(path)), name(std::move(name)), mapped(std::move(mapped)),
              metadata(std::move(metadata)) {
            for (const auto& item : this->metadata.exports) {
                if (item.name) named_exports.emplace(*item.name, &item);
                if (item.ordinal <= std::numeric_limits<std::uint16_t>::max())
                    ordinal_exports.emplace(static_cast<std::uint16_t>(item.ordinal), &item);
}
}
    };

    module::module(std::filesystem::path path, std::string name, hy::mod&& mapped,
                   detail::pe_image&& metadata)
        : impl_(std::make_unique<impl>(std::move(path), std::move(name),
              std::move(mapped), std::move(metadata))) {}

    module::~module() = default;

    detail::pe_image& module::metadata() noexcept { return impl_->metadata; }
    const detail::pe_image& module::metadata() const noexcept { return impl_->metadata; }

    const detail::export_record* module::find_export(const std::string_view name) const noexcept {
        const auto found = impl_->named_exports.find(std::string(name));
        return found == impl_->named_exports.end() ? nullptr : found->second;
}

    const detail::export_record* module::find_export(const std::uint16_t ordinal) const noexcept {
        const auto found = impl_->ordinal_exports.find(ordinal);
        return found == impl_->ordinal_exports.end() ? nullptr : found->second;
}

    const std::filesystem::path& module::path() const noexcept { return impl_->path; }
    std::string_view module::name() const noexcept { return impl_->name; }
    hy::ptr module::base() const noexcept { return impl_->mapped.base; }
    std::size_t module::size() const noexcept { return impl_->mapped.size; }
}