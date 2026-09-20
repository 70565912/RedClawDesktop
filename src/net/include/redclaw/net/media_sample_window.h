#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace redclaw::net {

// Bounded samples, measured in feedback rounds rather than fixed wall time.
// Quantiles are evaluated at decision boundaries, never allocate or perform I/O.
class MediaSampleWindow final {
public:
    void add(double value) {
        if (!std::isfinite(value) || value < 0) return;
        samples_[next_] = value;
        next_ = (next_ + 1) % samples_.size();
        count_ = std::min(count_ + 1, samples_.size());
    }
    [[nodiscard]] double quantile(double fraction) const {
        if (count_ == 0) return 0;
        auto sorted = samples_;
        std::sort(sorted.begin(), sorted.begin() + count_);
        return sorted[static_cast<std::size_t>(std::clamp(fraction, 0.0, 1.0)
            * static_cast<double>(count_ - 1))];
    }
    [[nodiscard]] std::size_t size() const { return count_; }
    void reset() { *this = {}; }

private:
    std::array<double, 32> samples_{};
    std::size_t next_ = 0;
    std::size_t count_ = 0;
};

}  // namespace redclaw::net
