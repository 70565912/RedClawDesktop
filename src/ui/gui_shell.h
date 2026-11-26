#pragma once

#include <string>

namespace redclaw::diag {
class ProcessFileLogger;
}

namespace redclaw::ui {
struct GuiAutoStartOptions;

bool launch_gui_shell(
    int argc,
    char** argv,
    redclaw::diag::ProcessFileLogger* process_logger = nullptr,
    std::string* error_detail = nullptr,
    const GuiAutoStartOptions* private_startup = nullptr);

}
