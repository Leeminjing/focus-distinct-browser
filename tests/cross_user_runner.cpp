// Elevated test-only broker. Never accepts passwords or existing account names.
#include "cross_user_common.h"
#include <lm.h>
#include <bcrypt.h>
#include <userenv.h>
#include <filesystem>
#include <cstdio>
#include <ctime>

using namespace cross_user;
namespace fs = std::filesystem;

static std::wstring wide(const std::string& value) { return {value.begin(), value.end()}; }
static std::string ascii(const std::wstring& value) {
    std::string result;
    for (wchar_t character : value) { require(character <= 127, "ASCII test identifier"); result += static_cast<char>(character); }
    return result;
}
static void net_check(NET_API_STATUS code, const char* operation) {
    if (code) throw std::runtime_error(std::string(operation) + ": " + std::to_string(code));
}
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const std::wstring& sddl) {
        require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "Test DACL");
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
struct Password {
    std::wstring value = L"aA1!";
    Password() {
        BYTE bytes[32]{};
        require(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "Password randomness");
        for (auto byte : bytes) {
            value += L"0123456789abcdef"[byte >> 4]; value += L"0123456789abcdef"[byte & 15];
        }
        SecureZeroMemory(bytes, sizeof(bytes));
    }
    void clear() { if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t)); value.clear(); }
    ~Password() { clear(); }
};
struct Account {
    std::wstring name, marker;
    std::string sid;
    bool created = false;
    bool was_created = false;
    bool profile_removed = true;
    bool inspect(bool capture = false) {
        LPBYTE buffer = nullptr;
        auto status = NetUserGetInfo(nullptr, name.c_str(), 23, &buffer);
        if (status) return false;
        auto info = reinterpret_cast<USER_INFO_23*>(buffer);
        std::string actual = sid_text(info->usri23_user_sid);
        bool owned = info->usri23_comment && marker == info->usri23_comment && (sid.empty() || sid == actual);
        if (capture && owned) sid = actual;
        NetApiBufferFree(buffer); return owned;
    }
    void create(Password& password) {
        USER_INFO_1 info{};
        info.usri1_name = name.data(); info.usri1_password = password.value.data();
        info.usri1_priv = USER_PRIV_USER; info.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT;
        info.usri1_comment = marker.data();
        DWORD parameter = 0;
        net_check(NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&info), &parameter), "NetUserAdd");
        created = was_created = true; require(inspect(true), "Capture created account identity");
        USER_INFO_1017 expiration{};
        expiration.usri1017_acct_expires = static_cast<DWORD>(std::time(nullptr) + 900);
        net_check(NetUserSetInfo(nullptr, name.c_str(), 1017, reinterpret_cast<LPBYTE>(&expiration), &parameter), "Expire temporary account");
        // Resolve the builtin Users alias by SID, independent of Windows language.
        PSID users = nullptr;
        require(ConvertStringSidToSidW(L"S-1-5-32-545", &users) != FALSE, "Users SID");
        DWORD length = 0, domain_length = 0; SID_NAME_USE use{};
        LookupAccountSidW(nullptr, users, nullptr, &length, nullptr, &domain_length, &use);
        std::vector<wchar_t> alias(length), domain(domain_length);
        BOOL ok = LookupAccountSidW(nullptr, users, alias.data(), &length, domain.data(), &domain_length, &use);
        LocalFree(users); require(ok != FALSE, "Resolve Users alias");
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
        require(GetComputerNameW(computer, &size) != FALSE, "Computer name");
        auto member = std::wstring(computer) + L"\\" + name;
        LOCALGROUP_MEMBERS_INFO_3 entry{member.data()};
        auto status = NetLocalGroupAddMembers(nullptr, alias.data(), 3, reinterpret_cast<LPBYTE>(&entry), 1);
        if (status != ERROR_MEMBER_IN_ALIAS) net_check(status, "Add test user to Users");
    }
    bool cleanup() noexcept {
        if (!created) return true;
        try {
            if (!inspect(true)) return false; // Never delete a colliding/replaced account.
            auto key_name = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList\\" + wide(sid);
            HKEY profile = nullptr;
            auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, key_name.c_str(), 0, KEY_READ, &profile);
            if (status == ERROR_SUCCESS) {
                RegCloseKey(profile);
                profile_removed = DeleteProfileW(wide(sid).c_str(), nullptr, nullptr) != FALSE;
            } else if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) profile_removed = false;
            if (NetUserDel(nullptr, name.c_str()) != NERR_Success) return false;
            LPBYTE buffer = nullptr;
            auto absent = NetUserGetInfo(nullptr, name.c_str(), 23, &buffer);
            if (buffer) NetApiBufferFree(buffer);
            created = absent != NERR_UserNotFound;
            return !created;
        } catch (...) { return false; }
    }
};

