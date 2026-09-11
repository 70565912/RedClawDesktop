#include "redclaw/agent/coordination.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

#include <boost/json.hpp>
#include <openssl/sha.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Aclapi.h>
#include <Windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace redclaw::agent {
namespace {

constexpr std::size_t kMaxJournalLineBytes = 16U * 1024U;
constexpr std::uint64_t kMaxLifecycleApprovalTtlMs = 120U * 1000U;
constexpr std::uint32_t kJournalLockTimeoutMs = 10000;

void assign_error(std::string value, std::string* error) {
    if (error != nullptr) {
        *error = std::move(value);
    }
}

class JournalLock final {
public:
    JournalLock(const std::filesystem::path& path, std::string* error) {
#ifdef _WIN32
        std::error_code filesystem_error;
        std::wstring normalized = std::filesystem::absolute(path, filesystem_error)
                                      .lexically_normal().wstring();
        if (filesystem_error) {
            assign_error("coordination journal path cannot be normalized", error);
            return;
        }
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](wchar_t ch) { return std::towlower(ch); });
        const std::string path_bytes(
            reinterpret_cast<const char*>(normalized.data()),
            normalized.size() * sizeof(wchar_t));
        const std::string digest = sha256_hex(path_bytes);
        const std::wstring name = L"Local\\RedClawDesktop.CoordinationJournal."
            + std::wstring(digest.begin(), digest.end());
        handle_ = CreateMutexW(nullptr, FALSE, name.c_str());
        if (handle_ == nullptr) {
            assign_error("coordination journal lock cannot be created", error);
            return;
        }
        const DWORD wait_result = WaitForSingleObject(handle_, kJournalLockTimeoutMs);
        if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
            assign_error("coordination journal lock timed out", error);
            CloseHandle(handle_);
            handle_ = nullptr;
            return;
        }
#else
        lock_ = std::unique_lock<std::mutex>(mutex_);
#endif
        acquired_ = true;
    }

    ~JournalLock() {
#ifdef _WIN32
        if (handle_ != nullptr) {
            ReleaseMutex(handle_);
            CloseHandle(handle_);
        }
#endif
    }

    JournalLock(const JournalLock&) = delete;
    JournalLock& operator=(const JournalLock&) = delete;

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#else
    inline static std::mutex mutex_;
    std::unique_lock<std::mutex> lock_;
#endif
    bool acquired_ = false;
};

bool valid_token(std::string_view value, std::size_t max_size, bool allow_empty = true) {
    if ((!allow_empty && value.empty()) || value.size() > max_size) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const auto uch = static_cast<unsigned char>(ch);
        return std::isalnum(uch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '/';
    });
}

bool valid_hex(std::string_view value, std::size_t minimum, std::size_t maximum,
               bool allow_empty = true) {
    if (value.empty()) {
        return allow_empty;
    }
    return value.size() >= minimum && value.size() <= maximum
        && std::all_of(value.begin(), value.end(), [](char ch) {
            return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
        });
}

bool terminal_state(redclaw::protocol::AgentTaskStateV1 state) {
    return state == redclaw::protocol::AgentTaskStateV1::kCompleted
        || state == redclaw::protocol::AgentTaskStateV1::kFailed
        || state == redclaw::protocol::AgentTaskStateV1::kInterrupted;
}

CoordinationRequestStateV1 request_state_from_agent(
    redclaw::protocol::AgentTaskStateV1 state) {
    switch (state) {
        case redclaw::protocol::AgentTaskStateV1::kPaused:
            return CoordinationRequestStateV1::kPaused;
        case redclaw::protocol::AgentTaskStateV1::kCompleted:
            return CoordinationRequestStateV1::kCompleted;
        case redclaw::protocol::AgentTaskStateV1::kFailed:
            return CoordinationRequestStateV1::kFailed;
        case redclaw::protocol::AgentTaskStateV1::kInterrupted:
            return CoordinationRequestStateV1::kInterrupted;
        default:
            return CoordinationRequestStateV1::kActive;
    }
}

bool state_changing_message(redclaw::protocol::AgentMessageTypeV1 type) {
    return type == redclaw::protocol::AgentMessageTypeV1::kTaskCreate
        || type == redclaw::protocol::AgentMessageTypeV1::kTurnStart
        || type == redclaw::protocol::AgentMessageTypeV1::kTurnSteer
        || type == redclaw::protocol::AgentMessageTypeV1::kTurnInterrupt
        || type == redclaw::protocol::AgentMessageTypeV1::kApprovalDecision;
}

bool controller_message(redclaw::protocol::AgentMessageTypeV1 type) {
    return state_changing_message(type)
        || type == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest
        || type == redclaw::protocol::AgentMessageTypeV1::kEventAck;
}

