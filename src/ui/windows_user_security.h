#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <sddl.h>
#include <memory>
#include <string>
#include <vector>

namespace redclaw::ui::win {
struct CloseFile { void operator()(void* value) const { CloseHandle(value); } };
struct FreeLocal { void operator()(void* value) const { LocalFree(value); } };
using File = std::unique_ptr<void, CloseFile>;
using Local = std::unique_ptr<void, FreeLocal>;
struct UserSecurity {
    std::vector<unsigned char> token_user;
    Local descriptor;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    PSID sid() const { return reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid; }
    bool load(bool require_unelevated = false) {
        HANDLE value = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &value)) return false;
        File token(value);
        DWORD bytes = 0;
        if (require_unelevated) {
            TOKEN_ELEVATION elevation{};
            if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &bytes)
                || elevation.TokenIsElevated) return false;
        }
        (void)GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (!bytes) return false;
        token_user.resize(bytes);
        if (!GetTokenInformation(token.get(), TokenUser, token_user.data(), bytes, &bytes)) return false;
        LPWSTR name = nullptr;
        if (!ConvertSidToStringSidW(sid(), &name)) return false;
        Local owner(name);
        const std::wstring sddl = L"O:" + std::wstring(name) + L"D:P(A;OICI;FA;;;" + name + L")";
        PSECURITY_DESCRIPTOR security = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &security, nullptr)) return false;
        descriptor.reset(security); attributes.lpSecurityDescriptor = security; return true;
    }
};
inline bool owner_only_object(HANDLE file, PSID current_user, bool directory) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info))
        || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        || ((info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return false;
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR security = nullptr;
    if (GetSecurityInfo(file, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &owner, nullptr, &acl, nullptr, &security) != ERROR_SUCCESS) return false;
    Local descriptor(security);
    SECURITY_DESCRIPTOR_CONTROL control = 0; DWORD revision = 0;
    if (!owner || !EqualSid(owner, current_user) || !acl || !GetSecurityDescriptorControl(security, &control, &revision)
        || !(control & SE_DACL_PROTECTED)) return false;
    bool allowed = false;
    for (DWORD index = 0; index < acl->AceCount; ++index) {
        void* raw = nullptr;
        if (!GetAce(acl, index, &raw)) return false;
        const auto* header = static_cast<ACE_HEADER*>(raw);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
        auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (!EqualSid(&ace->SidStart, current_user)) return false;
        allowed = true;
    }
    return allowed;
}
} // namespace redclaw::ui::win
#endif
