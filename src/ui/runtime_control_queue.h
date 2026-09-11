#pragma once
#include "redclaw/protocol/stream_control_protocol.h"
#include <QByteArray>
#include <deque>
#include <optional>

namespace redclaw::ui {

class RuntimeControlQueue {
public:
    static constexpr qsizetype kByteCapacity = 1024 * 1024;
    static constexpr std::size_t kMessageCapacity = 256;
    bool append(const QByteArray& bytes) {
        if (failed_ || bytes.size() > kByteCapacity - bytes_) return fail();
        pending_.append(bytes); bytes_ += bytes.size(); peak_bytes_ = std::max(peak_bytes_, bytes_);
        for (;;) {
            const auto newline = pending_.indexOf('\n');
            if (newline < 0) break;
            if (newline > static_cast<qsizetype>(redclaw::protocol::kMaxLocalRuntimeControlFrameBytes)
                || frames_.size() == kMessageCapacity) return fail();
            frames_.push_back(pending_.left(newline + 1)); pending_.remove(0, newline + 1);
            peak_messages_ = std::max(peak_messages_, frames_.size());
        }
        if (pending_.size() > static_cast<qsizetype>(redclaw::protocol::kMaxLocalRuntimeControlFrameBytes)) return fail();
        return true;
    }
    std::optional<QByteArray> take() {
        if (failed_ || frames_.empty()) return {};
        auto frame = std::move(frames_.front()); frames_.pop_front(); bytes_ -= frame.size();
        return frame.trimmed();
    }
    bool fail() { failed_ = true; frames_.clear(); pending_.clear(); bytes_ = 0; return false; }
    bool failed() const { return failed_; }
    bool ready() const { return !frames_.empty(); }
    qsizetype bytes() const { return bytes_; }
    qsizetype peak_bytes() const { return peak_bytes_; }
    std::size_t messages() const { return frames_.size(); }
    std::size_t peak_messages() const { return peak_messages_; }
private:
    QByteArray pending_;
    std::deque<QByteArray> frames_;
    qsizetype bytes_ = 0, peak_bytes_ = 0;
    std::size_t peak_messages_ = 0;
    bool failed_ = false;
};

}  // namespace redclaw::ui
