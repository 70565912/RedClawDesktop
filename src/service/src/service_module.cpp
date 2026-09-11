#include "redclaw/service/service_module.h"

#include "redclaw/diag/diag_module.h"

#include <cctype>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace redclaw::service {

namespace {

constexpr std::uint64_t kStepUpMaxSkewSeconds = 120;
constexpr std::uint64_t kGrantTokenTtlSeconds = 120;
constexpr std::uint32_t kLockoutFailureThreshold = 5;

bool is_empty(std::string_view value) {
    return value.empty();
}

std::string read_env_or_default(const char* key, std::string default_value) {
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&buffer, &size, key) == 0 && buffer != nullptr) {
        std::string value(buffer);
        std::free(buffer);
        if (!value.empty()) {
            return value;
        }
    }
#else
    if (const char* value = std::getenv(key); value != nullptr && *value != '\0') {
        return std::string(value);
    }
#endif
    return default_value;
}

std::string normalize_fallback_policy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (value == "retain") {
        return value;
    }
    return "clear";
}

std::string normalize_session_code_copy(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const char ch : value) {
        normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return normalized;
}

std::string_view decision_to_string(PrivilegedDecision decision) {
    switch (decision) {
    case PrivilegedDecision::kAllow:
        return "allow";
    case PrivilegedDecision::kDeny:
        return "deny";
    case PrivilegedDecision::kTimeout:
        return "timeout";
    case PrivilegedDecision::kBlocked:
        return "blocked";
    }

    return "unknown";
}

std::string_view error_to_string(PrivilegedError error) {
    switch (error) {
    case PrivilegedError::kNone:
        return "none";
    case PrivilegedError::kInvalidSession:
        return "invalid_session";
    case PrivilegedError::kPolicyDenied:
        return "policy_denied";
    case PrivilegedError::kStepUpRequired:
        return "step_up_required";
    case PrivilegedError::kTokenExpired:
        return "token_expired";
    case PrivilegedError::kReplayDetected:
        return "replay_detected";
    case PrivilegedError::kSecureDesktopUnavailable:
        return "secure_desktop_unavailable";
    case PrivilegedError::kRateLimited:
        return "rate_limited";
    case PrivilegedError::kInternalError:
        return "internal_error";
    }

    return "unknown";
}

class WindowsScmNativeAdapter final : public IWindowsServiceControlAdapter {
public:
    bool install(const ServiceInstallConfig& config, std::string* error_detail) override {
#if !defined(_WIN32)
        if (error_detail != nullptr) {
            *error_detail = "windows_service_wrapper_requires_windows";
        }
        (void)config;
        return false;
#else
        const std::wstring service_name = utf8_to_wstring(config.service_name);
        const std::wstring display_name = utf8_to_wstring(config.display_name);
        const std::wstring binary_path = utf8_to_wstring(config.binary_path);

        if (service_name.empty() || display_name.empty() || binary_path.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "invalid_utf8_service_install_config";
            }
            return false;
        }

        const std::wstring account_name = config.account_name == "LocalSystem"
            ? std::wstring{}
            : utf8_to_wstring(config.account_name);

        const ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE));
        if (!scm.valid()) {
            assign_last_error("open_scm_create_service", error_detail);
            return false;
        }

        const DWORD start_mode = config.auto_start ? SERVICE_AUTO_START : SERVICE_DEMAND_START;
        const ScopedServiceHandle service(CreateServiceW(
            scm.handle,
            service_name.c_str(),
            display_name.c_str(),
            SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE,
            SERVICE_WIN32_OWN_PROCESS,
            start_mode,
            SERVICE_ERROR_NORMAL,
            binary_path.c_str(),
            nullptr,
            nullptr,
            nullptr,
            account_name.empty() ? nullptr : account_name.c_str(),
            nullptr));

        if (!service.valid()) {
            assign_last_error("create_service", error_detail);
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
#endif
    }

    bool start(std::string_view service_name, std::string* error_detail) override {
#if !defined(_WIN32)
        if (error_detail != nullptr) {
            *error_detail = "windows_service_wrapper_requires_windows";
        }
        (void)service_name;
        return false;
#else
        const ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.valid()) {
            assign_last_error("open_scm_connect", error_detail);
            return false;
        }

        const std::wstring service_name_w = utf8_to_wstring(service_name);
        if (service_name_w.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "invalid_utf8_service_name";
            }
            return false;
        }

        const ScopedServiceHandle service(OpenServiceW(scm.handle, service_name_w.c_str(), SERVICE_START | SERVICE_QUERY_STATUS));
        if (!service.valid()) {
            assign_last_error("open_service_start", error_detail);
            return false;
        }

        if (StartServiceW(service.handle, 0, nullptr) != 0) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        const DWORD last_error = GetLastError();
        if (last_error == ERROR_SERVICE_ALREADY_RUNNING) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        assign_error("start_service", last_error, error_detail);
        return false;
#endif
    }

    bool stop(std::string_view service_name, std::string* error_detail) override {
#if !defined(_WIN32)
        if (error_detail != nullptr) {
            *error_detail = "windows_service_wrapper_requires_windows";
        }
        (void)service_name;
        return false;
#else
        const ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.valid()) {
            assign_last_error("open_scm_connect", error_detail);
            return false;
        }

        const std::wstring service_name_w = utf8_to_wstring(service_name);
        if (service_name_w.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "invalid_utf8_service_name";
            }
            return false;
        }

        const ScopedServiceHandle service(OpenServiceW(
            scm.handle,
            service_name_w.c_str(),
            SERVICE_STOP | SERVICE_QUERY_STATUS));
        if (!service.valid()) {
            assign_last_error("open_service_stop", error_detail);
            return false;
        }

        SERVICE_STATUS status = {};
        if (ControlService(service.handle, SERVICE_CONTROL_STOP, &status) != 0) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        const DWORD last_error = GetLastError();
        if (last_error == ERROR_SERVICE_NOT_ACTIVE) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        assign_error("stop_service", last_error, error_detail);
        return false;
