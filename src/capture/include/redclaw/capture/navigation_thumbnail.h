#pragma once
#include "redclaw/capture/capture_module.h"

namespace redclaw::capture {

struct NavigationThumbnailImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> bgra;
};

// Called after main-frame publication; produces only a bounded CPU image.
bool prepare_navigation_thumbnail(const CapturedFrame& source, std::uint32_t max_edge,
                                  NavigationThumbnailImage* image, std::string* error = nullptr);

// Create/use/destroy on one worker thread. COM, factory and stream are reused;
// WIC JPEG encoders themselves are single-image objects.
class NavigationThumbnailEncoder {
public:
    NavigationThumbnailEncoder();
    ~NavigationThumbnailEncoder();
    bool encode(const NavigationThumbnailImage& image, std::vector<std::uint8_t>* jpeg,
                std::string* error = nullptr);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace redclaw::capture
