#include "reconnect.h"
#include <bcrypt.h>
#include <sddl.h>
#include <wincrypt.h>
#include <vector>
#include <stdexcept>

namespace distinct {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Security {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    Security(const std::wstring& sid, const wchar_t* rights) {
        auto sddl = L"D:P(A;;" + std::wstring(rights) + L";;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            throw Error("endpoint_security", "Cannot create private access descriptor");
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
std::wstring logon_sid(HANDLE process) {
    Handle token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) throw Error("endpoint_security", "Cannot query process identity");
    DWORD size = 0;
    GetTokenInformation(token.value, TokenLogonSid, nullptr, 0, &size);
    std::vector<BYTE> bytes(size);
    if (!size || !GetTokenInformation(token.value, TokenLogonSid, bytes.data(), size, &size))
        throw Error("endpoint_security", "Cannot query logon identity");
    auto groups = reinterpret_cast<TOKEN_GROUPS*>(bytes.data());
    LPWSTR text = nullptr;
    if (groups->GroupCount != 1 || !ConvertSidToStringSidW(groups->Groups[0].Sid, &text))
        throw Error("endpoint_security", "Missing logon identity");
    std::wstring result(text); LocalFree(text); return result;
}
std::string identifier() {
    GUID id{}; checked(CoCreateGuid(&id), "CoCreateGuid");
    wchar_t text[40]{}; StringFromGUID2(id, text, 40);
    return winrt::to_string(std::wstring(text + 1, 36));
}
std::string base64(const BYTE* data, DWORD size) {
    DWORD length = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length))
        throw Error("endpoint_security", "Cannot encode protected credential");
    std::string text(length, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text.data(), &length))
        throw Error("endpoint_security", "Cannot encode protected credential");
    text.resize(length); return text;
}
bool equal_secret(const std::string& first, const std::string& second) {
    if (first.size() != second.size()) return false;
    unsigned char different = 0;
    for (size_t i = 0; i < first.size(); ++i) different |= first[i] ^ second[i];
    return different == 0;
}
// Every pending operation is cancelled and reaped before its stack/event dies.
bool wait_io(HANDLE pipe, OVERLAPPED& operation, const std::atomic<bool>& stopping,
             DWORD& count, uint64_t deadline, HANDLE peer = nullptr) {
    for (;;) {
        if (WaitForSingleObject(operation.hEvent, 25) == WAIT_OBJECT_0)
            return GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE;
        if (stopping || GetTickCount64() >= deadline || (peer && WaitForSingleObject(peer, 0) == WAIT_OBJECT_0)) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &count, TRUE);
            return false;
        }
    }
}
bool read_bytes(HANDLE pipe, char* buffer, DWORD capacity, DWORD& count,
                const std::atomic<bool>& stopping, uint64_t deadline, HANDLE peer = nullptr) {
    if (stopping || GetTickCount64() >= deadline || (peer && WaitForSingleObject(peer, 0) == WAIT_OBJECT_0)) return false;
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return false;
    OVERLAPPED operation{}; operation.hEvent = event.value;
    if (ReadFile(pipe, buffer, capacity, &count, &operation)) return count != 0;
    return GetLastError() == ERROR_IO_PENDING && wait_io(pipe, operation, stopping, count, deadline, peer) && count;
}
}

PipeConnection::PipeConnection() {
    acknowledged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!acknowledged) throw Error("startup_failed", "Cannot allocate connection acknowledgement");
}
PipeConnection::~PipeConnection() { if (peer) CloseHandle(peer); if (acknowledged) CloseHandle(acknowledged); }

