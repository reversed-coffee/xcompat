#include "image_file.hpp"

#include <hydra/blkstm.hpp>

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
    struct delay_descriptor_raw {
        std::uint32_t attributes;
        std::uint32_t name;
        std::uint32_t module_handle;
        std::uint32_t iat;
        std::uint32_t int_table;
        std::uint32_t bound_iat;
        std::uint32_t unload_iat;
        std::uint32_t timestamp;
    };

    bool is_zero(const delay_descriptor_raw& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        return std::all_of(bytes, bytes + sizeof(value), [](const std::byte byte) {
            return byte == std::byte{};
        });
    }

    std::vector<std::byte> read_image(const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            throw std::runtime_error(std::format("{}: cannot open image", path.string()));

        const auto end = stream.tellg();
        if (end <= 0 || static_cast<std::uintmax_t>(end) > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error(std::format("{}: invalid image size", path.string()));

        std::vector<std::byte> bytes(static_cast<std::size_t>(end));
        stream.seekg(0);
        if (!stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error(std::format("{}: failed to read image", path.string()));
        return bytes;
    }
}

namespace xcompat::detail {
    image_file::image_file(const std::filesystem::path& path)
        : path_(path), bytes_(read_image(path)),
          image_(hy::blkstm(hy::blk(bytes_.data(), bytes_.size())), hy::img_state::flat) {
        parse();
    }

    [[noreturn]] void image_file::fail(const std::string_view reason) const {
        throw std::runtime_error(std::format("{}: {}", path_.string(), reason));
    }

    std::size_t image_file::rva_offset(const std::uint32_t rva, const std::size_t size) const {
        const auto header_limit = std::min<std::size_t>(headers_size_, bytes_.size());
        if (rva <= header_limit && size <= header_limit - rva)
            return rva;

        for (const auto& segment : segments_) {
            if (rva < segment.mapped_rva) continue;
            const auto relative = static_cast<std::size_t>(rva - segment.mapped_rva);
            if (relative > segment.mapped_size || size > segment.mapped_size - relative)
                continue;
            if (relative > segment.raw_size || size > segment.raw_size - relative)
                continue;
            if (segment.raw_offset > bytes_.size() ||
                relative > bytes_.size() - segment.raw_offset ||
                size > bytes_.size() - segment.raw_offset - relative)
                continue;
            return segment.raw_offset + relative;
        }

        fail(std::format("RVA 0x{:X} is outside file-backed image data", rva));
    }

    std::size_t image_file::containing_limit(const std::size_t offset) const {
        const auto header_limit = std::min<std::size_t>(headers_size_, bytes_.size());
        if (offset < header_limit) return header_limit;

        for (const auto& segment : segments_) {
            const auto span = std::min(segment.raw_size, segment.mapped_size);
            if (segment.raw_offset <= offset && offset - segment.raw_offset < span)
                return std::min(bytes_.size(), segment.raw_offset + span);
        }

        fail("file offset has no containing image region");
    }

    std::string image_file::string_at_rva(const std::uint32_t rva) const {
        const auto offset = rva_offset(rva, 1);
        const auto limit = containing_limit(offset);
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset);
        const auto* end = reinterpret_cast<const char*>(bytes_.data() + limit);
        const auto* terminator = std::find(begin, end, '\0');
        if (terminator == end)
            fail(std::format("unterminated string at RVA 0x{:X}", rva));
        return { begin, terminator };
    }

    std::uint32_t image_file::va_to_rva(const std::uint64_t va,
                                        const std::string_view field) const {
        if (va < execution_.preferred_base ||
            va - execution_.preferred_base >= execution_.image_size)
            fail(std::format("{} VA is outside image", field));
        return static_cast<std::uint32_t>(va - execution_.preferred_base);
    }

    void image_file::parse() {
        const auto read_at = [this]<typename T>(const std::size_t offset) -> T {
            if (offset > bytes_.size() || sizeof(T) > bytes_.size() - offset)
                fail("truncated PE structure");
            T value{};
            std::memcpy(&value, bytes_.data() + offset, sizeof(value));
            return value;
        };

        const auto dos = read_at.template operator()<IMAGE_DOS_HEADER>(0);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
            fail("invalid DOS header");
        const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
        if (nt_offset > bytes_.size() || sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > bytes_.size() - nt_offset)
            fail("truncated NT headers");
        if (read_at.template operator()<DWORD>(nt_offset) != IMAGE_NT_SIGNATURE)
            fail("invalid PE signature");

        const auto file_header = read_at.template operator()<IMAGE_FILE_HEADER>(
            nt_offset + sizeof(DWORD));
        if (file_header.Machine != IMAGE_FILE_MACHINE_AMD64)
            fail("image is not AMD64");
        if (file_header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
            fail("truncated PE32+ optional header");

        const auto optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (optional_offset > bytes_.size() ||
            file_header.SizeOfOptionalHeader > bytes_.size() - optional_offset)
            fail("truncated PE32+ optional header");
        const auto optional = read_at.template operator()<IMAGE_OPTIONAL_HEADER64>(optional_offset);
        if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            fail("image is not PE32+");

        const auto directory_bytes = file_header.SizeOfOptionalHeader -
            offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory);
        if (optional.NumberOfRvaAndSizes > directory_bytes / sizeof(IMAGE_DATA_DIRECTORY))
            fail("truncated PE data-directory table");

        const auto section_offset = optional_offset + file_header.SizeOfOptionalHeader;
        if (section_offset > bytes_.size() ||
            file_header.NumberOfSections >
                (bytes_.size() - section_offset) / sizeof(IMAGE_SECTION_HEADER))
            fail("truncated section table");
        if (!file_header.NumberOfSections)
            fail("image has no sections");
        if (!optional.SizeOfImage || optional.SizeOfHeaders > optional.SizeOfImage)
            fail("invalid image size");
        if (optional.AddressOfEntryPoint >= optional.SizeOfImage && optional.AddressOfEntryPoint)
            fail("entry point is outside image");

        execution_ = {
            optional.ImageBase,
            optional.SizeOfImage,
            optional.AddressOfEntryPoint,
            optional.SizeOfStackReserve,
            optional.SizeOfStackCommit
        };
        headers_size_ = optional.SizeOfHeaders;

        if (image_.parse() != hy::STA_SUCCESS)
            fail("failed to parse PE image");

        hy::err segment_error = hy::STA_SUCCESS;
        for (const auto& segment : image_.segments(&segment_error)) {
            const auto raw_base = segment.calc_base(hy::img_state::flat, image_.base);
            const auto mapped_base = segment.calc_base(hy::img_state::mapped, image_.base);
            if (raw_base < image_.base.i || mapped_base < image_.base.i)
                fail("invalid section range");
            const auto raw_offset = raw_base - image_.base.i;
            const auto mapped_offset = mapped_base - image_.base.i;
            const auto raw_size = segment.calc_size(hy::img_state::flat);
            const auto mapped_size = segment.calc_size(hy::img_state::mapped);
            if (mapped_offset > execution_.image_size ||
                mapped_size > execution_.image_size - mapped_offset)
                fail("section mapped range is outside image");
            if (raw_offset > bytes_.size() || raw_size > bytes_.size() - raw_offset)
                fail("section raw range is outside file");
            if (mapped_offset > std::numeric_limits<std::uint32_t>::max())
                fail("section RVA overflow");
            segments_.push_back({ raw_offset, raw_size,
                static_cast<std::uint32_t>(mapped_offset), mapped_size });
        }
        if (segment_error != hy::STA_SUCCESS)
            fail("failed to enumerate PE sections");

        const auto directory = [&optional](const std::size_t index) {
            return index < optional.NumberOfRvaAndSizes
                ? optional.DataDirectory[index]
                : IMAGE_DATA_DIRECTORY{};
        };
        const auto check_image_range = [this](const std::uint32_t rva, const std::size_t size,
                                               const std::string_view field) {
            if (rva > execution_.image_size || size > execution_.image_size - rva)
                fail(std::format("{} range is outside image", field));
        };
        const auto check_directory = [&](const std::size_t index, const std::string_view field,
                                         const bool file_backed) {
            const auto value = directory(index);
            if (!value.VirtualAddress && !value.Size) return value;
            if (!value.VirtualAddress || !value.Size)
                fail(std::format("malformed {}", field));
            check_image_range(value.VirtualAddress, value.Size, field);
            if (file_backed)
                static_cast<void>(rva_offset(value.VirtualAddress, value.Size));
            return value;
        };

        check_directory(IMAGE_DIRECTORY_ENTRY_IMPORT, "import directory", true);
        check_directory(IMAGE_DIRECTORY_ENTRY_EXPORT, "export directory", true);
        check_directory(IMAGE_DIRECTORY_ENTRY_BASERELOC, "base-relocation directory", false);
        const auto delay_directory = check_directory(
            IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, "delay-import directory", true);
        if (!delay_directory.VirtualAddress) return;
        if (delay_directory.Size < sizeof(delay_descriptor_raw))
            fail("malformed delay-import directory");

        const auto descriptor_offset = rva_offset(
            delay_directory.VirtualAddress, delay_directory.Size);
        const auto descriptor_count = delay_directory.Size / sizeof(delay_descriptor_raw);
        bool terminated = false;
        for (std::size_t descriptor_index = 0;
             descriptor_index < descriptor_count; ++descriptor_index) {
            const auto raw = read_at.template operator()<delay_descriptor_raw>(
                descriptor_offset + descriptor_index * sizeof(delay_descriptor_raw));
            if (is_zero(raw)) {
                terminated = true;
                break;
            }
            if (raw.attributes & ~1u)
                fail("unknown delay-import attribute bits");

            const auto convert = [this, &raw](const std::uint64_t value,
                                               const std::string_view field) {
                if (!value) return std::uint32_t{};
                if (raw.attributes & 1u) {
                    if (value > std::numeric_limits<std::uint32_t>::max())
                        fail(std::format("{} RVA overflow", field));
                    return static_cast<std::uint32_t>(value);
                }
                return va_to_rva(value, field);
            };
            const auto name_rva = convert(raw.name, "delay module name");
            const auto handle_rva = convert(raw.module_handle, "delay module handle");
            const auto iat_rva = convert(raw.iat, "delay IAT");
            const auto int_rva = convert(raw.int_table, "delay INT");
            if (!name_rva || !handle_rva || !iat_rva || !int_rva)
                fail("malformed delay-import descriptor");
            check_image_range(handle_rva, sizeof(std::uint64_t), "delay module handle");

            delay_import_descriptor descriptor{
                .module = string_at_rva(name_rva),
                .module_handle_rva = handle_rva
            };
            const auto lookup_offset = rva_offset(int_rva, sizeof(IMAGE_THUNK_DATA64));
            const auto lookup_limit = containing_limit(lookup_offset);
            const auto max_thunks =
                (lookup_limit - lookup_offset) / sizeof(IMAGE_THUNK_DATA64);
            bool thunk_terminated = false;
            for (std::size_t thunk_index = 0; thunk_index < max_thunks; ++thunk_index) {
                const auto thunk = read_at.template operator()<IMAGE_THUNK_DATA64>(
                    lookup_offset + thunk_index * sizeof(IMAGE_THUNK_DATA64));
                if (!thunk.u1.AddressOfData) {
                    thunk_terminated = true;
                    break;
                }

                const auto slot = static_cast<std::uint64_t>(iat_rva) +
                    thunk_index * sizeof(std::uint64_t);
                if (slot > std::numeric_limits<std::uint32_t>::max())
                    fail("delay IAT overflow");
                check_image_range(static_cast<std::uint32_t>(slot),
                    sizeof(std::uint64_t), "delay IAT");

                delay_import_thunk item{ .iat_rva = static_cast<std::uint32_t>(slot) };
                if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) {
                    item.ordinal = static_cast<std::uint16_t>(IMAGE_ORDINAL64(thunk.u1.Ordinal));
                } else {
                    const auto import_name_rva = convert(
                        thunk.u1.AddressOfData, "delay import name");
                    static_cast<void>(rva_offset(import_name_rva, sizeof(WORD) + 1));
                    if (import_name_rva > std::numeric_limits<std::uint32_t>::max() - sizeof(WORD))
                        fail("delay import name RVA overflow");
                    item.name = string_at_rva(import_name_rva + sizeof(WORD));
                }
                descriptor.thunks.push_back(std::move(item));
            }
            if (!thunk_terminated)
                fail("unterminated delay-import thunk array");
            delay_imports_.push_back(std::move(descriptor));
        }
        if (!terminated)
            fail("unterminated delay-import descriptor table");
    }
}
