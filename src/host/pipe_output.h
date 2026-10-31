#pragma once
#include <windows.h>
#include "../json.h"
#include "perf_trace.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace distinct {
// A stalled pipe reader must not stall a browser window. Conversion to JSONL
// stays on the STA caller; only bytes and the failure callback cross threads.
class PipeOutput {
    HANDLE pipe_;
    std::function<void()> disconnected_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> queue_;
    size_t bytes_ = 0;
    bool stopped_ = false;
    bool overlapped_ = false;
    std::thread thread_;
public:
    PipeOutput(HANDLE pipe, std::function<void()> disconnected, bool overlapped = false) :
        pipe_(pipe), disconnected_(std::move(disconnected)), overlapped_(overlapped), thread_([this] { run(); }) {}
    ~PipeOutput() {
        { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
        wake_.notify_one();
        while (WaitForSingleObject(thread_.native_handle(), 50) == WAIT_TIMEOUT)
            CancelSynchronousIo(thread_.native_handle());
        thread_.join();
    }
    void send(const Json& value) {
        auto trace_begin = PerfTrace::now();
        auto line = winrt::to_string(value.Stringify()) + "\n";
        if (trace_begin && value.HasKey(L"result") && value.GetNamedValue(L"result").ValueType() == JsonType::Object &&
            value.GetNamedObject(L"result").HasKey(L"image"))
            PerfTrace::record("response_json_utf8", trace_begin, line.size());
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_) return;
        if (bytes_ + line.size() > 64 * 1024 * 1024) {
            stopped_ = true; disconnected_();
        } else { bytes_ += line.size(); queue_.push_back(std::move(line)); }
        wake_.notify_one();
    }
private:
    bool write(const char* data, DWORD size, DWORD& written) {
        if (!overlapped_) return WriteFile(pipe_, data, size, &written, nullptr) != FALSE;
        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) return false;
        OVERLAPPED operation{}; operation.hEvent = event;
        bool ok = WriteFile(pipe_, data, size, &written, &operation) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            for (;;) {
                if (WaitForSingleObject(event, 25) == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(pipe_, &operation, &written, FALSE) != FALSE; break;
                }
                bool stop;
                { std::lock_guard<std::mutex> lock(mutex_); stop = stopped_; }
                if (stop) {
                    CancelIoEx(pipe_, &operation);
                    GetOverlappedResult(pipe_, &operation, &written, TRUE); break;
                }
            }
        }
        CloseHandle(event); return ok;
    }
    void run() {
        for (;;) {
            std::string line;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
                if (stopped_) return;
                line = std::move(queue_.front()); queue_.pop_front(); bytes_ -= line.size();
            }
            size_t offset = 0;
            while (offset < line.size()) {
                DWORD written = 0;
                if (!write(line.data() + offset, static_cast<DWORD>(line.size() - offset), written) || !written) {
                    { std::lock_guard<std::mutex> lock(mutex_); stopped_ = true; }
                    disconnected_(); return;
                }
                offset += written;
            }
        }
    }
};
}
