#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <stdexcept>

namespace cross_user {
inline void require(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + ": " + std::to_string(GetLastError()));
}
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
inline std::string sid_text(PSID sid) {
    LPSTR text = nullptr;
    require(ConvertSidToStringSidA(sid, &text) != FALSE, "ConvertSidToStringSid");
    std::string result(text); LocalFree(text); return result;
}
inline std::vector<BYTE> token_info(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD size = 0;
    GetTokenInformation(token, kind, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    require(size && GetTokenInformation(token, kind, bytes.data(), size, &size), "GetTokenInformation");
    return bytes;
}
inline std::string user_sid(HANDLE token) {
    auto bytes = token_info(token, TokenUser);
    return sid_text(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
}
inline bool administrator(HANDLE token = nullptr) {
    Handle impersonation;
    if (token) {
        require(DuplicateToken(token, SecurityIdentification, &impersonation.value) != FALSE, "DuplicateToken");
        token = impersonation.value;
    }
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr; BOOL member = FALSE;
    require(AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins) != FALSE, "AllocateAndInitializeSid");
    BOOL ok = CheckTokenMembership(token, admins, &member); FreeSid(admins);
    require(ok != FALSE, "CheckTokenMembership"); return member != FALSE;
}
inline std::string read_file(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Open report");
    char buffer[16385]{}; DWORD size = 0;
    require(ReadFile(file.value, buffer, sizeof(buffer), &size, nullptr) && size < sizeof(buffer), "Read report");
    return {buffer, size};
}
inline void write_file(const std::wstring& path, const std::string& text) {
    // All reports are pre-created by the owner. Never create arbitrary paths.
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Open output");
    DWORD written = 0;
    require(WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
        && written == text.size() && SetEndOfFile(file.value) && FlushFileBuffers(file.value), "Write output");
}
}
