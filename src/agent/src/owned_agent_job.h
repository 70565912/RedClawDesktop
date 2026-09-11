#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace redclaw::agent {
// Only processes created suspended by this component may be attached. Closing
// the job reclaims descendants that could otherwise keep Provider pipes open.
class OwnedAgentJob final {
public:
    ~OwnedAgentJob() { reset(); }
    OwnedAgentJob() = default;
    OwnedAgentJob(const OwnedAgentJob&) = delete;
    OwnedAgentJob& operator=(const OwnedAgentJob&) = delete;
    bool attach_and_resume(PROCESS_INFORMATION& process) {
        reset();
        handle_ = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!handle_ || !SetInformationJobObject(handle_, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits)) || !AssignProcessToJobObject(handle_, process.hProcess)
            || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
            TerminateProcess(process.hProcess, 125);
            WaitForSingleObject(process.hProcess, 2000);
            reset();
            return false;
        }
        return true;
    }
    void terminate(UINT code) { if (handle_) TerminateJobObject(handle_, code); }
    void reset() { if (handle_) CloseHandle(handle_); handle_ = nullptr; }
private:
    HANDLE handle_ = nullptr;
};
}  // namespace redclaw::agent
#endif
