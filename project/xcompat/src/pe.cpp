#include "pe.hpp"

#include <algorithm>
#include <cstring>
#include <format>
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
}

namespace xcompat::detail {
    mapped_file::mapped_file(const std::filesystem::path& path) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error(std::format("{}: cannot open image ({})", path.string(), GetLastError()));

        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_, &size) || size.QuadPart <= 0 ||
            static_cast<unsigned long long>(size.QuadPart) > std::numeric_limits<std::size_t>::max()) {
            const auto error = GetLastError();
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(std::format("{}: invalid image size ({})", path.string(), error));
        }
        size_ = static_cast<std::size_t>(size.QuadPart);

        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) {
            const auto error = GetLastError();
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(std::format("{}: cannot map image ({})", path.string(), error));
        }

        data_ = static_cast<const std::byte*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
        if (!data_) {
            const auto error = GetLastError();
            CloseHandle(mapping_);
            CloseHandle(file_);
            mapping_ = nullptr;
            file_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(std::format("{}: cannot view image ({})", path.string(), error));
        }
    }

    mapped_file::~mapped_file() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    }

    mapped_file::mapped_file(mapped_file&& other) noexcept
        : file_(std::exchange(other.file_, INVALID_HANDLE_VALUE)),
          mapping_(std::exchange(other.mapping_, nullptr)),
          data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}

    mapped_file& mapped_file::operator=(mapped_file&& other) noexcept {
        if (this == &other) return *this;
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        file_ = std::exchange(other.file_, INVALID_HANDLE_VALUE);
        mapping_ = std::exchange(other.mapping_, nullptr);
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }

    pe_image::pe_image(const std::filesystem::path& path, const std::span<const std::byte> bytes)
        : path_(path), bytes_(bytes) {
        parse();
    }

    pe_image::pe_image(const std::filesystem::path& path)
        : path_(path), mapped_file_(std::make_unique<mapped_file>(path)) {
        bytes_ = mapped_file_->bytes();
        parse();
    }

    [[noreturn]] void pe_image::fail(const std::string_view reason) const {
        throw std::runtime_error(std::format("{}: {}", path_.string(), reason));
    }

    std::size_t pe_image::rva_offset(const std::uint32_t rva, const std::size_t size) const {
        const auto fits = [size](const std::uint64_t begin, const std::uint64_t extent) {
            return size <= extent && begin <= extent - size;
        };

        if (rva < headers_size_ && fits(rva, headers_size_) && rva + size <= bytes_.size())
            return rva;

        for (const auto& section : sections_) {
            if (rva < section.virtual_address) continue;
            const auto relative = static_cast<std::uint64_t>(rva) - section.virtual_address;
            if (!fits(relative, section.raw_size)) continue;
            const auto offset = static_cast<std::uint64_t>(section.raw_offset) + relative;
            if (offset <= bytes_.size() && size <= bytes_.size() - offset)
                return static_cast<std::size_t>(offset);
        }
        fail(std::format("RVA 0x{:X} is outside file-backed image data", rva));
    }

    std::size_t pe_image::containing_limit(const std::size_t offset) const {
        if (offset < headers_size_) return std::min<std::size_t>(headers_size_, bytes_.size());
        for (const auto& section : sections_) {
            const auto begin = static_cast<std::size_t>(section.raw_offset);
            const auto end = begin + section.raw_size;
            if (offset >= begin && offset < end) return std::min(end, bytes_.size());
        }
        fail("file offset has no containing image region");
    }

    std::string pe_image::string_at_rva(const std::uint32_t rva) const {
        const auto offset = rva_offset(rva, 1);
        const auto limit = containing_limit(offset);
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset);
        const auto* end = reinterpret_cast<const char*>(bytes_.data() + limit);
        const auto* terminator = std::find(begin, end, '\0');
        if (terminator == end) fail(std::format("unterminated string at RVA 0x{:X}", rva));
        return { begin, terminator };
    }

    std::uint32_t pe_image::va_to_rva(const std::uint64_t va, const std::string_view field) const {
        if (va < preferred_base || va - preferred_base >= image_size)
            fail(std::format("{} VA is outside image", field));
        return static_cast<std::uint32_t>(va - preferred_base);
    }

    void pe_image::parse() {
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
        const auto signature = read_at.template operator()<DWORD>(nt_offset);
        if (signature != IMAGE_NT_SIGNATURE) fail("invalid PE signature");
        const auto file_header = read_at.template operator()<IMAGE_FILE_HEADER>(nt_offset + sizeof(DWORD));
        if (file_header.Machine != IMAGE_FILE_MACHINE_AMD64)
            fail("image is not AMD64");
        if (file_header.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
            fail("truncated PE32+ optional header");
        const auto optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        const auto optional = read_at.template operator()<IMAGE_OPTIONAL_HEADER64>(optional_offset);
        if (optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            fail("image is not PE32+");
        if (!optional.SizeOfImage || optional.SizeOfHeaders > optional.SizeOfImage)
            fail("invalid image size");

        preferred_base = optional.ImageBase;
        image_size = optional.SizeOfImage;
        headers_size_ = optional.SizeOfHeaders;
        entry_rva = optional.AddressOfEntryPoint;
        stack_reserve = optional.SizeOfStackReserve;
        stack_commit = optional.SizeOfStackCommit;
        is_dll = (file_header.Characteristics & IMAGE_FILE_DLL) != 0;
        if (entry_rva >= image_size && entry_rva != 0) fail("entry point is outside image");

        const auto section_offset = optional_offset + file_header.SizeOfOptionalHeader;
        if (section_offset > bytes_.size() ||
            file_header.NumberOfSections > (bytes_.size() - section_offset) / sizeof(IMAGE_SECTION_HEADER))
            fail("truncated section table");
        sections_.reserve(file_header.NumberOfSections);
        for (std::size_t i = 0; i < file_header.NumberOfSections; ++i) {
            const auto section = read_at.template operator()<IMAGE_SECTION_HEADER>(
                section_offset + i * sizeof(IMAGE_SECTION_HEADER));
            const auto* name = reinterpret_cast<const char*>(section.Name);
            const auto name_size = strnlen(name, IMAGE_SIZEOF_SHORT_NAME);
            if (section.VirtualAddress > image_size ||
                std::max(section.Misc.VirtualSize, section.SizeOfRawData) > image_size - section.VirtualAddress)
                fail("section range is outside image");
            if (section.PointerToRawData > bytes_.size() ||
                section.SizeOfRawData > bytes_.size() - section.PointerToRawData)
                fail("section raw range is outside file");
            sections_.push_back({ std::string(name, name_size), section.VirtualAddress,
                section.Misc.VirtualSize, section.PointerToRawData, section.SizeOfRawData });
        }

        const auto directory = [&optional](const std::size_t index) {
            return index < optional.NumberOfRvaAndSizes
                ? optional.DataDirectory[index]
                : IMAGE_DATA_DIRECTORY{};
        };
        const auto check_image_range = [this](const std::uint32_t rva, const std::size_t size,
                                               const std::string_view field) {
            if (rva > image_size || size > image_size - rva)
                fail(std::format("{} range is outside image", field));
        };

        const auto imports_directory = directory(IMAGE_DIRECTORY_ENTRY_IMPORT);
        if (imports_directory.VirtualAddress || imports_directory.Size) {
            if (!imports_directory.VirtualAddress || imports_directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR))
                fail("malformed import directory");
            check_image_range(imports_directory.VirtualAddress, imports_directory.Size, "import directory");
            const auto offset = rva_offset(imports_directory.VirtualAddress, imports_directory.Size);
            const auto count = imports_directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
            bool terminated = false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto descriptor = read_at.template operator()<IMAGE_IMPORT_DESCRIPTOR>(
                    offset + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
                if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) {
                    terminated = true;
                    break;
                }
                if (!descriptor.Name || !descriptor.FirstThunk)
                    fail("malformed import descriptor");
                import_descriptor record{ .module = string_at_rva(descriptor.Name) };
                const auto lookup_rva = descriptor.OriginalFirstThunk
                    ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
                const auto lookup_offset = rva_offset(lookup_rva, sizeof(IMAGE_THUNK_DATA64));
                const auto lookup_limit = containing_limit(lookup_offset);
                const auto max_thunks = (lookup_limit - lookup_offset) / sizeof(IMAGE_THUNK_DATA64);
                bool thunk_terminated = false;
                for (std::size_t thunk_index = 0; thunk_index < max_thunks; ++thunk_index) {
                    const auto thunk = read_at.template operator()<IMAGE_THUNK_DATA64>(
                        lookup_offset + thunk_index * sizeof(IMAGE_THUNK_DATA64));
                    if (!thunk.u1.AddressOfData) {
                        thunk_terminated = true;
                        break;
                    }
                    const auto iat_rva64 = static_cast<std::uint64_t>(descriptor.FirstThunk) +
                        thunk_index * sizeof(std::uint64_t);
                    if (iat_rva64 > std::numeric_limits<std::uint32_t>::max())
                        fail("import IAT overflow");
                    check_image_range(static_cast<std::uint32_t>(iat_rva64), sizeof(std::uint64_t), "import IAT");
                    import_thunk item{ .iat_rva = static_cast<std::uint32_t>(iat_rva64) };
                    if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) {
                        item.ordinal = static_cast<std::uint16_t>(IMAGE_ORDINAL64(thunk.u1.Ordinal));
                    } else {
                        if (thunk.u1.AddressOfData > std::numeric_limits<std::uint32_t>::max())
                            fail("import name RVA overflow");
                        const auto name_rva = static_cast<std::uint32_t>(thunk.u1.AddressOfData);
                        rva_offset(name_rva, sizeof(WORD) + 1);
                        item.name = string_at_rva(name_rva + sizeof(WORD));
                    }
                    record.thunks.push_back(std::move(item));
                }
                if (!thunk_terminated) fail("unterminated import thunk array");
                imports.push_back(std::move(record));
            }
            if (!terminated) fail("unterminated import descriptor table");
        }

        const auto delay_directory = directory(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
        if (delay_directory.VirtualAddress || delay_directory.Size) {
            if (!delay_directory.VirtualAddress || delay_directory.Size < sizeof(delay_descriptor_raw))
                fail("malformed delay-import directory");
            check_image_range(delay_directory.VirtualAddress, delay_directory.Size, "delay-import directory");
            const auto offset = rva_offset(delay_directory.VirtualAddress, delay_directory.Size);
            const auto count = delay_directory.Size / sizeof(delay_descriptor_raw);
            bool terminated = false;
            for (std::size_t i = 0; i < count; ++i) {
                const auto raw = read_at.template operator()<delay_descriptor_raw>(
                    offset + i * sizeof(delay_descriptor_raw));
                if (is_zero(raw)) {
                    terminated = true;
                    break;
                }
                if (raw.attributes & ~1u) fail("unknown delay-import attribute bits");
                const auto convert = [this, &raw](const std::uint32_t value, const std::string_view field) {
                    if (!value) return std::uint32_t{};
                    return (raw.attributes & 1u) ? value : va_to_rva(value, field);
                };
                const auto name_rva = convert(raw.name, "delay module name");
                const auto handle_rva = convert(raw.module_handle, "delay module handle");
                const auto iat_rva = convert(raw.iat, "delay IAT");
                const auto int_rva = convert(raw.int_table, "delay INT");
                if (!name_rva || !handle_rva || !iat_rva || !int_rva)
                    fail("malformed delay-import descriptor");
                check_image_range(handle_rva, sizeof(std::uint64_t), "delay module handle");
                delay_import_descriptor record{
                    .module = string_at_rva(name_rva),
                    .module_handle_rva = handle_rva
                };
                const auto lookup_offset = rva_offset(int_rva, sizeof(IMAGE_THUNK_DATA64));
                const auto lookup_limit = containing_limit(lookup_offset);
                const auto max_thunks = (lookup_limit - lookup_offset) / sizeof(IMAGE_THUNK_DATA64);
                bool thunk_terminated = false;
                for (std::size_t thunk_index = 0; thunk_index < max_thunks; ++thunk_index) {
                    const auto thunk = read_at.template operator()<IMAGE_THUNK_DATA64>(
                        lookup_offset + thunk_index * sizeof(IMAGE_THUNK_DATA64));
                    if (!thunk.u1.AddressOfData) {
                        thunk_terminated = true;
                        break;
                    }
                    const auto slot64 = static_cast<std::uint64_t>(iat_rva) +
                        thunk_index * sizeof(std::uint64_t);
                    if (slot64 > std::numeric_limits<std::uint32_t>::max()) fail("delay IAT overflow");
                    check_image_range(static_cast<std::uint32_t>(slot64), sizeof(std::uint64_t), "delay IAT");
                    import_thunk item{ .iat_rva = static_cast<std::uint32_t>(slot64) };
                    if (IMAGE_SNAP_BY_ORDINAL64(thunk.u1.Ordinal)) {
                        item.ordinal = static_cast<std::uint16_t>(IMAGE_ORDINAL64(thunk.u1.Ordinal));
                    } else {
                        const auto value = (raw.attributes & 1u)
                            ? thunk.u1.AddressOfData
                            : va_to_rva(thunk.u1.AddressOfData, "delay import name");
                        if (value > std::numeric_limits<std::uint32_t>::max()) fail("delay import name RVA overflow");
                        const auto import_name_rva = static_cast<std::uint32_t>(value);
                        rva_offset(import_name_rva, sizeof(WORD) + 1);
                        item.name = string_at_rva(import_name_rva + sizeof(WORD));
                    }
                    record.thunks.push_back(std::move(item));
                }
                if (!thunk_terminated) fail("unterminated delay-import thunk array");
                delay_imports.push_back(std::move(record));
            }
            if (!terminated) fail("unterminated delay-import descriptor table");
        }

        const auto export_directory = directory(IMAGE_DIRECTORY_ENTRY_EXPORT);
        if (export_directory.VirtualAddress || export_directory.Size) {
            if (!export_directory.VirtualAddress || export_directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY))
                fail("malformed export directory");
            check_image_range(export_directory.VirtualAddress, export_directory.Size, "export directory");
            const auto offset = rva_offset(export_directory.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
            const auto table = read_at.template operator()<IMAGE_EXPORT_DIRECTORY>(offset);
            if (table.NumberOfNames > table.NumberOfFunctions) fail("invalid export name count");
            const auto functions_offset = rva_offset(table.AddressOfFunctions,
                static_cast<std::size_t>(table.NumberOfFunctions) * sizeof(DWORD));
            std::vector<std::optional<std::string>> names(table.NumberOfFunctions);
            if (table.NumberOfNames) {
                const auto names_offset = rva_offset(table.AddressOfNames,
                    static_cast<std::size_t>(table.NumberOfNames) * sizeof(DWORD));
                const auto ordinals_offset = rva_offset(table.AddressOfNameOrdinals,
                    static_cast<std::size_t>(table.NumberOfNames) * sizeof(WORD));
                for (std::size_t i = 0; i < table.NumberOfNames; ++i) {
                    const auto name_rva = read_at.template operator()<DWORD>(names_offset + i * sizeof(DWORD));
                    const auto index = read_at.template operator()<WORD>(ordinals_offset + i * sizeof(WORD));
                    if (index >= table.NumberOfFunctions || names[index]) fail("invalid export ordinal index");
                    names[index] = string_at_rva(name_rva);
                }
            }
            for (std::size_t i = 0; i < table.NumberOfFunctions; ++i) {
                const auto function_rva = read_at.template operator()<DWORD>(functions_offset + i * sizeof(DWORD));
                if (!function_rva) continue;
                check_image_range(function_rva, 1, "export");
                std::optional<std::string> forwarder;
                const auto export_end = static_cast<std::uint64_t>(export_directory.VirtualAddress) + export_directory.Size;
                if (function_rva >= export_directory.VirtualAddress && function_rva < export_end)
                    forwarder = string_at_rva(function_rva);
                exports.push_back({ std::move(names[i]), table.Base + static_cast<std::uint32_t>(i),
                    function_rva, std::move(forwarder) });
            }
        }

        const auto exception_directory = directory(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
        if (exception_directory.VirtualAddress || exception_directory.Size) {
            if (!exception_directory.VirtualAddress ||
                exception_directory.Size % sizeof(RUNTIME_FUNCTION) != 0)
                fail("malformed exception directory size");
            check_image_range(exception_directory.VirtualAddress, exception_directory.Size, "exception directory");
            const auto offset = rva_offset(exception_directory.VirtualAddress, exception_directory.Size);
            const auto count = exception_directory.Size / sizeof(RUNTIME_FUNCTION);
            std::uint32_t previous_begin = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto function = read_at.template operator()<RUNTIME_FUNCTION>(
                    offset + i * sizeof(RUNTIME_FUNCTION));
                if (function.BeginAddress >= function.EndAddress || function.EndAddress > image_size ||
                    function.UnwindData >= image_size || (i && function.BeginAddress < previous_begin))
                    fail("malformed or unsorted exception table");
                previous_begin = function.BeginAddress;
            }
            exceptions = { exception_directory.VirtualAddress, static_cast<std::uint32_t>(count) };
        }

        const auto tls_directory = directory(IMAGE_DIRECTORY_ENTRY_TLS);
        if (tls_directory.VirtualAddress || tls_directory.Size) {
            if (!tls_directory.VirtualAddress || tls_directory.Size < sizeof(IMAGE_TLS_DIRECTORY64))
                fail("malformed TLS directory");
            check_image_range(tls_directory.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64), "TLS directory");
            const auto raw = read_at.template operator()<IMAGE_TLS_DIRECTORY64>(
                rva_offset(tls_directory.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64)));
            if (raw.EndAddressOfRawData < raw.StartAddressOfRawData)
                fail("TLS template end precedes start");
            const auto template_size64 = raw.EndAddressOfRawData - raw.StartAddressOfRawData;
            if (template_size64 > std::numeric_limits<std::uint32_t>::max()) fail("TLS template size overflow");
            const auto alignment_bits = (raw.Characteristics >> 20) & 0xfu;
            if (alignment_bits > 14) fail("invalid TLS alignment");
            tls.present = true;
            tls.alignment = alignment_bits ? 1u << (alignment_bits - 1) : alignof(void*);
            tls.template_size = static_cast<std::uint32_t>(template_size64);
            tls.zero_fill = raw.SizeOfZeroFill;
            if (tls.template_size) {
                tls.template_rva = va_to_rva(raw.StartAddressOfRawData, "TLS template");
                check_image_range(tls.template_rva, tls.template_size, "TLS template");
                rva_offset(tls.template_rva, tls.template_size);
            }
            tls.index_rva = va_to_rva(raw.AddressOfIndex, "TLS index");
            check_image_range(tls.index_rva, sizeof(DWORD), "TLS index");
            if (raw.AddressOfCallBacks) {
                const auto callbacks_rva = va_to_rva(raw.AddressOfCallBacks, "TLS callbacks");
                const auto callbacks_offset = rva_offset(callbacks_rva, sizeof(std::uint64_t));
                const auto callbacks_limit = containing_limit(callbacks_offset);
                const auto max_callbacks = (callbacks_limit - callbacks_offset) / sizeof(std::uint64_t);
                bool terminated = false;
                for (std::size_t i = 0; i < max_callbacks; ++i) {
                    const auto callback = read_at.template operator()<std::uint64_t>(
                        callbacks_offset + i * sizeof(std::uint64_t));
                    if (!callback) {
                        terminated = true;
                        break;
                    }
                    tls.callback_rvas.push_back(va_to_rva(callback, "TLS callback"));
                }
                if (!terminated) fail("unterminated TLS callback array");
            }
            if (tls.template_size > std::numeric_limits<std::uint32_t>::max() - tls.zero_fill)
                fail("TLS allocation size overflow");
        }
    }
}
