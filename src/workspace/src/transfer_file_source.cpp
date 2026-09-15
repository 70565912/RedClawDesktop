#include "redclaw/workspace/transfer_file_source.h"
#include <array>
#include <algorithm>
#include <openssl/evp.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace redclaw::workspace {
struct TransferFileSource::Impl {
    std::array<std::uint8_t, 64 * 1024> buffer{};
    EVP_MD_CTX* digest = nullptr;
    std::uint64_t length = 0, offset = 0;
    std::string hash;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION identity{};
#endif
};
TransferFileSource::TransferFileSource() : impl_(std::make_unique<Impl>()) {}
TransferFileSource::~TransferFileSource() { close(); }
bool TransferFileSource::open(const std::filesystem::path& path, std::string* error) {
    close();
    const auto fail = [&](std::string reason) { if (error) *error = std::move(reason); close(); return false; };
    if (!path.is_absolute()) return fail("transfer_source_path_not_absolute");
#ifdef _WIN32
    impl_->file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (impl_->file == INVALID_HANDLE_VALUE) return fail("transfer_source_open:" + std::to_string(GetLastError()));
    if (GetFileType(impl_->file) != FILE_TYPE_DISK || !GetFileInformationByHandle(impl_->file, &impl_->identity)
        || (impl_->identity.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return fail("transfer_source_not_regular_file");
    impl_->length = (static_cast<std::uint64_t>(impl_->identity.nFileSizeHigh) << 32) | impl_->identity.nFileSizeLow;
    impl_->digest = EVP_MD_CTX_new();
    if (!impl_->digest || EVP_DigestInit_ex(impl_->digest, EVP_sha256(), nullptr) != 1) return fail("transfer_source_hash_initialize");
    if (error) error->clear();
    return true;
#else
    return fail("transfer_source_unsupported");
#endif
}
TransferReadResult TransferFileSource::read(std::size_t max_bytes) {
    TransferReadResult result;
    result.offset = impl_->offset;
    const auto fail = [&](std::string reason) { result.error = std::move(reason); close(); return result; };
    if (!max_bytes || max_bytes > impl_->buffer.size()) return fail("transfer_source_invalid_read_size");
#ifdef _WIN32
    if (impl_->file == INVALID_HANDLE_VALUE) return fail("transfer_source_not_open");
    if (impl_->offset < impl_->length) {
        const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(max_bytes, impl_->length - impl_->offset));
        DWORD count = 0;
        while (count < wanted) {
            DWORD part = 0;
            if (!ReadFile(impl_->file, impl_->buffer.data() + count, wanted - count, &part, nullptr))
                return fail("transfer_source_read:" + std::to_string(GetLastError()));
            if (!part) return fail("transfer_source_changed");
            count += part;
        }
        if (EVP_DigestUpdate(impl_->digest, impl_->buffer.data(), count) != 1) return fail("transfer_source_hash_update");
        impl_->offset += count;
        result.state = TransferReadState::kChunk; result.bytes = {impl_->buffer.data(), count};
        return result;
    }
    BY_HANDLE_FILE_INFORMATION current{};
    if (!GetFileInformationByHandle(impl_->file, &current)
        || current.nFileSizeHigh != impl_->identity.nFileSizeHigh || current.nFileSizeLow != impl_->identity.nFileSizeLow
        || CompareFileTime(&current.ftLastWriteTime, &impl_->identity.ftLastWriteTime) != 0
        || current.nFileIndexHigh != impl_->identity.nFileIndexHigh || current.nFileIndexLow != impl_->identity.nFileIndexLow
        || current.dwVolumeSerialNumber != impl_->identity.dwVolumeSerialNumber) return fail("transfer_source_changed");
    if (impl_->hash.empty()) {
        unsigned char bytes[EVP_MAX_MD_SIZE]{};
        unsigned length = 0;
        if (EVP_DigestFinal_ex(impl_->digest, bytes, &length) != 1 || length != 32) return fail("transfer_source_hash_finalize");
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned index = 0; index < length; ++index) { impl_->hash += hex[bytes[index] >> 4]; impl_->hash += hex[bytes[index] & 15]; }
    }
    result.state = TransferReadState::kComplete; result.sha256 = impl_->hash;
    return result;
#else
    return fail("transfer_source_unsupported");
#endif
}
void TransferFileSource::close() {
#ifdef _WIN32
    if (impl_->file != INVALID_HANDLE_VALUE) { CloseHandle(impl_->file); impl_->file = INVALID_HANDLE_VALUE; }
    impl_->identity = {};
#endif
    if (impl_->digest) { EVP_MD_CTX_free(impl_->digest); impl_->digest = nullptr; }
    impl_->length = impl_->offset = 0; impl_->hash.clear();
}
std::uint64_t TransferFileSource::size() const { return impl_->length; }
}
