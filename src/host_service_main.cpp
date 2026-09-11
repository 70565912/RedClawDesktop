#include <atomic>
#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace {

constexpr const wchar_t* kServiceName = L"RedClawHostService";

SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};
HANDLE g_stop_event = nullptr;
std::atomic<bool> g_running{false};

void report_status(DWORD current_state, DWORD win32_exit_code, DWORD wait_hint_ms) {
  g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_service_status.dwCurrentState = current_state;
  g_service_status.dwWin32ExitCode = win32_exit_code;
  g_service_status.dwWaitHint = wait_hint_ms;
  g_service_status.dwControlsAccepted =
      current_state == SERVICE_START_PENDING ? 0U : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);

  if (g_service_status_handle != nullptr) {
    SetServiceStatus(g_service_status_handle, &g_service_status);
  }
}

DWORD WINAPI service_control_handler(DWORD control, DWORD, LPVOID, LPVOID) {
  if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
    report_status(SERVICE_STOP_PENDING, NO_ERROR, 3000);
    if (g_stop_event != nullptr) {
      SetEvent(g_stop_event);
    }
    return NO_ERROR;
  }

  return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
  g_service_status_handle = RegisterServiceCtrlHandlerExW(kServiceName, service_control_handler, nullptr);
  if (g_service_status_handle == nullptr) {
    return;
  }

  report_status(SERVICE_START_PENDING, NO_ERROR, 3000);

  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stop_event == nullptr) {
    report_status(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }

  g_running.store(true);
  report_status(SERVICE_RUNNING, NO_ERROR, 0);

  WaitForSingleObject(g_stop_event, INFINITE);

  g_running.store(false);
  report_status(SERVICE_STOPPED, NO_ERROR, 0);

  CloseHandle(g_stop_event);
  g_stop_event = nullptr;
}

}  // namespace

int wmain() {
  SERVICE_TABLE_ENTRYW dispatch_table[] = {
      {const_cast<LPWSTR>(kServiceName), service_main},
      {nullptr, nullptr},
  };

  if (!StartServiceCtrlDispatcherW(dispatch_table)) {
    const DWORD error = GetLastError();
    if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
      std::cerr << "redclaw_host_service.exe must be started by Windows Service Control Manager" << '\n';
    } else {
      std::cerr << "StartServiceCtrlDispatcherW failed, error=" << error << '\n';
    }
    return 1;
  }

  return g_running.load() ? 1 : 0;
}

#else

int main() {
  std::cerr << "redclaw_host_service is only supported on Windows" << '\n';
  return 1;
}

#endif