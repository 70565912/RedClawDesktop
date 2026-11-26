#pragma once

#include "redclaw/render/render_module.h"
#include <filesystem>
#include <fstream>

namespace redclaw::runtime {

enum class PreviewOutput { kSkipped, kWritten, kFailed };

class DesktopPreviewOutput {
public:
    bool due(std::uint64_t now_ms) const { return now_ms >= next_write_ms_; }

    PreviewOutput publish(const std::filesystem::path& path, const render::DecodedVideoFrame& frame,
        bool ready, std::uint64_t now_ms, std::uint64_t gui_generation, std::string& error) {
        if (!ready || !due(now_ms)) return PreviewOutput::kSkipped;
        next_write_ms_ = now_ms + 1000;
        if (!frame.bgra || !frame.width || !frame.height || frame.width > frame.row_pitch / 4
            || frame.height > frame.pixels.size() / frame.row_pitch) {
            error = "invalid decoded BGRA frame";
            return PreviewOutput::kFailed;
        }
        auto output = path;
        if (gui_generation) output += ".partial";
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (file) {
            file << "P6\n";
            if (gui_generation) file << "# " << gui_generation << '\n';
            file << frame.width << ' ' << frame.height << "\n255\n";
            std::vector<char> rgb(static_cast<std::size_t>(frame.width) * 3);
            for (std::uint32_t row = 0; row < frame.height && file; ++row) {
                const auto* pixel = frame.pixels.data() + static_cast<std::size_t>(row) * frame.row_pitch;
                for (std::size_t offset = 0; offset < rgb.size(); offset += 3, pixel += 4) {
                    rgb[offset] = static_cast<char>(pixel[2]);
                    rgb[offset + 1] = static_cast<char>(pixel[1]);
                    rgb[offset + 2] = static_cast<char>(pixel[0]);
                }
                file.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
            }
            file.close();
        }
        if (file && gui_generation) {
            std::error_code ec;
            std::filesystem::rename(output, path, ec);
            if (ec) { error = ec.message(); return PreviewOutput::kFailed; }
        }
        if (!file) { error = "failed to write decoded preview"; return PreviewOutput::kFailed; }
        return PreviewOutput::kWritten;
    }

private:
    std::uint64_t next_write_ms_ = 0;
};

}  // namespace redclaw::runtime
