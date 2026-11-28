#include "discovery.hpp"

#include "xcompat/loader.hpp"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <format>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {
    std::wstring lowercase_path(const std::filesystem::path& path) {
        auto value = path.lexically_normal().wstring();
        std::ranges::transform(value, value.begin(), [](const wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    }

    bool same_path(const std::filesystem::path& left, const std::filesystem::path& right) {
        return lowercase_path(left) == lowercase_path(right);
    }

    bool below_path(const std::filesystem::path& child, const std::filesystem::path& parent) {
        const auto child_value = lowercase_path(child);
        auto parent_value = lowercase_path(parent);
        if (!parent_value.ends_with(L'\\')) parent_value.push_back(L'\\');
        return child_value.starts_with(parent_value);
    }

    std::filesystem::path canonical_path(const std::filesystem::path& path) {
        std::error_code error;
        auto result = std::filesystem::canonical(path, error);
        if (error)
            throw std::runtime_error(std::format("{}: path canonicalization failed ({})",
                path.string(), error.message()));
        return result;
    }

    void require_enumeration(const std::filesystem::path& path, const hy::err error,
                             const std::string_view subject) {
        if (error != hy::STA_SUCCESS)
            throw std::runtime_error(std::format("{}: failed to enumerate PE {}",
                path.string(), subject));
    }

    std::optional<xcompat::detail::symbol_reference> parse_forwarded_symbol(
        const std::string_view value
    ) {
        if (value.empty()) return std::nullopt;
        if (!value.starts_with('#'))
            return xcompat::detail::symbol_reference{ .name = std::string(value) };

        std::uint32_t ordinal{};
        const auto [end, error] = std::from_chars(
            value.data() + 1, value.data() + value.size(), ordinal);
        if (error != std::errc{} || end != value.data() + value.size() ||
            ordinal > std::numeric_limits<std::uint16_t>::max())
            return std::nullopt;
        return xcompat::detail::symbol_reference{
            .ordinal = static_cast<std::uint16_t>(ordinal) };
    }
}

namespace xcompat::detail {
    resolved_import_target resolve_import_target(
        const std::string_view raw_name,
        const std::filesystem::path& importer_path,
        const std::string_view importer_identity,
        const std::span<const std::filesystem::path> import_paths
    ) {
        std::string resolved_name(raw_name);
        if (loader::is_api_set(raw_name)) {
            const auto host = loader::resolve_api_set(
                std::string(raw_name), std::string(importer_identity));
            if (!host.empty()) resolved_name = host;
        }

        auto path = loader::resolve_literal_import(
            resolved_name, importer_path.parent_path(), import_paths);
        const auto key = loader::module_key(
            path.empty() ? resolved_name : path.filename().string());
        return { std::move(resolved_name), std::move(path), key };
    }

    discovery_result discover_images(
        const std::filesystem::path& root,
        const std::span<const std::filesystem::path> import_paths
    ) {
        struct registry_entry {
            std::filesystem::path path;
            bool native{};
            std::size_t index{};
        };

        discovery_result result;
        std::unordered_map<std::string, registry_entry> registry;

        wchar_t windows_buffer[MAX_PATH]{};
        const auto windows_length = GetWindowsDirectoryW(windows_buffer, MAX_PATH);
        if (!windows_length || windows_length >= MAX_PATH)
            throw std::runtime_error("cannot resolve Windows directory");
        const auto windows_directory = canonical_path(windows_buffer);

        const auto add_native_symbol = [&](const std::size_t index,
                                           const symbol_reference& symbol) {
            auto& symbols = result.native_images[index].symbols;
            if (std::ranges::none_of(symbols, [&](const symbol_reference& existing) {
                return existing.name == symbol.name && existing.ordinal == symbol.ordinal;
            })) {
                symbols.push_back(symbol);
            }
        };

        std::function<registry_entry(const std::filesystem::path&, bool)> register_image;
        register_image = [&](const std::filesystem::path& input_path,
                             const bool root_image) -> registry_entry {
            const auto path = canonical_path(input_path);
            const auto key = loader::module_key(path.filename().string());
            if (const auto found = registry.find(key); found != registry.end()) {
                if (!same_path(found->second.path, path))
                    throw std::runtime_error("module name collision: " + key);
                return found->second;
            }

            const auto native = !root_image && below_path(path, windows_directory);
            if (native) {
                const auto index = result.native_images.size();
                result.native_images.push_back({ path, key, {} });
                auto [entry, inserted] = registry.emplace(
                    key, registry_entry{ path, true, index });
                return entry->second;
            }

            const auto index = result.manual_images.size();
            result.manual_images.push_back({ path, key, image_file(path) });
            auto [entry, inserted] = registry.emplace(
                key, registry_entry{ path, false, index });
            const auto registered = entry->second;

            const auto discover_dependency = [&](const std::string_view raw_name,
                                                  const std::optional<symbol_reference>& symbol) {
                const auto target = resolve_import_target(
                    raw_name, path, key, import_paths);
                if (target.path.empty()) return;
                const auto dependency = register_image(target.path, false);
                if (dependency.native && symbol)
                    add_native_symbol(dependency.index, *symbol);
            };

            auto& image = result.manual_images[index].image;
            hy::err import_error = hy::STA_SUCCESS;
            for (const auto& imported : image.image().imports(&import_error)) {
                discover_dependency(imported.module,
                    symbol_reference{ imported.name, imported.ordinal });
            }
            require_enumeration(path, import_error, "imports");

            for (const auto& descriptor : image.delay_imports()) {
                for (const auto& thunk : descriptor.thunks) {
                    discover_dependency(descriptor.module,
                        symbol_reference{ thunk.name, thunk.ordinal });
                }
            }

            hy::err export_error = hy::STA_SUCCESS;
            for (const auto& exported : image.image().exports(&export_error)) {
                if (!exported.forwarder) continue;
                const auto separator = exported.forwarder->rfind('.');
                if (separator == std::string::npos || !separator) continue;
                const auto module_name = std::string_view(*exported.forwarder).substr(0, separator);
                const auto symbol_name = std::string_view(*exported.forwarder).substr(separator + 1);
                discover_dependency(module_name, parse_forwarded_symbol(symbol_name));
            }
            require_enumeration(path, export_error, "exports");
            return registered;
        };

        register_image(root, true);
        return result;
    }
}
