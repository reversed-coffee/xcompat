#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace xcompat::detail {
    struct import_thunk {
        std::optional<std::string> name;
        std::optional<std::uint16_t> ordinal;
        std::uint32_t iat_rva{};
    };

    struct import_descriptor {
        std::string module;
        std::vector<import_thunk> thunks;
    };

    struct delay_import_descriptor {
        std::string module;
        std::uint32_t module_handle_rva{};
        std::vector<import_thunk> thunks;
    };

    struct export_record {
        std::optional<std::string> name;
        std::uint32_t ordinal{};
        std::uint32_t function_rva{};
        std::optional<std::string> forwarder;
    };

    struct section_record {
        std::string name;
        std::uint32_t virtual_address{};
        std::uint32_t virtual_size{};
        std::uint32_t raw_offset{};
        std::uint32_t raw_size{};
    };

    struct tls_record {
        bool present{};
        std::uint32_t template_rva{};
        std::uint32_t template_size{};
        std::uint32_t zero_fill{};
        std::uint32_t index_rva{};
        std::uint32_t alignment{};
        std::vector<std::uint32_t> callback_rvas;
    };

    struct exception_record {
        std::uint32_t rva{};
        std::uint32_t count{};
    };

    class mapped_file {
        HANDLE file_{INVALID_HANDLE_VALUE};
        HANDLE mapping_{};
        const std::byte* data_{};
        std::size_t size_{};

    public:
        explicit mapped_file(const std::filesystem::path& path);
        ~mapped_file();

        mapped_file(const mapped_file&) = delete;
        mapped_file& operator=(const mapped_file&) = delete;
        mapped_file(mapped_file&& other) noexcept;
        mapped_file& operator=(mapped_file&& other) noexcept;

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
            return { data_, size_ };
        }
    };

    class pe_image {
        std::filesystem::path path_;
        std::unique_ptr<mapped_file> mapped_file_;
        std::span<const std::byte> bytes_;
        std::vector<section_record> sections_;
        std::uint32_t headers_size_{};

        [[noreturn]] void fail(std::string_view reason) const;
        [[nodiscard]] std::size_t rva_offset(std::uint32_t rva, std::size_t size) const;
        [[nodiscard]] std::size_t containing_limit(std::size_t offset) const;
        [[nodiscard]] std::string string_at_rva(std::uint32_t rva) const;
        [[nodiscard]] std::uint32_t va_to_rva(std::uint64_t va, std::string_view field) const;
        void parse();

    public:
        explicit pe_image(const std::filesystem::path& path, std::span<const std::byte> bytes);
        explicit pe_image(const std::filesystem::path& path);

        std::uint64_t preferred_base{};
        std::uint32_t image_size{};
        std::uint32_t entry_rva{};
        std::uint64_t stack_reserve{};
        std::uint64_t stack_commit{};
        bool is_dll{};
        exception_record exceptions;
        tls_record tls;
        std::vector<import_descriptor> imports;
        std::vector<delay_import_descriptor> delay_imports;
        std::vector<export_record> exports;

        [[nodiscard]] const std::vector<section_record>& sections() const noexcept {
            return sections_;
        }

        [[nodiscard]] std::size_t file_offset(std::uint32_t rva, std::size_t size = 1) const {
            return rva_offset(rva, size);
        }
    };
}