#endif
    }

    bool uninstall(std::string_view service_name, std::string* error_detail) override {
#if !defined(_WIN32)
        if (error_detail != nullptr) {
            *error_detail = "windows_service_wrapper_requires_windows";
        }
        (void)service_name;
        return false;
#else
        const ScopedServiceHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
        if (!scm.valid()) {
            assign_last_error("open_scm_connect", error_detail);
            return false;
        }

        const std::wstring service_name_w = utf8_to_wstring(service_name);
        if (service_name_w.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "invalid_utf8_service_name";
            }
            return false;
        }

        const ScopedServiceHandle service(OpenServiceW(
            scm.handle,
            service_name_w.c_str(),
            SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE));
        if (!service.valid()) {
            assign_last_error("open_service_delete", error_detail);
            return false;
        }

        SERVICE_STATUS status = {};
        if (ControlService(service.handle, SERVICE_CONTROL_STOP, &status) == 0) {
            const DWORD stop_error = GetLastError();
            if (stop_error != ERROR_SERVICE_NOT_ACTIVE) {
                assign_error("stop_before_delete", stop_error, error_detail);
                return false;
            }
        }

        if (DeleteService(service.handle) != 0) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        const DWORD delete_error = GetLastError();
        if (delete_error == ERROR_SERVICE_MARKED_FOR_DELETE) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        assign_error("delete_service", delete_error, error_detail);
        return false;
#endif
    }

    bool configure_environment(
        std::string_view service_name,
        const std::unordered_map<std::string, std::string>& environment_overrides,
        std::string* error_detail) override {
        if (environment_overrides.empty()) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

#if !defined(_WIN32)
        if (error_detail != nullptr) {
            *error_detail = "windows_service_wrapper_requires_windows";
        }
        (void)service_name;
        return false;
#else
        const std::wstring service_name_w = utf8_to_wstring(service_name);
        if (service_name_w.empty()) {
            if (error_detail != nullptr) {
                *error_detail = "invalid_utf8_service_name";
            }
            return false;
        }

        const std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Services\\" + service_name_w;
        HKEY service_key = nullptr;
        const LONG open_result = RegOpenKeyExW(
            HKEY_LOCAL_MACHINE,
            key_path.c_str(),
            0,
            KEY_SET_VALUE,
            &service_key);
        if (open_result != ERROR_SUCCESS) {
            assign_error("open_service_registry", static_cast<DWORD>(open_result), error_detail);
            return false;
        }

        std::vector<wchar_t> multi_sz;
        for (const auto& [key, value] : environment_overrides) {
            if (key.empty()) {
                continue;
            }

            const std::wstring key_w = utf8_to_wstring(key);
            const std::wstring value_w = utf8_to_wstring(value);
            if (key_w.empty()) {
                RegCloseKey(service_key);
                if (error_detail != nullptr) {
                    *error_detail = "invalid_utf8_environment_override_key";
                }
                return false;
            }

            const std::wstring line = key_w + L"=" + value_w;
            multi_sz.insert(multi_sz.end(), line.begin(), line.end());
            multi_sz.push_back(L'\0');
        }

        if (multi_sz.empty()) {
            multi_sz.push_back(L'\0');
        }
        multi_sz.push_back(L'\0');

        const DWORD data_size = static_cast<DWORD>(multi_sz.size() * sizeof(wchar_t));
        const LONG set_result = RegSetValueExW(
            service_key,
            L"Environment",
            0,
            REG_MULTI_SZ,
            reinterpret_cast<const BYTE*>(multi_sz.data()),
            data_size);

        RegCloseKey(service_key);

        if (set_result != ERROR_SUCCESS) {
            assign_error("set_service_environment", static_cast<DWORD>(set_result), error_detail);
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
#endif
    }

private:
#if defined(_WIN32)
    struct ScopedServiceHandle {
        SC_HANDLE handle = nullptr;

        ScopedServiceHandle() = default;
        explicit ScopedServiceHandle(SC_HANDLE value) : handle(value) {}

        ScopedServiceHandle(const ScopedServiceHandle&) = delete;
        ScopedServiceHandle& operator=(const ScopedServiceHandle&) = delete;

        ScopedServiceHandle(ScopedServiceHandle&& other) noexcept : handle(other.handle) {
            other.handle = nullptr;
        }

        ScopedServiceHandle& operator=(ScopedServiceHandle&& other) noexcept {
            if (this == &other) {
                return *this;
            }

            if (handle != nullptr) {
                CloseServiceHandle(handle);
            }

            handle = other.handle;
            other.handle = nullptr;
            return *this;
        }

        ~ScopedServiceHandle() {
            if (handle != nullptr) {
                CloseServiceHandle(handle);
            }
        }

        [[nodiscard]] bool valid() const {
            return handle != nullptr;
        }
    };

    static std::wstring utf8_to_wstring(std::string_view value) {
        if (value.empty()) {
            return {};
        }

        const int required_size = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0);
        if (required_size <= 0) {
            return {};
        }

        std::wstring result(static_cast<std::size_t>(required_size), L'\0');
        const int converted = MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required_size);
        if (converted <= 0) {
            return {};
        }

        return result;
    }

    static std::string format_windows_error(DWORD error_code) {
        LPSTR message_buffer = nullptr;
        const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
        const DWORD length = FormatMessageA(
            flags,
            nullptr,
            error_code,
            0,
            reinterpret_cast<LPSTR>(&message_buffer),
            0,
            nullptr);

        std::string message = "win32_error_" + std::to_string(error_code);
        if (length > 0 && message_buffer != nullptr) {
            message += ":";
            message.append(message_buffer, length);
            LocalFree(message_buffer);
        }

        return message;
    }

    static void assign_error(const char* operation, DWORD error_code, std::string* error_detail) {
        if (error_detail == nullptr) {
            return;
        }

        *error_detail = std::string(operation) + ":" + format_windows_error(error_code);
    }

    static void assign_last_error(const char* operation, std::string* error_detail) {
        assign_error(operation, GetLastError(), error_detail);
    }
#endif
};

class WindowsNativeUacPromptEventAdapter final : public IWindowsUacPromptEventAdapter {
public:
    ~WindowsNativeUacPromptEventAdapter() override {
        std::string ignored_error;
        (void)stop(&ignored_error);
    }

    void set_event_sink(UacPromptLifecycleHandler sink) override {
        std::lock_guard<std::mutex> lock(mutex_);
        sink_ = std::move(sink);
    }

    bool start(std::string* error_detail) override {
#if !defined(_WIN32)
        if (error_detail != nullptr) {
            *error_detail = "windows_uac_prompt_source_requires_windows";
        }
        return false;
#else
        if (running_.load()) {
            if (error_detail != nullptr) {
                error_detail->clear();
            }
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!sink_) {
                if (error_detail != nullptr) {
                    *error_detail = "windows_uac_prompt_event_sink_not_set";
                }
                return false;
            }
        }

        running_.store(true);
        try {
            monitor_thread_ = std::thread([this]() {
                monitor_loop();
            });
        } catch (const std::system_error& ex) {
            running_.store(false);
            if (error_detail != nullptr) {
                *error_detail = std::string("uac_monitor_thread_start_failed:") + ex.what();
            }
            return false;
        }

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
#endif
    }

    bool stop(std::string* error_detail) override {
#if defined(_WIN32)
        running_.store(false);
        if (monitor_thread_.joinable()) {
            monitor_thread_.join();
        }

        last_on_secure_desktop_ = false;
        active_prompt_id_.clear();
#endif

        if (error_detail != nullptr) {
            error_detail->clear();
        }
        return true;
    }