ReconnectServer::ReconnectServer(HWND dispatcher, UINT message, const std::filesystem::path& root)
    : dispatcher_(dispatcher), message_(message) {
    try {
        host_id_ = identifier(); logon_sid_ = logon_sid(GetCurrentProcess());
        pipe_name_ = L"\\\\.\\pipe\\DistinctBrowser-" + winrt::to_hstring(host_id_);
        BYTE random[32]{};
        if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            throw Error("endpoint_security", "Cannot create reconnect credential");
        credential_ = base64(random, sizeof(random));
        // Clients can read/write data and attributes, never create another
        // server instance. Keep this first server handle for the entire host.
        Security pipe_security(logon_sid_, L"0x0012018b");
        pipe_ = CreateNamedPipeW(pipe_name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &pipe_security.attributes);
        if (pipe_ == INVALID_HANDLE_VALUE) throw Error("endpoint_security", "Cannot reserve private pipe");
        slots_ = CreateSemaphoreW(nullptr, 128, 128, nullptr);
        if (!slots_) throw Error("startup_failed", "Cannot create transport queue");
        wchar_t image[32768]{}; DWORD length = static_cast<DWORD>(std::size(image));
        if (!QueryFullProcessImageNameW(GetCurrentProcess(), 0, image, &length)) throw Error("startup_failed", "Cannot identify host image");
        executable_ = winrt::to_string(std::wstring(image, length));
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) throw Error("startup_failed", "Cannot identify host process");
        created_ = std::to_string((static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime);
        std::filesystem::create_directories(root);
        directory_ = root / winrt::to_hstring(host_id_).c_str();
        Security file_security(logon_sid_, L"FA");
        if (!CreateDirectoryW(directory_.c_str(), &file_security.attributes)) throw Error("endpoint_security", "Cannot create private endpoint directory");
        directory_created_ = true;
        endpoint_ = directory_ / L"endpoint.json";
        DATA_BLOB plain{static_cast<DWORD>(credential_.size()), reinterpret_cast<BYTE*>(credential_.data())}, protected_data{};
        if (!CryptProtectData(&plain, L"Distinct Browser reconnect", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &protected_data))
            throw Error("endpoint_security", "Cannot protect reconnect credential");
        auto protected_text = base64(protected_data.pbData, protected_data.cbData); LocalFree(protected_data.pbData);
        auto description = object({{L"host_id", js(host_id_)}, {L"pipe_name", js(winrt::to_string(pipe_name_))},
            {L"pid", jn(GetCurrentProcessId())}, {L"process_created", js(created_)}, {L"executable", js(executable_)},
            {L"protocol_version", jn(1)}, {L"credential_dpapi", js(protected_text)}});
        auto bytes = winrt::to_string(description.Stringify());
        auto temporary = directory_ / L"endpoint.tmp";
        {
            Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, &file_security.attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            DWORD written = 0;
            if (file.value == INVALID_HANDLE_VALUE || !WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size() || !FlushFileBuffers(file.value))
                throw Error("endpoint_security", "Cannot write private endpoint");
        }
        if (!MoveFileExW(temporary.c_str(), endpoint_.c_str(), MOVEFILE_WRITE_THROUGH)) throw Error("endpoint_security", "Cannot publish endpoint");
        thread_ = std::thread([this] { run(); });
    } catch (...) {
        if (pipe_ != INVALID_HANDLE_VALUE) { CloseHandle(pipe_); pipe_ = INVALID_HANDLE_VALUE; }
        if (slots_) { CloseHandle(slots_); slots_ = nullptr; }
        if (!endpoint_.empty()) DeleteFileW(endpoint_.c_str());
        if (directory_created_) { DeleteFileW((directory_ / L"endpoint.tmp").c_str()); RemoveDirectoryW(directory_.c_str()); }
        throw;
    }
}
ReconnectServer::~ReconnectServer() {
    stopping_ = true;
    CancelIoEx(pipe_, nullptr);
    if (thread_.joinable()) thread_.join();
    CloseHandle(pipe_); CloseHandle(slots_);
    DeleteFileW(endpoint_.c_str()); RemoveDirectoryW(directory_.c_str());
}
Json ReconnectServer::bootstrap() const {
    return object({{L"event", js("ready")}, {L"protocol_version", jn(1)}, {L"pid", jn(GetCurrentProcessId())},
        {L"transport", js("named_pipe")}, {L"host_id", js(host_id_)}, {L"endpoint", js(winrt::to_string(endpoint_.wstring()))}});
}
bool ReconnectServer::post(TransportMessage::Kind kind, const std::shared_ptr<PipeConnection>& connection, std::string line) {
    if (kind == TransportMessage::Kind::input) {
        const auto deadline = GetTickCount64() + 30000;
        while (!stopping_) {
            if (!connection->alive || WaitForSingleObject(connection->peer, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) {
                connection->alive = false; return false;
            }
            if (WaitForSingleObject(slots_, 25) == WAIT_OBJECT_0) break;
        }
        if (stopping_) return false;
    }
    auto message = std::make_unique<TransportMessage>(TransportMessage{kind, connection, std::move(line)});
    if (PostMessageW(dispatcher_, message_, 0, reinterpret_cast<LPARAM>(message.get()))) { message.release(); return true; }
    if (kind == TransportMessage::Kind::input) ReleaseSemaphore(slots_, 1, nullptr);
    return false;
}
bool ReconnectServer::acknowledge(const std::shared_ptr<PipeConnection>& connection) {
    while (!stopping_)
        if (WaitForSingleObject(connection->acknowledged, 25) == WAIT_OBJECT_0) return true;
    return false;
}
void ReconnectServer::run() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    uint64_t generation = 0;
    while (!stopping_) {
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event.value) break;
        OVERLAPPED operation{}; operation.hEvent = event.value;
        BOOL accepted = ConnectNamedPipe(pipe_, &operation);
        DWORD error = accepted ? ERROR_SUCCESS : GetLastError(), count = 0;
        if (error == ERROR_IO_PENDING)
            accepted = wait_io(pipe_, operation, stopping_, count, UINT64_MAX);
        else accepted = accepted || error == ERROR_PIPE_CONNECTED;
        if (!accepted) { if (stopping_) break; DisconnectNamedPipe(pipe_); continue; }
        std::shared_ptr<PipeConnection> connection;
        bool attached = false;
        try {
            connection = std::make_shared<PipeConnection>(); connection->pipe = pipe_;
            ULONG pid = 0;
            if (!GetNamedPipeClientProcessId(pipe_, &pid)) throw Error("identity_failed", "Unknown pipe client");
            connection->peer = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
            if (!connection->peer || logon_sid(connection->peer) != logon_sid_) throw Error("identity_failed", "Client logon does not match");
            std::string handshake;
            const auto deadline = GetTickCount64() + 5000;
            // No over-read: bytes following the handshake remain protocol data.
            char ch = 0;
            do {
                if (!read_bytes(pipe_, &ch, 1, count, stopping_, deadline, connection->peer)) throw Error("handshake_failed", "Handshake ended");
                if (ch != '\n') handshake += ch;
                if (handshake.size() > 8192) throw Error("handshake_failed", "Oversized handshake");
            } while (ch != '\n');
            auto request = Json::Parse(winrt::to_hstring(handshake));
            if (string(request, L"host_id", 128) != host_id_ || number(request, L"protocol_version", 1, 1) != 1 ||
                !equal_secret(string(request, L"credential", 128), credential_)) throw Error("handshake_failed", "Invalid handshake");
            connection->generation = ++generation;
            attached = post(TransportMessage::Kind::attach, connection);
            if (!attached || !acknowledge(connection)) throw Error("connection_lost", "Cannot attach");
            std::string line; bool oversized = false;
            auto input_deadline = GetTickCount64() + 30000;
            char buffer[8192];
            while (!stopping_ && connection->alive && read_bytes(pipe_, buffer, sizeof(buffer), count, stopping_, input_deadline, connection->peer)) {
                for (DWORD i = 0; i < count; ++i) {
                    if (buffer[i] == '\n') {
                        if (oversized || !line.empty()) {
                            if (!post(TransportMessage::Kind::input, connection, oversized ? "{}" : std::move(line))) throw Error("connection_lost", "Input queue closed");
                        }
                        line.clear(); oversized = false;
                        input_deadline = GetTickCount64() + 30000;
                    } else if (!oversized) {
                        if (line.size() >= 1024 * 1024) { oversized = true; line.clear(); }
                        else line += buffer[i];
                    }
                }
            }
        } catch (...) { /* Authentication failures reveal no browser state. */ }
        if (connection) connection->alive = false;
        CancelIoEx(pipe_, nullptr);
        if (attached && post(TransportMessage::Kind::detach, connection)) acknowledge(connection);
        DisconnectNamedPipe(pipe_);
    }
}
}