std::optional<CoordinationAuthorityV1> parse_authority(std::string_view value) {
    for (const auto candidate : {CoordinationAuthorityV1::kNone,
                                 CoordinationAuthorityV1::kNormalAgent,
                                 CoordinationAuthorityV1::kDebugBridge,
                                 CoordinationAuthorityV1::kOutOfBand}) {
        if (value == to_string(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<CoordinationRecordTypeV1> parse_record_type(std::string_view value) {
    for (const auto candidate : {CoordinationRecordTypeV1::kAuthorityTransfer,
                                 CoordinationRecordTypeV1::kRequest,
                                 CoordinationRecordTypeV1::kReply,
                                 CoordinationRecordTypeV1::kSupersede,
                                 CoordinationRecordTypeV1::kAuthorization,
                                 CoordinationRecordTypeV1::kTerminal,
                                 CoordinationRecordTypeV1::kEvidence,
                                 CoordinationRecordTypeV1::kLifecycle}) {
        if (value == to_string(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<CoordinationRequestStateV1> parse_request_state(std::string_view value) {
    for (const auto candidate : {CoordinationRequestStateV1::kAccepted,
                                 CoordinationRequestStateV1::kSyncing,
                                 CoordinationRequestStateV1::kActive,
                                 CoordinationRequestStateV1::kPaused,
                                 CoordinationRequestStateV1::kSuperseded,
                                 CoordinationRequestStateV1::kCompleted,
                                 CoordinationRequestStateV1::kFailed,
                                 CoordinationRequestStateV1::kInterrupted}) {
        if (value == to_string(candidate)) {
            return candidate;
        }
    }
    return std::nullopt;
}

bool validate_record(const CoordinationJournalRecordV1& record, std::string* error) {
    const bool valid_manifest = record.evidence_manifest_name.empty()
        || (valid_token(record.evidence_manifest_name, 256, false)
            && record.evidence_manifest_name.find('/') == std::string::npos);
    if (!valid_token(record.request_id, 128)
        || !valid_token(record.reply_to_request_id, 128)
        || !valid_token(record.supersedes_request_id, 128)
        || !valid_token(record.task_id, 128)
        || !valid_token(record.authorization_state, 64)
        || !valid_token(record.terminal_result, 128)
        || !valid_manifest
        || !valid_hex(record.request_digest_sha256, 64, 64)
        || !valid_hex(record.git_sha, 7, 64)
        || !valid_hex(record.executable_sha256, 64, 64)
        || !valid_hex(record.evidence_sha256, 64, 64)) {
        assign_error("coordination journal record contains an invalid or sensitive field", error);
        return false;
    }
    return true;
}

boost::json::object record_json(const CoordinationJournalRecordV1& record) {
    return {
        {"schema", "redclaw.coordination.journal.v1"},
        {"sequence", record.sequence},
        {"recorded_at_ms", record.recorded_at_ms},
        {"record_type", to_string(record.record_type)},
        {"authority", to_string(record.authority)},
        {"request_state", to_string(record.request_state)},
        {"request_id", record.request_id},
        {"reply_to_request_id", record.reply_to_request_id},
        {"supersedes_request_id", record.supersedes_request_id},
        {"task_id", record.task_id},
        {"request_digest_sha256", record.request_digest_sha256},
        {"authorization_state", record.authorization_state},
        {"git_sha", record.git_sha},
        {"executable_sha256", record.executable_sha256},
        {"terminal_result", record.terminal_result},
        {"evidence_manifest_name", record.evidence_manifest_name},
        {"evidence_sha256", record.evidence_sha256},
        {"acknowledged_event_sequence", record.acknowledged_event_sequence},
    };
}

bool read_string(const boost::json::object& object, std::string_view name, std::string* value) {
    const auto found = object.find(name);
    if (found == object.end() || !found->value().is_string()) {
        return false;
    }
    *value = found->value().as_string().c_str();
    return true;
}

bool read_u64(const boost::json::object& object, std::string_view name,
              std::uint64_t* value) {
    const auto found = object.find(name);
    if (found == object.end()) {
        return false;
    }
    if (found->value().is_uint64()) {
        *value = found->value().as_uint64();
        return true;
    }
    if (found->value().is_int64() && found->value().as_int64() >= 0) {
        *value = static_cast<std::uint64_t>(found->value().as_int64());
        return true;
    }
    return false;
}

bool parse_record(std::string_view line, CoordinationJournalRecordV1* record,
                  std::string* error) {
    boost::system::error_code json_error;
    const auto parsed = boost::json::parse(line, json_error);
    if (json_error || !parsed.is_object()) {
        assign_error("coordination journal line is not valid JSON", error);
        return false;
    }
    const auto& object = parsed.as_object();
    const auto schema = object.if_contains("schema");
    std::uint64_t sequence = 0;
    std::uint64_t recorded_at = 0;
    std::uint64_t acknowledged = 0;
    std::string record_type;
    std::string authority;
    std::string request_state;
    if (schema == nullptr || !schema->is_string()
        || schema->as_string() != "redclaw.coordination.journal.v1"
        || !read_u64(object, "sequence", &sequence)
        || !read_u64(object, "recorded_at_ms", &recorded_at)
        || !read_string(object, "record_type", &record_type)
        || !read_string(object, "authority", &authority)
        || !read_string(object, "request_state", &request_state)
        || !read_string(object, "request_id", &record->request_id)
        || !read_string(object, "reply_to_request_id", &record->reply_to_request_id)
        || !read_string(object, "supersedes_request_id", &record->supersedes_request_id)
        || !read_string(object, "task_id", &record->task_id)
        || !read_string(object, "request_digest_sha256", &record->request_digest_sha256)
        || !read_string(object, "authorization_state", &record->authorization_state)
        || !read_string(object, "git_sha", &record->git_sha)
        || !read_string(object, "executable_sha256", &record->executable_sha256)
        || !read_string(object, "terminal_result", &record->terminal_result)
        || !read_string(object, "evidence_manifest_name", &record->evidence_manifest_name)
        || !read_string(object, "evidence_sha256", &record->evidence_sha256)) {
        assign_error("coordination journal line has missing or invalid fields", error);
        return false;
    }
    const auto parsed_type = parse_record_type(record_type);
    const auto parsed_authority = parse_authority(authority);
    const auto parsed_state = parse_request_state(request_state);
    if (!read_u64(object, "acknowledged_event_sequence", &acknowledged)
        || !parsed_type.has_value() || !parsed_authority.has_value()
        || !parsed_state.has_value()) {
        assign_error("coordination journal line has an invalid enum or sequence", error);
        return false;
    }
    record->sequence = sequence;
    record->recorded_at_ms = recorded_at;
    record->record_type = *parsed_type;
    record->authority = *parsed_authority;
    record->request_state = *parsed_state;
    record->acknowledged_event_sequence = acknowledged;
    return validate_record(*record, error);
}

bool protect_owner_only(const std::filesystem::path& path, std::string* error) {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        assign_error("failed to inspect current user for journal ACL", error);
        return false;
    }
    DWORD bytes = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    const bool token_ok = bytes > 0
        && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!token_ok) {
        assign_error("failed to resolve current user for journal ACL", error);
        return false;
    }
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const std::wstring path_text = path.wstring();
    PSID owner_sid = nullptr;
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    const DWORD owner_result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path_text.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION, &owner_sid, nullptr, nullptr, nullptr,
        &security_descriptor);
    const bool owner_matches = owner_result == ERROR_SUCCESS
        && owner_sid != nullptr && EqualSid(owner_sid, token_user->User.Sid) != FALSE;
    if (security_descriptor != nullptr) {
        LocalFree(security_descriptor);
    }
    if (!owner_matches) {
        assign_error("coordination journal is not owned by the current user", error);
        return false;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | DELETE;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(token_user->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
        assign_error("failed to build journal ACL", error);
        return false;
    }
    const DWORD result = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path_text.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (result != ERROR_SUCCESS) {
        assign_error("failed to apply current-user journal ACL", error);
        return false;
    }
    return true;
#else
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        assign_error("failed to apply owner-only journal permissions", error);
        return false;
    }
    return true;
#endif
}

bool verify_owner_only(const std::filesystem::path& path, std::string* error) {
#ifdef _WIN32
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        assign_error("failed to inspect current user for journal ACL", error);
        return false;
    }
    DWORD bytes = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    const bool token_ok = bytes > 0
        && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!token_ok) {
        assign_error("failed to resolve current user for journal ACL", error);
        return false;
    }
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    const std::wstring path_text = path.wstring();
    PSID owner_sid = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    const DWORD result = GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path_text.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner_sid, nullptr, &dacl, nullptr, &security_descriptor);
    bool valid = result == ERROR_SUCCESS && security_descriptor != nullptr
        && owner_sid != nullptr && dacl != nullptr
        && EqualSid(owner_sid, token_user->User.Sid) != FALSE;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    valid = valid
        && GetSecurityDescriptorControl(security_descriptor, &control, &revision) != FALSE
        && (control & SE_DACL_PROTECTED) != 0;
    bool current_user_allowed = false;
    if (valid) {
        for (DWORD index = 0; index < dacl->AceCount; ++index) {
            void* raw_ace = nullptr;
            if (GetAce(dacl, index, &raw_ace) == FALSE) {
                valid = false;
                break;
            }
            const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                continue;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
            PSID ace_sid = const_cast<DWORD*>(&ace->SidStart);
            if (EqualSid(ace_sid, token_user->User.Sid) == FALSE) {
                valid = false;
                break;
            }
            current_user_allowed = true;
        }
    }
    if (security_descriptor != nullptr) {
        LocalFree(security_descriptor);
    }
    if (!valid || !current_user_allowed) {
        assign_error("coordination journal ACL is not current-user-only", error);
        return false;
    }
    return true;
#else
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || status.st_uid != ::geteuid()
        || (status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        assign_error("coordination journal permissions are not owner-only", error);
        return false;
    }
    return true;
#endif
}

std::string lifecycle_material(const AgentLifecycleRequestV1& request) {
    std::ostringstream out;
    out << "redclaw-agent-lifecycle-v1\n"
        << request.request_id << '\n'
        << to_string(request.action) << '\n'
        << request.expected_git_sha << '\n'
        << request.candidate_executable_sha256 << '\n'
        << request.target_path_sha256 << '\n'
        << request.target_pid << '\n'
        << (request.build_gate_passed ? "true" : "false") << '\n'
        << (request.focused_test_gate_passed ? "true" : "false") << '\n';
    return out.str();
}

bool valid_lifecycle_request(const AgentLifecycleRequestV1& request, std::string* error) {
    if (!valid_token(request.request_id, 128, false)
        || !valid_hex(request.expected_git_sha, 7, 64, false)
        || !valid_hex(request.candidate_executable_sha256, 64, 64, false)
        || !valid_hex(request.target_path_sha256, 64, 64, false)
        || request.target_pid == 0) {
        assign_error("lifecycle request identity is invalid", error);
        return false;
    }
    if (!request.build_gate_passed || !request.focused_test_gate_passed) {
        assign_error("lifecycle prerequisite gate failed", error);
        return false;
    }
    return true;
}

}  // namespace

std::string_view to_string(CoordinationAuthorityV1 value) {
    switch (value) {
        case CoordinationAuthorityV1::kNone: return "none";
        case CoordinationAuthorityV1::kNormalAgent: return "normal_agent";
        case CoordinationAuthorityV1::kDebugBridge: return "debug_bridge";
        case CoordinationAuthorityV1::kOutOfBand: return "out_of_band";
    }
    return "none";
}

std::string_view to_string(CoordinationRecordTypeV1 value) {
    switch (value) {
        case CoordinationRecordTypeV1::kAuthorityTransfer: return "authority_transfer";
        case CoordinationRecordTypeV1::kRequest: return "request";
        case CoordinationRecordTypeV1::kReply: return "reply";
        case CoordinationRecordTypeV1::kSupersede: return "supersede";
        case CoordinationRecordTypeV1::kAuthorization: return "authorization";
        case CoordinationRecordTypeV1::kTerminal: return "terminal";
        case CoordinationRecordTypeV1::kEvidence: return "evidence";
        case CoordinationRecordTypeV1::kLifecycle: return "lifecycle";
    }
    return "request";
}

std::string_view to_string(CoordinationRequestStateV1 value) {
    switch (value) {
        case CoordinationRequestStateV1::kAccepted: return "accepted";
        case CoordinationRequestStateV1::kSyncing: return "syncing";
        case CoordinationRequestStateV1::kActive: return "active";
        case CoordinationRequestStateV1::kPaused: return "paused";
        case CoordinationRequestStateV1::kSuperseded: return "superseded";
        case CoordinationRequestStateV1::kCompleted: return "completed";
        case CoordinationRequestStateV1::kFailed: return "failed";
        case CoordinationRequestStateV1::kInterrupted: return "interrupted";
    }
    return "accepted";
}

std::string_view to_string(AgentLifecycleActionV1 value) {
    switch (value) {
        case AgentLifecycleActionV1::kPublish: return "publish";
        case AgentLifecycleActionV1::kStop: return "stop";
        case AgentLifecycleActionV1::kRestart: return "restart";
    }
    return "publish";
}

std::string sha256_hex(std::string_view value) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2U);
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2U] = kHex[(digest[index] >> 4U) & 0x0FU];
        result[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
    }
    return result;
}