private:
    static constexpr std::chrono::milliseconds kProbeInterval{300};
    static constexpr std::string_view kBootstrapSessionId = "bootstrap-session";

    void emit_event(const UacPromptLifecycleEvent& event) {
        UacPromptLifecycleHandler sink_copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sink_copy = sink_;
        }

        if (sink_copy) {
            sink_copy(event);
        }
    }

#if defined(_WIN32)
    static bool is_winlogon_desktop(std::string* error_detail) {
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (desktop == nullptr) {
            if (error_detail != nullptr) {
                *error_detail = "open_input_desktop_failed:" + std::to_string(GetLastError());
            }
            return false;
        }

        wchar_t desktop_name[256] = {};
        DWORD bytes_needed = 0;
        const BOOL ok = GetUserObjectInformationW(
            desktop,
            UOI_NAME,
            desktop_name,
            static_cast<DWORD>(sizeof(desktop_name)),
            &bytes_needed);
        CloseDesktop(desktop);

        if (ok == 0) {
            if (error_detail != nullptr) {
                *error_detail = "query_input_desktop_name_failed:" + std::to_string(GetLastError());
            }
            return false;
        }

        return _wcsicmp(desktop_name, L"Winlogon") == 0;
    }

    std::string next_prompt_id() {
        ++prompt_counter_;
        return "uac-prompt-" + std::to_string(prompt_counter_);
    }

    void monitor_loop() {
        while (running_.load()) {
            std::string probe_error;
            const bool on_secure_desktop = is_winlogon_desktop(&probe_error);

            if (!probe_error.empty()) {
                emit_event({
                    .session_id = std::string(kBootstrapSessionId),
                    .uac_prompt_id = active_prompt_id_,
                    .type = UacPromptLifecycleEventType::kUnavailable,
                    .reason_code = std::move(probe_error),
                });
                last_on_secure_desktop_ = false;
                active_prompt_id_.clear();
            } else if (on_secure_desktop && !last_on_secure_desktop_) {
                active_prompt_id_ = next_prompt_id();
                last_on_secure_desktop_ = true;
                emit_event({
                    .session_id = std::string(kBootstrapSessionId),
                    .uac_prompt_id = active_prompt_id_,
                    .type = UacPromptLifecycleEventType::kRaised,
                    .reason_code = "raised",
                });
            } else if (!on_secure_desktop && last_on_secure_desktop_) {
                emit_event({
                    .session_id = std::string(kBootstrapSessionId),
                    .uac_prompt_id = active_prompt_id_,
                    .type = UacPromptLifecycleEventType::kClosed,
                    .reason_code = "closed",
                });
                last_on_secure_desktop_ = false;
                active_prompt_id_.clear();
            }

            std::this_thread::sleep_for(kProbeInterval);
        }
    }
#endif

    UacPromptLifecycleHandler sink_;
    std::mutex mutex_;
#if defined(_WIN32)
    std::atomic<bool> running_ = false;
    std::thread monitor_thread_;
    bool last_on_secure_desktop_ = false;
    std::string active_prompt_id_;
    std::uint64_t prompt_counter_ = 0;
#endif
};

}  // namespace

SessionSecurityBootstrapConfig resolve_session_security_bootstrap_config() {
    SessionSecurityBootstrapConfig config;
    config.trust_store_path = read_env_or_default("REDCLAW_TRUST_STORE_PATH", config.trust_store_path);
    config.trust_store_fallback = normalize_fallback_policy(
        read_env_or_default("REDCLAW_TRUST_STORE_FALLBACK", config.trust_store_fallback));
    return config;
}

InMemoryRendezvousSessionRegistry::InMemoryRendezvousSessionRegistry(NowProvider now_provider)
    : now_provider_(now_provider ? std::move(now_provider) : &InMemoryRendezvousSessionRegistry::default_now) {}

std::uint64_t InMemoryRendezvousSessionRegistry::default_now() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

bool InMemoryRendezvousSessionRegistry::is_valid_session_code(std::string_view value) {
    if (value.size() != 8) {
        return false;
    }

    for (const char ch : value) {
        const bool is_upper = ch >= 'A' && ch <= 'Z';
        const bool is_digit = ch >= '0' && ch <= '9';
        if (!is_upper && !is_digit) {
            return false;
        }
    }

    return true;
}

RendezvousRegisterResult InMemoryRendezvousSessionRegistry::register_session(
    const RendezvousSessionRegistration& registration) {
    RendezvousRegisterResult result;
    result.accepted = false;
    result.error = RendezvousRegistryError::kInvalidRequest;

    const std::string normalized_code = normalize_session_code_copy(registration.session_code);
    if (!is_valid_session_code(normalized_code)
        || registration.session_id.empty()
        || registration.host_display_name.empty()
        || registration.host_fingerprint_summary.empty()
        || registration.ttl_seconds == 0) {
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = now_provider_();

    // Opportunistically clear stale records before collision checks.
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now >= it->second.expires_at_unix) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }

    if (entries_.contains(normalized_code)) {
        result.error = RendezvousRegistryError::kCodeAlreadyExists;
        return result;
    }

    RegistryEntry entry;
    entry.session_id = registration.session_id;
    entry.host_display_name = registration.host_display_name;
    entry.host_fingerprint_summary = registration.host_fingerprint_summary;
    entry.expires_at_unix = now + registration.ttl_seconds;
    entry.claimed = false;
    entries_.emplace(normalized_code, std::move(entry));

    result.accepted = true;
    result.error = RendezvousRegistryError::kNone;
    result.expires_at_unix = now + registration.ttl_seconds;
    return result;
}

