#pragma once
#include <windows.h>
#include <objbase.h>
#include "../json.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <thread>

namespace distinct {
// The pipe handle belongs to the server. It is reused only after the STA has
// stopped/joined its writer and acknowledged detachment of this connection.
struct PipeConnection {
    HANDLE pipe = INVALID_HANDLE_VALUE, peer = nullptr, acknowledged = nullptr;
    uint64_t generation = 0;
    std::atomic<bool> alive{true};
    PipeConnection();
    ~PipeConnection();
};
struct TransportMessage {
    enum class Kind { attach, input, detach } kind;
    std::shared_ptr<PipeConnection> connection;
    std::string line;
};

class ReconnectServer {
    HWND dispatcher_;
    UINT message_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE, slots_ = nullptr;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::wstring user_sid_, logon_sid_, pipe_name_;
    std::string host_id_, credential_, executable_, created_;
    std::filesystem::path directory_, endpoint_;
    bool directory_created_ = false;
    void run();
    bool post(TransportMessage::Kind kind, const std::shared_ptr<PipeConnection>& connection, std::string line = {});
    bool acknowledge(const std::shared_ptr<PipeConnection>& connection);
public:
    ReconnectServer(HWND dispatcher, UINT message, const std::filesystem::path& root);
    ~ReconnectServer();
    ReconnectServer(const ReconnectServer&) = delete;
    ReconnectServer& operator=(const ReconnectServer&) = delete;
    Json bootstrap() const;
    const std::string& host_id() const { return host_id_; }
    void release_slot() { ReleaseSemaphore(slots_, 1, nullptr); }
};
}