CoordinationJournalV1::CoordinationJournalV1(std::filesystem::path path)
    : path_(std::move(path)) {}

bool CoordinationJournalV1::reload(std::string* error) {
    JournalLock lock(path_, error);
    return lock.acquired() && reload_unlocked(error);
}

bool CoordinationJournalV1::reload_unlocked(std::string* error) {
    if (invalid_) {
        assign_error("coordination journal requires recovery after an integrity failure", error);
        return false;
    }
    auto fail = [&](const char* detail) {
        invalid_ = true;
        assign_error(detail, error);
        return false;
    };
    std::error_code filesystem_error;
    if (std::filesystem::exists(path_, filesystem_error)) {
        if (filesystem_error || !verify_owner_only(path_, error)) {
            if (filesystem_error) {
                assign_error("coordination journal cannot be inspected", error);
            }
            return false;
        }
    } else if (filesystem_error) {
        assign_error("coordination journal cannot be inspected", error);
        return false;
    } else {
        if (!file_identity_.empty() || read_offset_ != 0) {
            return fail("coordination journal was removed");
        }
        if (error != nullptr) error->clear();
        return true;
    }
    std::string identity;
#ifdef _WIN32
    HANDLE file = CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    const bool inspected = file != INVALID_HANDLE_VALUE
        && GetFileInformationByHandle(file, &info) != FALSE;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!inspected) return fail("coordination journal identity cannot be read");
    identity = std::to_string(info.dwVolumeSerialNumber) + ":"
        + std::to_string(info.nFileIndexHigh) + ":" + std::to_string(info.nFileIndexLow);
