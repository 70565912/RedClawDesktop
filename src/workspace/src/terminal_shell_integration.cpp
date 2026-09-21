#include "redclaw/workspace/terminal_shell_integration.h"
#include <array>
#include <charconv>
#include <openssl/evp.h>
#include <vector>

namespace redclaw::workspace {
void TerminalShellIntegration::reset(std::string nonce) {
    nonce_ = std::move(nonce); pending_.clear(); position_ = 0;
}
std::string TerminalShellIntegration::consume(std::string_view bytes,
    const std::function<void(const Event&)>& callback) {
    pending_.append(bytes);
    const std::string prefix = "\x1b]777;redclaw-v1;" + nonce_ + ";";
    std::string output;
    while (!pending_.empty()) {
        const auto marker = pending_.find(prefix);
        if (marker == std::string::npos) {
            std::size_t retained = 0;
            for (std::size_t n = 1; n < prefix.size() && n <= pending_.size(); ++n)
                if (pending_.compare(pending_.size() - n, n, prefix, 0, n) == 0) retained = n;
            const auto count = pending_.size() - retained;
            output.append(pending_, 0, count); position_ += count; pending_.erase(0, count); break;
        }
        output.append(pending_, 0, marker); position_ += marker; pending_.erase(0, marker);
        const auto end = pending_.find('\a', prefix.size());
        if (end == std::string::npos && pending_.size() <= 8192) break;
        if (end == std::string::npos || end > 8192) {
            output += pending_[0]; ++position_; pending_.erase(0, 1); continue;
        }
        const auto body = pending_.substr(prefix.size(), end - prefix.size());
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (;;) {
            const auto split = body.find(';', start);
            fields.push_back(body.substr(start, split - start));
            if (split == std::string::npos) break;
            start = split + 1;
        }
        if (fields.size() == 4 && (fields[0] == "ready" || fields[0] == "start" || fields[0] == "done" || fields[0] == "output")) {
            Event event; event.kind = fields[0]; event.operation_id = fields[1];
            event.output_position = position_; event.success = fields[2] == "1";
            if (event.kind == "output") {
                auto& payload = fields[3];
                if (payload.size() % 4 || payload.size() > 5464) { pending_.erase(0, end + 1); continue; }
                event.output.resize(3 * payload.size() / 4);
                const auto count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(event.output.data()),
                    reinterpret_cast<const unsigned char*>(payload.data()), static_cast<int>(payload.size()));
                if (count < 0) { pending_.erase(0, end + 1); continue; }
                std::size_t size = static_cast<std::size_t>(count);
                if (payload.ends_with("==")) size -= 2; else if (payload.ends_with("=")) --size;
                event.output.resize(size);
            } else if (!fields[3].empty()) {
                const auto parsed = std::from_chars(fields[3].data(), fields[3].data() + fields[3].size(), event.last_native_exit_code);
                event.has_native_exit_code = parsed.ec == std::errc{} && parsed.ptr == fields[3].data() + fields[3].size();
            }
            if (callback) callback(event);
        } else {
            output.append(pending_, 0, end + 1); position_ += end + 1;
        }
        pending_.erase(0, end + 1);
    }
    return output;
}
std::string TerminalShellIntegration::execution_line(std::string_view id, std::string_view utf8) const {
    std::string encoded(4 * ((utf8.size() + 2) / 3) + 1, '\0');
    const int length = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(utf8.data()), static_cast<int>(utf8.size()));
    encoded.resize(static_cast<std::size_t>(length));
    // Dot source at the interactive scope, so cwd and user variables persist.
    // The fixed output sink drains merged streams before the completion boundary.
    return "$global:__rc_id='" + std::string(id) + "'; $global:__rc_complete=$false; "
        "$global:__rc_ok=$false; try { . ([scriptblock]::Create('Write-RedClawBoundary start' + [Environment]::NewLine + [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('"
        + encoded + "')) + [Environment]::NewLine + '$global:__rc_ok=$?')) *>&1 | Out-String -Stream | Write-RedClawExecOutput } "
        "catch { $global:__rc_ok=$false; $_ | Out-String -Stream | Write-RedClawExecOutput } "
        "finally { $global:__rc_complete=$true }\r";
}
}
