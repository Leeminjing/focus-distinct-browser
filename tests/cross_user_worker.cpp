// A console-only probe: no user32/UI, Python, profile loading or network login.
#include "cross_user_common.h"
#include <cstdio>

using namespace cross_user;

static DWORD open_result(const wchar_t* path, DWORD access, bool pipe) {
    Handle file(CreateFileW(path, access, pipe ? 0 : FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        pipe ? FILE_FLAG_OVERLAPPED : 0, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    if (!pipe) {
        char byte{}; DWORD read = 0;
        if (!ReadFile(file.value, &byte, 1, &read, nullptr)) return GetLastError();
    }
    return ERROR_SUCCESS;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 7 || std::wstring(argv[1]) != L"--probe") return 2;
    try {
        Handle token;
        require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE, "OpenProcessToken");
        auto groups = token_info(token.value, TokenLogonSid);
        auto logon = reinterpret_cast<TOKEN_GROUPS*>(groups.data());
        require(logon->GroupCount == 1, "TokenLogonSid");
        auto elevation = token_info(token.value, TokenElevation);
        auto statistics = token_info(token.value, TokenStatistics);
        auto luid = reinterpret_cast<TOKEN_STATISTICS*>(statistics.data())->AuthenticationId;
        DWORD endpoint = open_result(argv[2], GENERIC_READ, false);
        DWORD pipe = open_result(argv[3], 0x0012018b, true);
        // Readability proves this actual process ran with access to the staging
        // files. Missing DLLs, bad paths and pipe busy never count as denials.
        std::string control = read_file(argv[4]);
        std::string nonce;
        for (auto p = argv[6]; *p; ++p) {
            require((*p >= L'0' && *p <= L'9') || (*p >= L'a' && *p <= L'f'), "Nonce");
            nonce += static_cast<char>(*p);
        }
        std::string output = "{\"user_sid\":\"" + user_sid(token.value) + "\",\"logon_sid\":\""
            + sid_text(logon->Groups[0].Sid) + "\",\"administrator\":" + (administrator() ? "true" : "false")
            + ",\"elevated\":" + (reinterpret_cast<TOKEN_ELEVATION*>(elevation.data())->TokenIsElevated ? "true" : "false")
            + ",\"authentication_id\":\"" + std::to_string(luid.HighPart) + ":" + std::to_string(luid.LowPart)
            + "\",\"endpoint_error\":" + std::to_string(endpoint) + ",\"pipe_error\":" + std::to_string(pipe)
            + ",\"control_readable\":" + (control == nonce ? "true" : "false") + ",\"nonce\":\"" + nonce + "\"}";
        write_file(argv[5], output);
        return 0;
    } catch (const std::exception& error) { fprintf(stderr, "%s\n", error.what()); return 1; }
}