#else
#include <fcntl.h>
    struct stat info{};
    if (::stat(path_.c_str(), &info) != 0) return fail("coordination journal identity cannot be read");
    identity = std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino);
#endif
    if (!file_identity_.empty() && identity != file_identity_) {
        return fail("coordination journal was replaced");
    }
    file_identity_ = std::move(identity);
    const auto length = std::filesystem::file_size(path_, filesystem_error);
    if (filesystem_error || length < read_offset_) return fail("coordination journal was truncated");
    if (length == read_offset_) {
        if (error != nullptr) error->clear();
        return true;
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input.is_open()) {
        filesystem_error.clear();
        if (std::filesystem::exists(path_, filesystem_error)) {
            assign_error("coordination journal cannot be opened", error);
            return false;
        }
        if (error != nullptr) {
            error->clear();
        }
        return true;
    }
    input.seekg(static_cast<std::streamoff>(read_offset_));
    std::array<char, kMaxJournalLineBytes + 2> line_buffer{};
    while (input.peek() != std::char_traits<char>::eof()) {
        input.getline(line_buffer.data(), static_cast<std::streamsize>(line_buffer.size()));
        if (!input || input.eof()) return fail("coordination journal has an incomplete or oversized record");
        std::string line(line_buffer.data(), static_cast<std::size_t>(input.gcount()) - 1);
        const auto bytes = line.size() + 1U;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.size() > kMaxJournalLineBytes) {
            return fail("coordination journal line size is invalid");
        }
        CoordinationJournalRecordV1 record;
        if (!parse_record(line, &record, error) || record.sequence != next_sequence_) {
            if (error != nullptr && error->empty()) {
                *error = "coordination journal sequence is not append-only";
            }
            invalid_ = true;
            return false;
        }
        index_record(record);
        records_.push_back(std::move(record));
        ++next_sequence_;
        ++parsed_record_count_;
        read_offset_ += bytes;
    }
    if (!input.eof()) {
        assign_error("coordination journal read failed", error);
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool CoordinationJournalV1::append(
    CoordinationJournalRecordV1 record,
    std::string* error) {
    JournalLock lock(path_, error);
    if (!lock.acquired() || !reload_unlocked(error)) {
        return false;
    }
    return append_unlocked(std::move(record), error);
}

bool CoordinationJournalV1::append_if_authority(
    CoordinationJournalRecordV1 record,
    CoordinationAuthorityV1 expected_authority,
    std::string* error) {
    JournalLock lock(path_, error);
    if (!lock.acquired() || !reload_unlocked(error)) {
        return false;
    }
    if (current_authority_ != expected_authority) {
        assign_error("coordination authority changed before append", error);
        return false;
    }
    if ((record.record_type == CoordinationRecordTypeV1::kRequest
            || (record.record_type == CoordinationRecordTypeV1::kEvidence
                && record.authorization_state == "scoped_request"))
        && !record.request_id.empty()) {
        const auto found = pending_approvals_.find(record.request_id);
        if (record.authorization_state == "explicit_decision") {
            if (found == pending_approvals_.end() || !found->second) {
                assign_error("approval was already decided or is not pending", error);
                return false;
            }
        } else if (found != pending_approvals_.end()) {
            assign_error("duplicate coordination request ID at append", error);
            return false;
        }
    }
    return append_unlocked(std::move(record), error);
}

void CoordinationJournalV1::index_record(const CoordinationJournalRecordV1& record) {
    if (record.record_type == CoordinationRecordTypeV1::kAuthorityTransfer) {
        current_authority_ = record.authority;
    }
    if (!record.request_id.empty()) {
        if (record.authorization_state == "explicit_decision") decided_approvals_.insert(record.request_id);
        pending_approvals_[record.request_id] = record.authorization_state == "pending"
            && !decided_approvals_.contains(record.request_id);
    }
}

bool CoordinationJournalV1::append_unlocked(
    CoordinationJournalRecordV1 record,
    std::string* error) {
    record.sequence = next_sequence_;
    if (!validate_record(record, error)) {
        return false;
    }
    std::error_code filesystem_error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, filesystem_error);
        if (filesystem_error) {
            assign_error("coordination journal directory cannot be created", error);
            return false;
        }
    }
    bool created = false;
    if (!std::filesystem::exists(path_, filesystem_error)) {
        if (filesystem_error) {
            assign_error("coordination journal cannot be inspected", error);
            return false;
        }
        std::ofstream create(path_, std::ios::binary | std::ios::trunc);
        if (!create.is_open()) {
            assign_error("coordination journal cannot be created", error);
            return false;
        }
        create.close();
        created = true;
    }
    if (created && !protect_owner_only(path_, error)) {
        return false;
    }
    if (created && !reload_unlocked(error)) return false;
    std::ofstream output(path_, std::ios::binary | std::ios::app);
    if (!output.is_open()) {
        assign_error("coordination journal cannot be appended", error);
        return false;
    }
    const std::string line = boost::json::serialize(record_json(record));
    if (line.size() > kMaxJournalLineBytes) {
        assign_error("coordination journal line exceeds limit", error);
        return false;
    }
    output << line << '\n';
    output.flush();
    if (!output.good()) {
        assign_error("coordination journal append did not reach storage", error);
        return false;
    }
    output.close();