RendezvousLookupResult InMemoryRendezvousSessionRegistry::lookup_session(std::string_view session_code) {
    RendezvousLookupResult result;
    result.found = false;
    result.claimed = false;
    result.error = RendezvousRegistryError::kCodeNotFound;

    const std::string normalized_code = normalize_session_code_copy(session_code);
    if (!is_valid_session_code(normalized_code)) {
        result.error = RendezvousRegistryError::kInvalidRequest;
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = now_provider_();
    const auto it = entries_.find(normalized_code);
    if (it == entries_.end()) {
        return result;
    }

    if (now >= it->second.expires_at_unix) {
        entries_.erase(it);
        result.error = RendezvousRegistryError::kCodeExpired;
        return result;
    }

    result.found = true;
    result.claimed = it->second.claimed;
    result.error = RendezvousRegistryError::kNone;
    result.session_id = it->second.session_id;
    result.host_display_name = it->second.host_display_name;
    result.host_fingerprint_summary = it->second.host_fingerprint_summary;
    result.expires_at_unix = it->second.expires_at_unix;
    return result;
}

RendezvousClaimResult InMemoryRendezvousSessionRegistry::claim_session(std::string_view session_code) {
    RendezvousClaimResult result;
    result.claimed = false;
    result.error = RendezvousRegistryError::kCodeNotFound;

    const std::string normalized_code = normalize_session_code_copy(session_code);
    if (!is_valid_session_code(normalized_code)) {
        result.error = RendezvousRegistryError::kInvalidRequest;
        return result;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = now_provider_();
    const auto it = entries_.find(normalized_code);
    if (it == entries_.end()) {
        return result;
    }

    if (now >= it->second.expires_at_unix) {
        entries_.erase(it);
        result.error = RendezvousRegistryError::kCodeExpired;
        return result;
    }

    if (it->second.claimed) {
        result.error = RendezvousRegistryError::kCodeAlreadyClaimed;
        result.session_id = it->second.session_id;
        return result;
    }

    it->second.claimed = true;
    result.claimed = true;
    result.error = RendezvousRegistryError::kNone;
    result.session_id = it->second.session_id;
    return result;
}

void InMemoryRendezvousSessionRegistry::cleanup_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t now = now_provider_();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (now >= it->second.expires_at_unix) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

std::string format_privileged_audit_log_line(const PrivilegedAuditEvent& event) {
    redclaw::diag::StructuredLogEvent log_event;
    log_event.component = "service.privileged_broker";
    log_event.event = event.action;
    log_event.level = event.error == PrivilegedError::kNone ? redclaw::diag::LogLevel::kInfo : redclaw::diag::LogLevel::kWarn;
    log_event.timestamp_unix = event.timestamp_unix;
    log_event.fields = {
        {"session_id", event.session_id},
        {"operator_id", event.operator_id},
        {"auth_detail", event.detail},
        {"decision", std::string(decision_to_string(event.decision))},
        {"error", std::string(error_to_string(event.error))},
    };

    return redclaw::diag::format_structured_log_line(log_event);
}

void CallbackDrivenHostServiceLifecycleEventSource::set_events(HostSessionLifecycleEvents events) {
    events_ = std::move(events);
}

bool CallbackDrivenHostServiceLifecycleEventSource::start_host_service() {
    if (running_) {
        return false;
    }

    running_ = true;
    if (events_.on_host_service_started) {
        events_.on_host_service_started();
    }

    return true;
}

bool CallbackDrivenHostServiceLifecycleEventSource::stop_host_service(std::string_view session_id) {
    if (!running_) {
        return false;
    }

    if (events_.on_host_service_stopping) {
        events_.on_host_service_stopping(session_id);
    }

    running_ = false;
    return true;
}

void CallbackDrivenHostServiceLifecycleEventSource::notify_secure_desktop_channel_ready(std::string_view session_id) const {
    if (!running_ || !events_.on_secure_desktop_channel_ready) {
        return;
    }

    events_.on_secure_desktop_channel_ready(session_id);
}

void CallbackDrivenHostServiceLifecycleEventSource::notify_secure_desktop_channel_lost(std::string_view session_id) const {
    if (!running_ || !events_.on_secure_desktop_channel_lost) {
        return;
    }

    events_.on_secure_desktop_channel_lost(session_id);
}

void CallbackDrivenHostServiceLifecycleEventSource::notify_transport_disconnected(std::string_view session_id) const {
    if (!running_ || !events_.on_transport_disconnected) {
        return;
    }

    events_.on_transport_disconnected(session_id);
}

bool CallbackDrivenHostServiceLifecycleEventSource::running() const {
    return running_;
}

void InMemoryHostServiceLifecycleEmitter::set_events(HostSessionLifecycleEvents events) {
    events_ = std::move(events);
}

bool InMemoryHostServiceLifecycleEmitter::start_host_service() {
    if (running_) {
        return false;
    }

    running_ = true;
    if (events_.on_host_service_started) {
        events_.on_host_service_started();
    }

    return true;
}

bool InMemoryHostServiceLifecycleEmitter::stop_host_service(std::string_view session_id) {
    if (!running_) {
        return false;
    }

    if (events_.on_host_service_stopping) {
        events_.on_host_service_stopping(session_id);
    }

    running_ = false;
    return true;
}

void InMemoryHostServiceLifecycleEmitter::mark_secure_desktop_channel_ready(std::string_view session_id) const {
    if (!running_ || !events_.on_secure_desktop_channel_ready) {
        return;
    }

    events_.on_secure_desktop_channel_ready(session_id);
}

void InMemoryHostServiceLifecycleEmitter::mark_secure_desktop_channel_lost(std::string_view session_id) const {
    if (!running_ || !events_.on_secure_desktop_channel_lost) {
        return;
    }

    events_.on_secure_desktop_channel_lost(session_id);
}

void InMemoryHostServiceLifecycleEmitter::mark_transport_disconnected(std::string_view session_id) const {
    if (!running_ || !events_.on_transport_disconnected) {
        return;
    }

    events_.on_transport_disconnected(session_id);
}

bool InMemoryHostServiceLifecycleEmitter::running() const {
    return running_;
}

WindowsServiceLifecycleWrapper::WindowsServiceLifecycleWrapper(std::shared_ptr<IWindowsServiceControlAdapter> adapter)
    : adapter_(adapter ? std::move(adapter) : std::make_shared<WindowsScmNativeAdapter>()) {}

ServiceLifecycleError WindowsServiceLifecycleWrapper::install(const ServiceInstallConfig& config) {
    if (!is_valid_service_name(config.service_name)
        || is_empty(config.display_name)
        || is_empty(config.binary_path)
        || is_empty(config.account_name)) {
        last_error_detail_ = "invalid_service_install_config";
        return ServiceLifecycleError::kInvalidConfig;
    }

#if !defined(_WIN32)
    last_error_detail_ = "windows_service_wrapper_requires_windows";
    return ServiceLifecycleError::kUnsupportedPlatform;
#else
    const ServiceLifecycleError install_result = run_operation([this, &config](std::string* error_detail) {
        return adapter_->install(config, error_detail);
    });

    if (install_result != ServiceLifecycleError::kNone || config.environment_overrides.empty()) {
        return install_result;
    }

    return run_operation([this, &config](std::string* error_detail) {
        return adapter_->configure_environment(config.service_name, config.environment_overrides, error_detail);
    });
#endif
}

ServiceLifecycleError WindowsServiceLifecycleWrapper::start(std::string_view service_name) {
    if (!is_valid_service_name(service_name)) {
        last_error_detail_ = "invalid_service_name";
        return ServiceLifecycleError::kInvalidConfig;
    }

#if !defined(_WIN32)
    last_error_detail_ = "windows_service_wrapper_requires_windows";
    return ServiceLifecycleError::kUnsupportedPlatform;
#else
    return run_operation([this, service_name](std::string* error_detail) {
        return adapter_->start(service_name, error_detail);
    });
#endif
}

ServiceLifecycleError WindowsServiceLifecycleWrapper::stop(std::string_view service_name) {
    if (!is_valid_service_name(service_name)) {
        last_error_detail_ = "invalid_service_name";
        return ServiceLifecycleError::kInvalidConfig;
    }

#if !defined(_WIN32)
    last_error_detail_ = "windows_service_wrapper_requires_windows";
    return ServiceLifecycleError::kUnsupportedPlatform;
#else
    return run_operation([this, service_name](std::string* error_detail) {
        return adapter_->stop(service_name, error_detail);
    });
#endif
}

ServiceLifecycleError WindowsServiceLifecycleWrapper::uninstall(std::string_view service_name) {
    if (!is_valid_service_name(service_name)) {
        last_error_detail_ = "invalid_service_name";
        return ServiceLifecycleError::kInvalidConfig;
    }

#if !defined(_WIN32)
    last_error_detail_ = "windows_service_wrapper_requires_windows";
    return ServiceLifecycleError::kUnsupportedPlatform;
#else
    return run_operation([this, service_name](std::string* error_detail) {
        return adapter_->uninstall(service_name, error_detail);
    });
#endif
}

std::string WindowsServiceLifecycleWrapper::last_error_detail() const {
    return last_error_detail_;
}

ServiceLifecycleError WindowsServiceLifecycleWrapper::run_operation(const std::function<bool(std::string*)>& operation) {
    std::string error_detail;
    if (operation(&error_detail)) {
        last_error_detail_.clear();
        return ServiceLifecycleError::kNone;
    }

    if (error_detail.empty()) {
        error_detail = "service_command_failed";
    }

    last_error_detail_ = std::move(error_detail);
    return ServiceLifecycleError::kCommandFailed;
}

bool WindowsServiceLifecycleWrapper::is_valid_service_name(std::string_view service_name) {
    return !is_empty(service_name);
}

HostServiceRuntimeLifecycleBridge::HostServiceRuntimeLifecycleBridge(
    WindowsServiceLifecycleWrapper& service_wrapper,
    CallbackDrivenHostServiceLifecycleEventSource& lifecycle_event_source)
    : service_wrapper_(service_wrapper),
      lifecycle_event_source_(lifecycle_event_source) {}

ServiceLifecycleError HostServiceRuntimeLifecycleBridge::start(std::string_view service_name) {
    const ServiceLifecycleError start_result = service_wrapper_.start(service_name);
    if (start_result != ServiceLifecycleError::kNone) {
        last_error_detail_ = service_wrapper_.last_error_detail();
        return start_result;
    }

    if (!lifecycle_event_source_.running() && !lifecycle_event_source_.start_host_service()) {
        last_error_detail_ = "lifecycle_event_source_start_failed";
        return ServiceLifecycleError::kCommandFailed;
    }

    last_error_detail_.clear();
    return ServiceLifecycleError::kNone;
}

ServiceLifecycleError HostServiceRuntimeLifecycleBridge::stop(
    std::string_view service_name,
    std::string_view session_id) {
    (void)lifecycle_event_source_.stop_host_service(session_id);

    const ServiceLifecycleError stop_result = service_wrapper_.stop(service_name);
    if (stop_result != ServiceLifecycleError::kNone) {
        last_error_detail_ = service_wrapper_.last_error_detail();
        return stop_result;
    }

    last_error_detail_.clear();
    return ServiceLifecycleError::kNone;
}

void HostServiceRuntimeLifecycleBridge::notify_secure_desktop_channel_ready(std::string_view session_id) const {
    lifecycle_event_source_.notify_secure_desktop_channel_ready(session_id);
}

void HostServiceRuntimeLifecycleBridge::notify_secure_desktop_channel_lost(std::string_view session_id) const {
    lifecycle_event_source_.notify_secure_desktop_channel_lost(session_id);
}

void HostServiceRuntimeLifecycleBridge::notify_transport_disconnected(std::string_view session_id) const {
    lifecycle_event_source_.notify_transport_disconnected(session_id);
}

std::string HostServiceRuntimeLifecycleBridge::last_error_detail() const {
    return last_error_detail_;
}

HostServiceSessionSignalProducer::HostServiceSessionSignalProducer(
    HostServiceRuntimeLifecycleBridge& lifecycle_bridge,
    std::string service_name,
    std::string session_id)
    : lifecycle_bridge_(lifecycle_bridge),
      service_name_(std::move(service_name)),
      session_id_(std::move(session_id)) {}

ServiceLifecycleError HostServiceSessionSignalProducer::start_host_service() {
    return lifecycle_bridge_.start(service_name_);
}

ServiceLifecycleError HostServiceSessionSignalProducer::stop_host_service() {
    return lifecycle_bridge_.stop(service_name_, session_id_);
}

void HostServiceSessionSignalProducer::on_secure_desktop_channel_ready() const {
    lifecycle_bridge_.notify_secure_desktop_channel_ready(session_id_);
}

void HostServiceSessionSignalProducer::on_secure_desktop_channel_lost() const {
    lifecycle_bridge_.notify_secure_desktop_channel_lost(session_id_);
}

void HostServiceSessionSignalProducer::on_transport_disconnected() const {
    lifecycle_bridge_.notify_transport_disconnected(session_id_);
}

std::string HostServiceSessionSignalProducer::last_error_detail() const {
    return lifecycle_bridge_.last_error_detail();
}

WindowsUacPromptLifecycleEventSource::WindowsUacPromptLifecycleEventSource(
    std::shared_ptr<IWindowsUacPromptEventAdapter> adapter)
    : adapter_(adapter ? std::move(adapter) : std::make_shared<WindowsNativeUacPromptEventAdapter>()) {}

void WindowsUacPromptLifecycleEventSource::set_handler(UacPromptLifecycleHandler handler) {
    handler_ = std::move(handler);
    adapter_->set_event_sink([this](const UacPromptLifecycleEvent& event) {
        if (!handler_) {
            return;
        }

        handler_(event);
    });
}

bool WindowsUacPromptLifecycleEventSource::start_monitoring() {
    if (running_) {
        return true;
    }

    std::string error_detail;
    if (!adapter_->start(&error_detail)) {
        last_error_detail_ = error_detail.empty() ? "uac_prompt_source_start_failed" : std::move(error_detail);
        return false;
    }

    last_error_detail_.clear();
    running_ = true;
    return true;
}

bool WindowsUacPromptLifecycleEventSource::stop_monitoring() {
    if (!running_) {
        return true;
    }

    std::string error_detail;
    if (!adapter_->stop(&error_detail)) {
        last_error_detail_ = error_detail.empty() ? "uac_prompt_source_stop_failed" : std::move(error_detail);
        return false;
    }

    running_ = false;
    last_error_detail_.clear();
    return true;
}

bool WindowsUacPromptLifecycleEventSource::running() const {
    return running_;
}

std::string WindowsUacPromptLifecycleEventSource::last_error_detail() const {
    return last_error_detail_;
}

bool is_valid_transition(PrivilegedState from, PrivilegedTransitionEvent event, PrivilegedState to) {
    switch (from) {
        case PrivilegedState::kIdle:
            return event == PrivilegedTransitionEvent::kSessionAuthenticated && to == PrivilegedState::kStandardActive;
        case PrivilegedState::kStandardActive:
            return event == PrivilegedTransitionEvent::kRequestPrivilegedControl && to == PrivilegedState::kPrivilegePending;
        case PrivilegedState::kPrivilegePending:
            return (event == PrivilegedTransitionEvent::kVerificationPassed && to == PrivilegedState::kFullControlActive)
                || (event == PrivilegedTransitionEvent::kVerificationFailed && to == PrivilegedState::kStandardActive)
                || (event == PrivilegedTransitionEvent::kLockoutTriggered && to == PrivilegedState::kDeniedLockout);
        case PrivilegedState::kFullControlActive:
            return (event == PrivilegedTransitionEvent::kUacPromptRaised && to == PrivilegedState::kUacPromptActive)
                || ((event == PrivilegedTransitionEvent::kTokenExpired
                        || event == PrivilegedTransitionEvent::kSessionDisconnected
                        || event == PrivilegedTransitionEvent::kPolicyUpdated
                        || event == PrivilegedTransitionEvent::kExplicitRevoke)
                    && to == PrivilegedState::kRevoked);
        case PrivilegedState::kUacPromptActive:
            return (event == PrivilegedTransitionEvent::kUacPromptHandled && to == PrivilegedState::kFullControlActive)
                || ((event == PrivilegedTransitionEvent::kSecureDesktopFailure
                        || event == PrivilegedTransitionEvent::kTokenExpired
                        || event == PrivilegedTransitionEvent::kSessionDisconnected)
                    && to == PrivilegedState::kRevoked);
        case PrivilegedState::kDeniedLockout:
            return event == PrivilegedTransitionEvent::kLockoutCooldownComplete && to == PrivilegedState::kStandardActive;
        case PrivilegedState::kRevoked:
            return false;
    }

    return false;
}

InMemoryPrivilegedControlBroker::InMemoryPrivilegedControlBroker(
    NowProvider now_provider,
    StepUpVerifier step_up_verifier,
        AuditSink audit_sink,
        CapabilityChangeHook capability_change_hook)
    : now_provider_(now_provider ? std::move(now_provider) : &InMemoryPrivilegedControlBroker::default_now),
      step_up_verifier_(step_up_verifier ? std::move(step_up_verifier)
                                         : &InMemoryPrivilegedControlBroker::default_step_up_verifier),
            audit_sink_(std::move(audit_sink)),
            capability_change_hook_(std::move(capability_change_hook)) {}

std::uint64_t InMemoryPrivilegedControlBroker::default_now() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

bool InMemoryPrivilegedControlBroker::default_step_up_verifier(const PrivilegedRequest& request) {
    return !request.step_up_proof.signed_proof.empty();
}

void InMemoryPrivilegedControlBroker::emit_audit(
    const std::string& session_id,
    const std::string& operator_id,
    const std::string& action,
    const std::string& detail,
    PrivilegedDecision decision,
    PrivilegedError error) const {
    if (!audit_sink_) {
        return;
    }

    PrivilegedAuditEvent event;
    event.session_id = session_id;
    event.operator_id = operator_id;
    event.action = action;
    event.detail = detail;
    event.decision = decision;
    event.error = error;
    event.timestamp_unix = now_provider_();
    audit_sink_(event);
}

void InMemoryPrivilegedControlBroker::emit_capability_change(
    const std::string& session_id,
    CapabilityLevel before,
    CapabilityLevel after,
    const std::string& reason) const {
    if (!capability_change_hook_ || before == after) {
        return;
    }

    CapabilityChangeEvent event;
    event.session_id = session_id;
    event.before = before;
    event.after = after;
    event.reason = reason;
    event.timestamp_unix = now_provider_();
    capability_change_hook_(event);
}

InMemoryPrivilegedControlBroker::SessionContext& InMemoryPrivilegedControlBroker::get_or_create_session(
    const std::string& session_id) {
    auto [it, inserted] = sessions_.try_emplace(session_id);
    if (inserted) {
        it->second.state = PrivilegedState::kStandardActive;
        it->second.capability = CapabilityLevel::kStandardControl;
    }
    return it->second;
}

PrivilegedRequestResult InMemoryPrivilegedControlBroker::requestPrivilegedControl(const PrivilegedRequest& request) {
    PrivilegedRequestResult result;
    result.accepted = false;
    result.error = PrivilegedError::kInvalidSession;
    result.granted_level = CapabilityLevel::kStandardControl;

    if (is_empty(request.session_id)
        || is_empty(request.step_up_proof.challenge_id)
        || is_empty(request.step_up_proof.operator_id)
        || is_empty(request.step_up_proof.device_fingerprint)) {
        emit_audit(request.session_id, request.step_up_proof.operator_id, "request_privileged", "invalid_request", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    auto& session = get_or_create_session(request.session_id);

    if (session.failed_attempts >= kLockoutFailureThreshold) {
        session.state = PrivilegedState::kDeniedLockout;
        result.error = PrivilegedError::kRateLimited;
        emit_audit(request.session_id, request.step_up_proof.operator_id, "request_privileged", "rate_limited", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    const std::uint64_t now = now_provider_();

    if (session.seen_challenges.contains(request.step_up_proof.challenge_id)) {
        ++session.failed_attempts;
        result.error = PrivilegedError::kReplayDetected;
        emit_audit(request.session_id, request.step_up_proof.operator_id, "request_privileged", "replay_detected", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    session.seen_challenges.insert(request.step_up_proof.challenge_id);

    if (request.step_up_proof.issued_at_unix > now
        || (now - request.step_up_proof.issued_at_unix) > kStepUpMaxSkewSeconds) {
        ++session.failed_attempts;
        result.error = PrivilegedError::kTokenExpired;
        emit_audit(request.session_id, request.step_up_proof.operator_id, "request_privileged", "proof_expired", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    if (!step_up_verifier_(request)) {
        ++session.failed_attempts;
        result.error = PrivilegedError::kStepUpRequired;
        emit_audit(request.session_id, request.step_up_proof.operator_id, "request_privileged", "step_up_failed", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    session.failed_attempts = 0;
    const CapabilityLevel capability_before = session.capability;
    session.state = PrivilegedState::kFullControlActive;
    session.capability = CapabilityLevel::kFullControl;
    session.active_uac_prompt_id.clear();

    ++token_counter_;
    session.active_token_id = "grant-" + std::to_string(token_counter_);
    session.active_token_expiry = now + kGrantTokenTtlSeconds;

    result.accepted = true;
    result.error = PrivilegedError::kNone;
    result.granted_level = CapabilityLevel::kFullControl;
    result.grant_token.token_id = session.active_token_id;
    result.grant_token.session_id = request.session_id;
    result.grant_token.operator_id = request.step_up_proof.operator_id;
    result.grant_token.expires_at_unix = session.active_token_expiry;
    result.grant_token.token_signature = "sig:" + session.active_token_id;
    emit_capability_change(request.session_id, capability_before, session.capability, "privileged_granted");
    emit_audit(request.session_id, request.step_up_proof.operator_id, "request_privileged", "granted", PrivilegedDecision::kAllow, PrivilegedError::kNone);
    return result;
}

bool InMemoryPrivilegedControlBroker::beginUacPrompt(std::string_view session_id, std::string_view uac_prompt_id) {
    if (is_empty(session_id) || is_empty(uac_prompt_id)) {
        return false;
    }

    const auto session_it = sessions_.find(std::string(session_id));
    if (session_it == sessions_.end()) {
        emit_audit(std::string(session_id), {}, "uac_prompt_state", "invalid_session", PrivilegedDecision::kBlocked, PrivilegedError::kInvalidSession);
        return false;
    }

    auto& session = session_it->second;
    if (session.capability != CapabilityLevel::kFullControl || is_empty(session.active_token_id)) {
        emit_audit(std::string(session_id), {}, "uac_prompt_state", "capability_denied", PrivilegedDecision::kBlocked, PrivilegedError::kPolicyDenied);
        return false;
    }

    session.state = PrivilegedState::kUacPromptActive;
    session.active_uac_prompt_id = std::string(uac_prompt_id);
    emit_audit(std::string(session_id), {}, "uac_prompt_state", "raised", PrivilegedDecision::kBlocked, PrivilegedError::kNone);
    return true;
}

UacConsentResult InMemoryPrivilegedControlBroker::confirmUacConsent(const UacConsentAction& action) {
    UacConsentResult result;
    result.applied = false;
    result.error = PrivilegedError::kInvalidSession;
    result.final_decision = PrivilegedDecision::kBlocked;

    const auto session_it = sessions_.find(action.session_id);
    if (session_it == sessions_.end()) {
        emit_audit(action.session_id, {}, "confirm_uac", "invalid_session", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    auto& session = session_it->second;
    if (session.capability != CapabilityLevel::kFullControl || is_empty(session.active_token_id)) {
        result.error = PrivilegedError::kPolicyDenied;
        emit_audit(action.session_id, {}, "confirm_uac", "capability_denied", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    if (is_empty(action.uac_prompt_id)
        || session.state != PrivilegedState::kUacPromptActive
        || action.uac_prompt_id != session.active_uac_prompt_id) {
        result.error = PrivilegedError::kSecureDesktopUnavailable;
        emit_audit(action.session_id, {}, "confirm_uac", "prompt_unavailable", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    if (action.token_id != session.active_token_id || session.seen_tokens.contains(action.token_id)) {
        result.error = PrivilegedError::kReplayDetected;
        emit_audit(action.session_id, {}, "confirm_uac", "token_replay", PrivilegedDecision::kBlocked, result.error);
        return result;
    }

    const std::uint64_t now = now_provider_();
    if (now > session.active_token_expiry) {
        const CapabilityLevel capability_before = session.capability;
        session.state = PrivilegedState::kRevoked;
        session.capability = CapabilityLevel::kStandardControl;
        session.active_uac_prompt_id.clear();
        result.error = PrivilegedError::kTokenExpired;
        result.final_decision = PrivilegedDecision::kTimeout;
        emit_capability_change(action.session_id, capability_before, session.capability, "token_expired");
        emit_audit(action.session_id, {}, "confirm_uac", "token_expired", result.final_decision, result.error);
        return result;
    }

    if (action.decision != PrivilegedDecision::kAllow && action.decision != PrivilegedDecision::kDeny) {
        const CapabilityLevel capability_before = session.capability;
        session.state = PrivilegedState::kRevoked;
        session.capability = CapabilityLevel::kStandardControl;
        session.active_uac_prompt_id.clear();
        result.error = PrivilegedError::kPolicyDenied;
        result.final_decision = PrivilegedDecision::kBlocked;
        session.seen_tokens.insert(action.token_id);
        emit_capability_change(action.session_id, capability_before, session.capability, "invalid_uac_decision");
        emit_audit(action.session_id, {}, "confirm_uac", "invalid_decision", result.final_decision, result.error);
        return result;
    }

    session.state = PrivilegedState::kFullControlActive;
    session.active_uac_prompt_id.clear();
    session.seen_tokens.insert(action.token_id);

    result.applied = true;
    result.error = PrivilegedError::kNone;
    result.final_decision = action.decision;
    emit_audit(action.session_id, {}, "confirm_uac", "applied", result.final_decision, result.error);
    return result;
}

bool InMemoryPrivilegedControlBroker::reportSecureDesktopUnavailable(
    std::string_view session_id,
    std::string_view reason_code) {
    const auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return false;
    }

    auto& session = it->second;
    if (session.capability != CapabilityLevel::kFullControl) {
        return false;
    }

    const CapabilityLevel capability_before = session.capability;
    session.state = PrivilegedState::kRevoked;
    session.capability = CapabilityLevel::kStandardControl;
    session.active_token_id.clear();
    session.active_token_expiry = 0;
    session.active_uac_prompt_id.clear();

    emit_capability_change(std::string(session_id), capability_before, session.capability, "secure_desktop_unavailable");
    emit_audit(
        std::string(session_id),
        {},
        "uac_prompt_state",
        reason_code.empty() ? "secure_desktop_unavailable" : std::string(reason_code),
        PrivilegedDecision::kBlocked,
        PrivilegedError::kSecureDesktopUnavailable);
    return true;
}

bool InMemoryPrivilegedControlBroker::revokePrivilegedControl(std::string_view session_id, std::string_view /*reason_code*/) {
    const auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return false;
    }

    auto& session = it->second;
    const CapabilityLevel capability_before = session.capability;
    session.state = PrivilegedState::kRevoked;
    session.capability = CapabilityLevel::kStandardControl;
    session.active_token_id.clear();
    session.active_token_expiry = 0;
    session.active_uac_prompt_id.clear();
    emit_capability_change(std::string(session_id), capability_before, session.capability, "explicit_revoke");
    emit_audit(std::string(session_id), {}, "revoke_privileged", "revoked", PrivilegedDecision::kBlocked, PrivilegedError::kNone);
    return true;
}

void InMemoryUacPromptLifecycleEventSource::set_handler(UacPromptLifecycleHandler handler) {
    handler_ = std::move(handler);
}

void InMemoryUacPromptLifecycleEventSource::emit_prompt_raised(
    std::string_view session_id,
    std::string_view uac_prompt_id) const {
    dispatch_event({
        .session_id = std::string(session_id),
        .uac_prompt_id = std::string(uac_prompt_id),
        .type = UacPromptLifecycleEventType::kRaised,
        .reason_code = "raised",
    });
}

void InMemoryUacPromptLifecycleEventSource::emit_prompt_closed(
    std::string_view session_id,
    std::string_view uac_prompt_id) const {
    dispatch_event({
        .session_id = std::string(session_id),
        .uac_prompt_id = std::string(uac_prompt_id),
        .type = UacPromptLifecycleEventType::kClosed,
        .reason_code = "closed",
    });
}

void InMemoryUacPromptLifecycleEventSource::emit_prompt_timeout(
    std::string_view session_id,
    std::string_view uac_prompt_id) const {
    dispatch_event({
        .session_id = std::string(session_id),
        .uac_prompt_id = std::string(uac_prompt_id),
        .type = UacPromptLifecycleEventType::kTimeout,
        .reason_code = "timeout",
    });
}

void InMemoryUacPromptLifecycleEventSource::emit_prompt_unavailable(
    std::string_view session_id,
    std::string_view uac_prompt_id,
    std::string_view reason_code) const {
    dispatch_event({
        .session_id = std::string(session_id),
        .uac_prompt_id = std::string(uac_prompt_id),
        .type = UacPromptLifecycleEventType::kUnavailable,
        .reason_code = std::string(reason_code),
    });
}

void InMemoryUacPromptLifecycleEventSource::dispatch_event(const UacPromptLifecycleEvent& event) const {
    if (!handler_) {
        return;
    }

    handler_(event);
}

void InMemoryUacConsentActionTransport::set_handler(UacConsentActionHandler handler) {
    handler_ = std::move(handler);
}

UacConsentResult InMemoryUacConsentActionTransport::submit(const UacConsentAction& action) const {
    if (!handler_) {
        return {
            .applied = false,
            .error = PrivilegedError::kInternalError,
            .final_decision = PrivilegedDecision::kBlocked,
        };
    }

    return handler_(action);
}

HostServiceUacConsentSignalProducer::HostServiceUacConsentSignalProducer(
    IUacConsentActionTransport& consent_transport,
    std::string session_id)
    : consent_transport_(consent_transport),
      session_id_(std::move(session_id)) {}

UacConsentResult HostServiceUacConsentSignalProducer::submit_allow(
    std::string_view token_id,
    std::string_view uac_prompt_id) const {
    return submit(token_id, uac_prompt_id, PrivilegedDecision::kAllow);
}

UacConsentResult HostServiceUacConsentSignalProducer::submit_deny(
    std::string_view token_id,
    std::string_view uac_prompt_id) const {
    return submit(token_id, uac_prompt_id, PrivilegedDecision::kDeny);
}

UacConsentResult HostServiceUacConsentSignalProducer::submit_timeout(
    std::string_view token_id,
    std::string_view uac_prompt_id) const {
    return submit(token_id, uac_prompt_id, PrivilegedDecision::kTimeout);
}

UacConsentResult HostServiceUacConsentSignalProducer::submit(
    std::string_view token_id,
    std::string_view uac_prompt_id,
    PrivilegedDecision decision) const {
    UacConsentAction action;
    action.session_id = session_id_;
    action.token_id = std::string(token_id);
    action.uac_prompt_id = std::string(uac_prompt_id);
    action.decision = decision;
    return consent_transport_.submit(action);
}

HostServiceUacPromptRuntimeCoordinator::HostServiceUacPromptRuntimeCoordinator(
    IPrivilegedControlBroker& broker,
    IUacPromptLifecycleEventSource& lifecycle_source,
    IUacConsentActionTransport& consent_transport)
    : broker_(broker) {
    lifecycle_source.set_handler([this](const UacPromptLifecycleEvent& event) {
        handle_prompt_event(event);
    });

    consent_transport.set_handler([this](const UacConsentAction& action) {
        return handle_consent_action(action);
    });
}

bool HostServiceUacPromptRuntimeCoordinator::last_prompt_event_applied() const {
    return last_prompt_event_applied_;
}

void HostServiceUacPromptRuntimeCoordinator::handle_prompt_event(const UacPromptLifecycleEvent& event) {
    switch (event.type) {
    case UacPromptLifecycleEventType::kRaised:
        last_prompt_event_applied_ = broker_.beginUacPrompt(event.session_id, event.uac_prompt_id);
        return;
    case UacPromptLifecycleEventType::kClosed:
        last_prompt_event_applied_ = broker_.reportSecureDesktopUnavailable(
            event.session_id,
            event.reason_code.empty() ? "prompt_closed" : event.reason_code);
        return;
    case UacPromptLifecycleEventType::kTimeout:
        last_prompt_event_applied_ = broker_.reportSecureDesktopUnavailable(
            event.session_id,
            event.reason_code.empty() ? "prompt_timeout" : event.reason_code);
        return;
    case UacPromptLifecycleEventType::kUnavailable:
        last_prompt_event_applied_ = broker_.reportSecureDesktopUnavailable(
            event.session_id,
            event.reason_code.empty() ? "secure_desktop_unavailable" : event.reason_code);
        return;
    }

    last_prompt_event_applied_ = false;
}

UacConsentResult HostServiceUacPromptRuntimeCoordinator::handle_consent_action(const UacConsentAction& action) const {
    return broker_.confirmUacConsent(action);
}

CapabilityLevel InMemoryPrivilegedControlBroker::currentCapability(std::string_view session_id) const {
    const auto it = sessions_.find(std::string(session_id));
    if (it == sessions_.end()) {
        return CapabilityLevel::kStandardControl;
    }

    return it->second.capability;
}

std::string_view module_name() {
    return "service";
}
}
