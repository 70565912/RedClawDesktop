#include "redclaw/protocol/compressed_protobuf.h"

#include <memory>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/message_lite.h>
#include <zstd.h>

namespace redclaw::protocol {
namespace {
constexpr std::string_view kMagic = "RCP1";
constexpr std::size_t kHeaderBytes = 5;
struct CompressorDelete { void operator()(ZSTD_CCtx* p) const { ZSTD_freeCCtx(p); } };
struct DecoderDelete { void operator()(ZSTD_DCtx* p) const { ZSTD_freeDCtx(p); } };

bool fail(std::string* error, const char* reason) {
    if (error) *error = reason;
    return false;
}
}  // namespace

std::string compress_protobuf(
    const google::protobuf::MessageLite& message, ProtobufWireKind kind) {
    if (message.ByteSizeLong() > kMaxProtobufWireBytes) return {};
    std::string plain;
    if (!message.SerializeToString(&plain)) return {};
    // Reuse the codec workspace, never retain previous message content as a
    // compression dictionary. A reconnect or lost frame needs no decode history.
    thread_local std::unique_ptr<ZSTD_CCtx, CompressorDelete> context(ZSTD_createCCtx());
    if (!context
        || ZSTD_isError(ZSTD_CCtx_reset(context.get(), ZSTD_reset_session_and_parameters))
        || ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 1))
        || ZSTD_isError(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1))) return {};
    std::string frame(kHeaderBytes + ZSTD_compressBound(plain.size()), '\0');
    frame.replace(0, kMagic.size(), kMagic);
    frame[kMagic.size()] = static_cast<char>(kind);
    const auto size = ZSTD_compress2(context.get(), frame.data() + kHeaderBytes,
        frame.size() - kHeaderBytes, plain.data(), plain.size());
    if (ZSTD_isError(size) || size > kMaxProtobufWireBytes - kHeaderBytes) return {};
    frame.resize(kHeaderBytes + size);
    return frame;
}

bool decompress_protobuf(
    std::string_view frame, ProtobufWireKind expected_kind,
    google::protobuf::MessageLite& message, std::string* error) {
    if (frame.size() <= kHeaderBytes || frame.size() > kMaxProtobufWireBytes
        || !frame.starts_with(kMagic)
        || static_cast<std::uint8_t>(frame[4]) != static_cast<std::uint8_t>(expected_kind)) {
        return fail(error, "unsupported compressed Protobuf frame or size");
    }
    const auto payload = frame.substr(kHeaderBytes);
    const auto plain_size = ZSTD_getFrameContentSize(payload.data(), payload.size());
    // Never allocate from an untrusted length before enforcing the expanded cap.
    // Unknown-size streams, concatenated frames and trailing bytes are rejected.
    if (plain_size == ZSTD_CONTENTSIZE_ERROR || plain_size == ZSTD_CONTENTSIZE_UNKNOWN
        || plain_size > kMaxProtobufWireBytes
        || ZSTD_findFrameCompressedSize(payload.data(), payload.size()) != payload.size()) {
        return fail(error, "invalid or oversized compressed Protobuf content");
    }
    thread_local std::unique_ptr<ZSTD_DCtx, DecoderDelete> context(ZSTD_createDCtx());
    if (!context) return fail(error, "Protobuf decompressor unavailable");
    std::string plain(static_cast<std::size_t>(plain_size), '\0');
    const auto decoded = ZSTD_decompressDCtx(context.get(), plain.data(), plain.size(),
        payload.data(), payload.size());
    if (ZSTD_isError(decoded) || decoded != plain.size()) {
        return fail(error, "corrupt compressed Protobuf content");
    }
    google::protobuf::io::CodedInputStream input(
        reinterpret_cast<const std::uint8_t*>(plain.data()), static_cast<int>(plain.size()));
    input.SetRecursionLimit(16);
    input.SetTotalBytesLimit(static_cast<int>(kMaxProtobufWireBytes));
    message.Clear();
    if (!message.ParseFromCodedStream(&input) || !input.ConsumedEntireMessage()) {
        message.Clear();
        return fail(error, "invalid Protobuf message");
    }
    if (error) error->clear();
    return true;
}
}  // namespace redclaw::protocol