#ifdef _WIN32
    HANDLE durable_file = CreateFileW(path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool durable = durable_file != INVALID_HANDLE_VALUE && FlushFileBuffers(durable_file);
    if (durable_file != INVALID_HANDLE_VALUE) CloseHandle(durable_file);
#else
    const int durable_file = ::open(path_.c_str(), O_WRONLY);
    const bool durable = durable_file >= 0 && ::fsync(durable_file) == 0;
    if (durable_file >= 0) ::close(durable_file);
#endif
    if (!durable) {
        invalid_ = true;
        assign_error("coordination journal durable flush failed; Agent changes stopped", error);
        return false;
    }
    read_offset_ += line.size() + 1U;
    index_record(record);
    records_.push_back(std::move(record));
    ++next_sequence_;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

const std::filesystem::path& CoordinationJournalV1::path() const noexcept {
    return path_;
}

const std::vector<CoordinationJournalRecordV1>& CoordinationJournalV1::records() const noexcept {
    return records_;
}

std::uint64_t CoordinationJournalV1::next_sequence() const noexcept {
    return next_sequence_;
}

NormalAgentControlStateV1::NormalAgentControlStateV1(NormalAgentControlConfigV1 config)
    : config_(std::move(config)), journal_(config_.journal_path) {}

bool NormalAgentControlStateV1::initialize(std::string* error) {
    providers_.clear();
    projects_.clear();
    incoming_guard_.reset();
    remote_epoch_.clear();
    authority_ = CoordinationAuthorityV1::kNone;
    requests_.clear();
    tasks_.clear();
    acknowledged_sequences_.clear();
    pending_sync_tasks_.clear();
    applied_record_count_ = 0;
    if (!journal_.reload(error)) {
        return false;
    }
    return rebuild(error);
}

bool NormalAgentControlStateV1::refresh(std::string* error) {
    const auto pending_sync_tasks = pending_sync_tasks_;
    const bool sync_required = sync_required_;
    const bool sync_requests_issued = sync_requests_issued_;
    if (!journal_.reload(error)) {
        return false;
    }
    if (!rebuild(error)) {
        return false;
    }
    if (!remote_epoch_.empty()) {
        pending_sync_tasks_ = pending_sync_tasks;
        sync_required_ = sync_required;
        sync_requests_issued_ = sync_requests_issued;
    }
    return true;
}

bool NormalAgentControlStateV1::rebuild(std::string* error) {
    for (; applied_record_count_ < journal_.records().size(); ++applied_record_count_) {
        const auto& record = journal_.records()[applied_record_count_];
        if (record.record_type == CoordinationRecordTypeV1::kAuthorityTransfer) {
            authority_ = record.authority;
        }
        if (!record.request_id.empty()) {
            const bool decided = requests_[record.request_id].approval_decided
                || record.authorization_state == "explicit_decision";
            requests_[record.request_id] = {record.task_id, record.request_state,
                record.authorization_state == "pending" && !decided, decided};
        }
        if (!record.task_id.empty()) {
            acknowledged_sequences_[record.task_id] = std::max(
                acknowledged_sequences_[record.task_id],
                record.acknowledged_event_sequence);
            const auto [task, inserted] = tasks_.try_emplace(record.task_id);
            auto& snapshot = task->second;
            snapshot.task_id = record.task_id;
            // ACKs, status/sync queries and dispatched turns describe local
            // intent, not a new observation of provider execution. Previously
            // their coarse "accepted/active" journal state replaced a peer's
            // awaiting_approval or terminal state with running. This also broke
            // headless API polling and could hide an approval until it timed out.
            const bool peer_observation = record.record_type == CoordinationRecordTypeV1::kReply
                || record.record_type == CoordinationRecordTypeV1::kTerminal
                || (record.record_type == CoordinationRecordTypeV1::kEvidence
                    && record.authorization_state == "unchanged");
            if (!peer_observation) {
                if (inserted) snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kQueued;
                continue;
            }
            if (record.authorization_state == "pending") {
                const auto approval = requests_.find(record.request_id);
                if (approval != requests_.end() && !approval->second.approval_decided)
                    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kAwaitingApproval;
                // An already-decided approval replay cannot revive old state.
                continue;
            }
            switch (record.request_state) {
                case CoordinationRequestStateV1::kCompleted:
                    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kCompleted;
                    break;
                case CoordinationRequestStateV1::kFailed:
                    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kFailed;
                    break;
                case CoordinationRequestStateV1::kInterrupted:
                    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kInterrupted;
                    break;
                case CoordinationRequestStateV1::kPaused:
                case CoordinationRequestStateV1::kSuperseded:
                    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kPaused;
                    break;
                default:
                    snapshot.task_state = redclaw::protocol::AgentTaskStateV1::kRunning;
                    break;
            }
        }
    }
    for (const auto& [task_id, snapshot] : tasks_) {
        if (!terminal_state(snapshot.task_state)) {
            pending_sync_tasks_.insert(task_id);
        }
    }
    sync_required_ = !pending_sync_tasks_.empty();
    sync_requests_issued_ = false;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool NormalAgentControlStateV1::append_record(
    CoordinationJournalRecordV1 record,
    std::string* error) {
    record.authority = authority_;
    record.git_sha = config_.git_sha;
    record.executable_sha256 = config_.executable_sha256;
    if (!journal_.append_if_authority(std::move(record), authority_, error)) {
        return false;
    }
    // append_if_authority may also have consumed another writer's tail. Apply
    // every newly durable record once, in file order, without rescanning history.
    const auto pending_sync = pending_sync_tasks_;
    const bool needed_sync = sync_required_;
    const bool issued_sync = sync_requests_issued_;
    if (!rebuild(error)) {
        return false;
    }
    pending_sync_tasks_ = pending_sync;
    sync_required_ = needed_sync;
    sync_requests_issued_ = issued_sync;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool NormalAgentControlStateV1::transfer_authority(
    CoordinationAuthorityV1 expected,
    CoordinationAuthorityV1 next,
    bool explicitly_selected,
    bool normal_agent_available,
    bool debug_bridge_available,
    std::string_view reason,
    std::uint64_t now_ms,
    std::string* error) {
    if (!refresh(error)) {
        return false;
    }
    if (authority_ != expected) {
        assign_error("coordination authority changed before transfer", error);
        return false;
    }
    if (next != CoordinationAuthorityV1::kNone && !explicitly_selected) {
        assign_error("coordination authority transfer was not explicitly selected", error);
        return false;
    }
    if (next == CoordinationAuthorityV1::kDebugBridge && normal_agent_available) {
        assign_error("normal Agent authority is still available", error);
        return false;
    }
    if (next == CoordinationAuthorityV1::kOutOfBand
        && (normal_agent_available || debug_bridge_available)) {
        assign_error("authenticated runtime authority is still available", error);
        return false;
    }
    if (!valid_token(reason, 128, false)) {
        assign_error("authority transfer reason must be a bounded token", error);
        return false;
    }
    authority_ = next;
    CoordinationJournalRecordV1 record;
    record.recorded_at_ms = now_ms;
    record.record_type = CoordinationRecordTypeV1::kAuthorityTransfer;
    record.authority = next;
    record.authorization_state = explicitly_selected ? "explicit" : "none";
    record.terminal_result = std::string(reason);
    record.git_sha = config_.git_sha;
    record.executable_sha256 = config_.executable_sha256;
    if (!journal_.append_if_authority(std::move(record), expected, error)) {
        authority_ = expected;
        return false;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool NormalAgentControlStateV1::validate_catalog_request(
    const redclaw::protocol::AgentMessageEnvelopeV1& message,
    std::string* error) const {
    if (message.type != redclaw::protocol::AgentMessageTypeV1::kTaskCreate
        && message.type != redclaw::protocol::AgentMessageTypeV1::kTurnStart) {
        return true;
    }
    const auto provider = providers_.find(static_cast<int>(message.provider));
    if (provider == providers_.end() || !provider->second.available
        || provider->second.readiness != redclaw::protocol::AgentProviderReadinessV1::kReady) {
        assign_error("Agent provider is not ready in the authenticated catalog", error);
        return false;
    }
    if (!message.model.empty() && !provider->second.models.empty()
        && !provider->second.models.contains(message.model)) {
        assign_error("Agent model is not in the authenticated capability catalog", error);
        return false;
    }
    const auto project = projects_.find(message.project_id);
    if (project == projects_.end()) {
        assign_error("Agent project ID is not in the opaque project catalog", error);
        return false;
    }
    if (message.work_directory_mode
            == redclaw::protocol::AgentWorkDirectoryModeV1::kIsolatedWorktree
        && !project->second.git_repository) {
        assign_error("isolated worktree requires a registered Git project", error);
        return false;
    }
    return true;
}

bool NormalAgentControlStateV1::prepare_outbound(
    const redclaw::protocol::AgentMessageEnvelopeV1& message,
    std::string_view supersedes_request_id,
    std::string_view authorization_state,
    std::uint64_t now_ms,
    std::string* error) {
    if (!refresh(error)) {
        return false;
    }
    if (authority_ != CoordinationAuthorityV1::kNormalAgent) {
        assign_error("normal Agent does not own coordination authority", error);
        return false;
    }
    if (!controller_message(message.type)) {
        assign_error("message type is not a Controller Agent operation", error);
        return false;
    }
    std::string validation_error;
    if (!redclaw::protocol::validate_agent_message_v1(message, &validation_error)) {
        assign_error(validation_error, error);
        return false;
    }
    if (sync_required_ && state_changing_message(message.type)) {
        assign_error("task synchronization must complete before state-changing work", error);
        return false;
    }
    if (state_changing_message(message.type) && message.request_id.empty()) {
        assign_error("state-changing Agent request requires a request ID", error);
        return false;
    }
    if (!validate_catalog_request(message, error)) {
        return false;
    }
    const auto prior_request = requests_.find(message.request_id);
    const bool approval_decision = message.type
        == redclaw::protocol::AgentMessageTypeV1::kApprovalDecision;
    if (approval_decision && (prior_request == requests_.end()
            || !prior_request->second.awaiting_approval
            || prior_request->second.task_id != message.task_id)) {
        assign_error("approval decision does not match a pending request", error);
        return false;
    }
    if (!approval_decision && !message.request_id.empty() && prior_request != requests_.end()) {
        assign_error("duplicate coordination request ID", error);
        return false;
    }
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskCreate
        && tasks_.contains(message.task_id)) {
        assign_error("duplicate coordination task ID", error);
        return false;
    }
    if (!supersedes_request_id.empty()) {
        const auto old = requests_.find(std::string(supersedes_request_id));
        if (old == requests_.end()
            || old->second.state == CoordinationRequestStateV1::kCompleted
            || old->second.state == CoordinationRequestStateV1::kFailed
            || old->second.state == CoordinationRequestStateV1::kInterrupted
            || old->second.state == CoordinationRequestStateV1::kSuperseded) {
            assign_error("superseded request is not active", error);
            return false;
        }
        CoordinationJournalRecordV1 supersede;
        supersede.recorded_at_ms = now_ms;
        supersede.record_type = CoordinationRecordTypeV1::kSupersede;
        supersede.request_state = CoordinationRequestStateV1::kSuperseded;
        supersede.request_id = std::string(supersedes_request_id);
        supersede.reply_to_request_id = message.request_id;
        supersede.task_id = old->second.task_id;
        supersede.authorization_state = "scoped_request";
        supersede.terminal_result = "superseded";
        if (!append_record(std::move(supersede), error)) {
            return false;
        }
    }
    CoordinationJournalRecordV1 record;
    record.recorded_at_ms = now_ms;
    record.record_type = message.evidence_manifest_name.empty()
        ? CoordinationRecordTypeV1::kRequest
        : CoordinationRecordTypeV1::kEvidence;
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest) {
        const auto snapshot = tasks_.find(message.task_id);
        record.request_state = snapshot == tasks_.end()
            ? CoordinationRequestStateV1::kSyncing
            : request_state_from_agent(snapshot->second.task_state);
    } else {
        record.request_state = sync_required_
            ? CoordinationRequestStateV1::kSyncing
            : CoordinationRequestStateV1::kAccepted;
    }
    record.request_id = message.request_id;
    record.supersedes_request_id = std::string(supersedes_request_id);
    record.task_id = message.task_id;
    record.request_digest_sha256 = sha256_hex(
        redclaw::protocol::serialize_agent_message_v1(message));
    record.authorization_state = approval_decision ? "explicit_decision" : authorization_state.empty()
        ? "scoped_request" : std::string(authorization_state);
    record.evidence_manifest_name = message.evidence_manifest_name;
    record.evidence_sha256 = message.evidence_sha256;
    record.acknowledged_event_sequence = message.acknowledged_event_sequence;
    return append_record(std::move(record), error);
}

bool NormalAgentControlStateV1::observe_remote(
    const redclaw::protocol::AgentMessageEnvelopeV1& message,
    std::uint64_t now_ms,
    std::string* error,
    bool* apply_to_view) {
    if (apply_to_view) *apply_to_view = true;
    if (!remote_epoch_.empty() && message.session_epoch != remote_epoch_) {
        incoming_guard_.reset();
        pending_sync_tasks_.clear();
        for (const auto& [task_id, snapshot] : tasks_) {
            if (!terminal_state(snapshot.task_state)) {
                pending_sync_tasks_.insert(task_id);
            }
        }
        sync_required_ = !pending_sync_tasks_.empty();
        sync_requests_issued_ = false;
    }
    auto candidate_guard = incoming_guard_;
    if (!candidate_guard.accept(message, error)) {
        return false;
    }
    remote_epoch_ = message.session_epoch;
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kCapabilities
        && message.provider != redclaw::protocol::AgentProviderKindV1::kNone) {
        auto& provider = providers_[static_cast<int>(message.provider)];
        if (message.model.empty()) {
            provider.models.clear();
        } else {
            provider.models.insert(message.model);
        }
        provider.available = message.available;
        provider.readiness = message.provider_readiness;
        incoming_guard_ = std::move(candidate_guard);
        return true;
    }
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kProjectCatalog
        && !message.project_id.empty()) {
        projects_[message.project_id] = {.git_repository = message.git_repository};
        incoming_guard_ = std::move(candidate_guard);
        return true;
    }
    if (message.task_id.empty()) {
        incoming_guard_ = std::move(candidate_guard);
        return true;
    }
    const auto previous_ack = acknowledged_event_sequence(message.task_id);
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot
        && message.error_code == "task_not_found" && message.event_kind == "sync_unavailable") {
        const auto request = requests_.find(message.request_id);
        const bool correlated = pending_sync_tasks_.contains(message.task_id)
            && (message.request_id == "sync-" + message.task_id
                || (request != requests_.end() && request->second.task_id == message.task_id));
        if (!correlated || !message.complete || message.gap || message.event_sequence != previous_ack
            || message.task_state != redclaw::protocol::AgentTaskStateV1::kFailed) {
            incoming_guard_ = std::move(candidate_guard);
            if (apply_to_view) *apply_to_view = false;
            return true;
        }
        auto resolved = message;
        const auto prior = task_snapshot(message.task_id);
        if (prior && terminal_state(prior->task_state)) resolved.task_state = prior->task_state;
        CoordinationJournalRecordV1 record;
        record.recorded_at_ms = now_ms;
        record.record_type = CoordinationRecordTypeV1::kTerminal;
        record.request_state = request_state_from_agent(resolved.task_state);
        record.request_id = message.request_id;
        record.reply_to_request_id = message.request_id;
        record.task_id = message.task_id;
        record.authorization_state = "sync_unavailable";
        record.terminal_result = "task_unavailable";
        record.acknowledged_event_sequence = previous_ack;
        if (!append_record(std::move(record), error)) return false;
        tasks_[message.task_id] = std::move(resolved);
        pending_sync_tasks_.erase(message.task_id);
        sync_required_ = !pending_sync_tasks_.empty();
        incoming_guard_ = std::move(candidate_guard);
        return true;
    }
    // A gap snapshot is an explicit durable loss boundary, never an assertion
    // that the missing text was displayed. Ordinary out-of-order events cannot
    // advance the ACK beyond the next contiguous event.
    const bool contiguous = message.event_sequence == previous_ack + 1;
    const bool explicit_gap = message.type
        == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot && message.gap;
    const bool snapshot = message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot;
    const bool missing = !explicit_gap && (message.event_sequence > previous_ack + 1
        || (snapshot && message.event_sequence > previous_ack));
    if ((message.event_sequence > 0 && message.event_sequence <= previous_ack && (!snapshot || explicit_gap))
        || (snapshot && message.event_sequence < previous_ack)
        || missing) {
        // Out-of-order data must be replayed contiguously before it can change
        // UI/task state. Do not journal and redisplay the same history per sync.
        if (missing) {
            if (pending_sync_tasks_.insert(message.task_id).second) sync_requests_issued_ = false;
            sync_required_ = true;
        }
        incoming_guard_ = std::move(candidate_guard);
        if (apply_to_view) *apply_to_view = false;
        return true;
    }
    const auto next_ack = contiguous || explicit_gap
        ? std::max(previous_ack, message.event_sequence) : previous_ack;
    const bool approval_request = message.type == redclaw::protocol::AgentMessageTypeV1::kApprovalRequest
        || (snapshot && message.task_state == redclaw::protocol::AgentTaskStateV1::kAwaitingApproval
            && message.event_kind == "approval_pending" && !message.request_id.empty());
    const auto prior_request = requests_.find(message.request_id);
    const bool decided_approval = approval_request
        && prior_request != requests_.end() && prior_request->second.approval_decided;
    const auto prior_task = task_snapshot(message.task_id);
    const auto effective_state = decided_approval && prior_task ? prior_task->task_state : message.task_state;
    CoordinationJournalRecordV1 record;
    record.recorded_at_ms = now_ms;
    record.record_type = !message.evidence_manifest_name.empty()
        ? CoordinationRecordTypeV1::kEvidence
        : (terminal_state(message.task_state)
            ? CoordinationRecordTypeV1::kTerminal
            : CoordinationRecordTypeV1::kReply);
    record.request_state = request_state_from_agent(effective_state);
    record.request_id = message.request_id;
    record.reply_to_request_id = message.request_id;
    record.task_id = message.task_id;
    record.authorization_state = !decided_approval && approval_request
        ? "pending" : "unchanged";
    record.terminal_result = terminal_state(message.task_state)
        ? std::string(redclaw::protocol::to_string(message.task_state)) : std::string{};
    record.evidence_manifest_name = message.evidence_manifest_name;
    record.evidence_sha256 = message.evidence_sha256;
    record.acknowledged_event_sequence = next_ack;
    if (!append_record(std::move(record), error)) {
        return false;
    }
    incoming_guard_ = std::move(candidate_guard);
    if (decided_approval) {
        if (prior_task) tasks_[message.task_id] = *prior_task;
        if (apply_to_view) *apply_to_view = false;
    } else {
        tasks_[message.task_id] = message;
    }
    if (next_ack > previous_ack) last_sync_request_ms_ = now_ms;
    if (message.type == redclaw::protocol::AgentMessageTypeV1::kTaskSnapshot) {
        pending_sync_tasks_.erase(message.task_id);
    }
    sync_required_ = !pending_sync_tasks_.empty();
    return true;
}