static fs::path physical_directory(HANDLE handle) {
    wchar_t path[32768]{};
    DWORD length = GetFinalPathNameByHandleW(handle, path, 32768, 0);
    require(length && length < 32768, "Resolve staging root");
    return fs::path(path);
}
static void create_file(const fs::path& path, const std::string& text) {
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr));
    require(file.value != INVALID_HANDLE_VALUE, "Create owned staging file");
    DWORD count = 0;
    require(WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &count, nullptr) && count == text.size(), "Stage data");
}
static std::wstring quoted(const std::wstring& value) {
    require(value.find(L'"') == std::wstring::npos && !value.empty() && value.back() != L'\\', "Probe argument");
    return L"\"" + value + L"\"";
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 7 || std::wstring(argv[1]) != L"--run") return 2;
    std::wstring nonce(argv[5]), owner(argv[6]);
    if (nonce.size() != 32 || nonce.find_first_not_of(L"0123456789abcdef") != std::wstring::npos) return 2;
    PSID original_owner = nullptr;
    if (!ConvertStringSidToSidW(owner.c_str(), &original_owner)) return 2;
    LocalFree(original_owner);
    if (!administrator()) {
        write_file(argv[4], "{\"status\":\"not_run\",\"reason\":\"administrator_required\"}"); return 77;
    }
    Account account; account.name = L"DBSec_" + nonce.substr(0, 12);
    account.marker = L"DistinctBrowser.CrossUser." + nonce;
    Handle process, thread, root;
    fs::path stage;
    bool stage_created = false;
    std::string probe, error, process_sid;
    try {
        Password password; account.create(password);
        write_file(argv[4], "{\"status\":\"running\",\"account_created\":true,\"account_deleted\":false,\"temporary_account\":\""
            + ascii(account.name) + "\",\"expected_user_sid\":\"" + account.sid + "\"}");
        Handle token; require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) != FALSE, "Broker token");
        auto broker_sid = user_sid(token.value);
        require(wide(account.sid) != owner && account.sid != broker_sid, "Different local user");
        wchar_t data[32768]{};
        require(GetEnvironmentVariableW(L"ProgramData", data, 32768) > 0 && fs::path(data).is_absolute(), "ProgramData");
        root.value = CreateFileW(data, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(root.value != INVALID_HANDLE_VALUE, "Open staging root");
        BY_HANDLE_FILE_INFORMATION information{};
        require(GetFileInformationByHandle(root.value, &information) && (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && !(information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT), "Staging root type");
        stage = physical_directory(root.value) / (L"DistinctBrowser-Security-" + nonce);
        Security access(L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + wide(broker_sid) + L")(A;OICI;GRGX;;;" + wide(account.sid) + L")");
        require(CreateDirectoryW(stage.c_str(), &access.attributes) != FALSE, "Create private staging directory");
        stage_created = true;
        wchar_t module[32768]{}; require(GetModuleFileNameW(nullptr, module, 32768) > 0, "Broker image");
        auto worker = stage / L"worker.exe", control = stage / L"control.txt", result = stage / L"probe.json";
        require(CopyFileW((fs::path(module).parent_path() / L"cross-user-worker.exe").c_str(), worker.c_str(), TRUE) != FALSE, "Stage worker");
        Security executable_access(L"D:P(A;;FA;;;SY)(A;;FA;;;" + wide(broker_sid) + L")(A;;GRGX;;;" + wide(account.sid) + L")");
        require(SetFileSecurityW(worker.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            executable_access.descriptor) != FALSE, "Worker image DACL");
        create_file(control, ascii(nonce)); create_file(result, "");
        Security output(L"D:P(A;;FA;;;SY)(A;;FA;;;" + wide(broker_sid) + L")(A;;GW;;;" + wide(account.sid) + L")");
        require(SetFileSecurityW(result.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, output.descriptor) != FALSE, "Probe output DACL");
        auto command = quoted(worker.wstring()) + L" --probe " + quoted(argv[2]) + L" " + quoted(argv[3]) + L" "
            + quoted(control.wstring()) + L" " + quoted(result.wstring()) + L" " + nonce;
        require(command.size() < 1024, "CreateProcessWithLogonW command length");
        wchar_t windows[MAX_PATH]{}; require(GetWindowsDirectoryW(windows, MAX_PATH) > 0, "Windows directory");
        std::wstring environment = L"SystemRoot=" + std::wstring(windows);
        environment.push_back(0); environment += L"TEMP=" + stage.wstring();
        environment.push_back(0); environment += L"TMP=" + stage.wstring(); environment.append(2, L'\0');
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION child{};
        // Flag zero authenticates locally, unlike LOGON_NETCREDENTIALS_ONLY.
        // The console-only worker inherits the desktop; no SwitchDesktop or
        // interactive desktop/window-station ACL changes are performed.
        require(CreateProcessWithLogonW(account.name.c_str(), L".", password.value.c_str(), 0, worker.c_str(), command.data(),
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, environment.data(), stage.c_str(), &startup, &child) != FALSE,
            "CreateProcessWithLogonW");
        password.clear(); process.value = child.hProcess; thread.value = child.hThread;
        Handle child_token;
        require(OpenProcessToken(process.value, TOKEN_QUERY | TOKEN_DUPLICATE, &child_token.value) != FALSE, "Actual child token");
        process_sid = user_sid(child_token.value);
        auto elevated = token_info(child_token.value, TokenElevation);
        require(process_sid == account.sid && !administrator(child_token.value)
            && !reinterpret_cast<TOKEN_ELEVATION*>(elevated.data())->TokenIsElevated, "Actual child must be the standard test user");
        DWORD parent_session = 0, child_session = 0;
        require(ProcessIdToSessionId(GetCurrentProcessId(), &parent_session) && ProcessIdToSessionId(child.dwProcessId, &child_session)
            && parent_session == child_session, "Same Windows session");
        require(ResumeThread(thread.value) != static_cast<DWORD>(-1), "Resume verified probe");
        require(WaitForSingleObject(process.value, 30000) == WAIT_OBJECT_0, "Probe timeout");
        DWORD code = 1; require(GetExitCodeProcess(process.value, &code) && code == 0, "Probe exit");
        probe = read_file(result.wstring()); require(!probe.empty(), "Probe report");
    } catch (const std::exception& failure) { error = failure.what(); }
    // Stop only this exact child handle before account/profile/file cleanup.
    if (process.value && WaitForSingleObject(process.value, 0) != WAIT_OBJECT_0) {
        if (!TerminateProcess(process.value, 1) || WaitForSingleObject(process.value, 10000) != WAIT_OBJECT_0) error = "Probe cleanup failed";
    }
    bool account_clean = account.cleanup();
    bool account_deleted = account.was_created && account_clean;
    bool stage_removed = true;
    if (stage_created) {
        for (const wchar_t* name : {L"worker.exe", L"control.txt", L"probe.json"}) {
            if (!DeleteFileW((stage / name).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) stage_removed = false;
        }
        if (!RemoveDirectoryW(stage.c_str())) stage_removed = false; // No scans/recursive deletion.
    }
    auto status = error.empty() && account_deleted && account.profile_removed && stage_removed ? "executed" : "failed";
    std::string output = "{\"status\":\"" + std::string(status) + "\",\"expected_user_sid\":\"" + account.sid
        + "\",\"actual_process_sid\":\"" + process_sid + "\",\"account_created\":" + (account.was_created ? "true" : "false")
        + ",\"account_deleted\":" + (account_deleted ? "true" : "false")
        + ",\"profile_removed\":" + (account.profile_removed ? "true" : "false")
        + ",\"stage_removed\":" + (stage_removed ? "true" : "false") + ",\"reason\":\"" + error
        + "\",\"temporary_account\":\"" + ascii(account.name)
        + "\",\"probe\":" + (probe.empty() ? "null" : probe) + "}";
    try { write_file(argv[4], output); }
    catch (const std::exception& failure) { fprintf(stderr, "%s\n", failure.what()); return 1; }
    return std::string(status) == "executed" ? 0 : 1;
}
