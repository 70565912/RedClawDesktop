#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace redclaw::workspace {
// GUI/runtime workspace traffic uses one bounded overlapped byte pipe. The
// runtime scheduler never waits for GUI reads or writes on its media thread.
class LocalWorkspacePipe final {
public:
    LocalWorkspacePipe();
    ~LocalWorkspacePipe();
    LocalWorkspacePipe(const LocalWorkspacePipe&) = delete;
    LocalWorkspacePipe& operator=(const LocalWorkspacePipe&) = delete;
    bool connect(std::string_view name, std::uint32_t expected_gui_pid, std::string* error = nullptr);
    bool send(std::string_view frame);
    void poll(const std::function<void(std::string_view)>& receive);
    bool connected() const;
    bool writable() const;
    void close();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