std::vector<redclaw::protocol::AgentMessageEnvelopeV1>
NormalAgentControlStateV1::take_sync_requests(
    std::string_view local_epoch,
    std::uint64_t* next_message_id,
    std::uint64_t now_ms) {
    std::vector<redclaw::protocol::AgentMessageEnvelopeV1> result;
    if (!sync_required_ || next_message_id == nullptr
        || (sync_requests_issued_ && now_ms < last_sync_request_ms_ + 5000)) {
        return result;
    }
    for (const auto& task_id : pending_sync_tasks_) {
        redclaw::protocol::AgentMessageEnvelopeV1 message;
        message.session_epoch = std::string(local_epoch);
        message.message_id = (*next_message_id)++;
        message.sent_at_ms = now_ms;
        message.type = redclaw::protocol::AgentMessageTypeV1::kTaskSyncRequest;
        message.task_id = task_id;
        message.request_id = "sync-" + task_id;
        message.acknowledged_event_sequence = acknowledged_sequences_[task_id];
        result.push_back(std::move(message));
    }
    sync_requests_issued_ = true;
    last_sync_request_ms_ = now_ms;
    return result;
}

void NormalAgentControlStateV1::request_sync(std::string_view task_id) {
    if (!task_id.empty()) {
        pending_sync_tasks_.insert(std::string(task_id));
    } else {
        for (const auto& [id, task] : tasks_) {
            if (!terminal_state(task.task_state)) pending_sync_tasks_.insert(id);
        }
    }
    sync_required_ = !pending_sync_tasks_.empty();
    sync_requests_issued_ = false;
}

std::optional<redclaw::protocol::AgentMessageEnvelopeV1>
NormalAgentControlStateV1::task_snapshot(std::string_view task_id) const {
    const auto found = tasks_.find(std::string(task_id));
    if (found == tasks_.end()) {
        return std::nullopt;
    }
    return found->second;
}

CoordinationAuthorityV1 NormalAgentControlStateV1::authority() const noexcept {
    return authority_;
}

bool NormalAgentControlStateV1::sync_required() const noexcept {
    return sync_required_;
}

std::uint64_t NormalAgentControlStateV1::acknowledged_event_sequence(
    std::string_view task_id) const noexcept {
    const auto found = acknowledged_sequences_.find(std::string(task_id));
    return found == acknowledged_sequences_.end() ? 0 : found->second;
}

const CoordinationJournalV1& NormalAgentControlStateV1::journal() const noexcept {
    return journal_;
}

std::string lifecycle_request_digest_v1(const AgentLifecycleRequestV1& request) {
    return sha256_hex(lifecycle_material(request));
}

bool AgentLifecycleApprovalGateV1::authorize(
    const AgentLifecycleRequestV1& request,
    std::string approval_id,
    std::uint64_t now_ms,
    std::uint64_t ttl_ms,
    bool operator_approved,
    AgentLifecycleApprovalV1* approval,
    std::string* error) {
    if (!operator_approved) {
        assign_error("lifecycle request lacks explicit operator approval", error);
        return false;
    }
    if (!valid_lifecycle_request(request, error)) {
        return false;
    }
    if (!valid_token(approval_id, 128, false) || ttl_ms == 0
        || ttl_ms > kMaxLifecycleApprovalTtlMs || approvals_.contains(approval_id)) {
        assign_error("lifecycle approval identity or TTL is invalid", error);
        return false;
    }
    AgentLifecycleApprovalV1 value;
    value.approval_id = std::move(approval_id);
    value.request_digest_sha256 = lifecycle_request_digest_v1(request);
    value.expires_at_ms = now_ms + ttl_ms;
    approvals_[value.approval_id] = value;
    if (approval != nullptr) {
        *approval = value;
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

bool AgentLifecycleApprovalGateV1::consume(
    const AgentLifecycleRequestV1& request,
    const AgentLifecycleObservedStateV1& observed,
    std::string_view approval_id,
    std::string* error) {
    if (!valid_lifecycle_request(request, error)) {
        return false;
    }
    const auto found = approvals_.find(std::string(approval_id));
    if (found == approvals_.end() || found->second.consumed) {
        assign_error("lifecycle approval is missing or already consumed", error);
        return false;
    }
    if (observed.now_ms > found->second.expires_at_ms) {
        assign_error("lifecycle approval expired", error);
        return false;
    }
    if (found->second.request_digest_sha256 != lifecycle_request_digest_v1(request)) {
        assign_error("lifecycle request changed after approval", error);
        return false;
    }
    if (request.expected_git_sha != observed.git_sha
        || request.candidate_executable_sha256 != observed.candidate_executable_sha256
        || request.target_path_sha256 != observed.target_path_sha256
        || request.target_pid != observed.target_pid) {
        assign_error("lifecycle PID, path, Git, or executable identity is stale", error);
        return false;
    }
    found->second.consumed = true;
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

}  // namespace redclaw::agent
