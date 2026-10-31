#pragma once
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace distinct {
// Opt-in local numeric timings only: never log URLs, tokens or image contents.
// The regular executable does no trace I/O unless this variable is configured.
class PerfTrace {
    HANDLE file_ = INVALID_HANDLE_VALUE;
    std::mutex mutex_;
    PerfTrace() {
        wchar_t path[32768]{};
        DWORD count = GetEnvironmentVariableW(L"DISTINCT_BROWSER_PERF_TRACE", path, 32768);
        if (count && count < 32768)
            file_ = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
public:
    ~PerfTrace() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    static PerfTrace& instance() { static PerfTrace trace; return trace; }
    static unsigned long long now() {
        if (instance().file_ == INVALID_HANDLE_VALUE) return 0;
        return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static void record(const char* stage, unsigned long long begin, size_t bytes = 0) {
        if (!begin) return;
        auto end = now();
        char line[220]{};
        int length = sprintf_s(line, "{\"stage\":\"%s\",\"elapsed_us\":%.3f,\"bytes\":%llu}\n",
            stage, (end - begin) / 1000.0, static_cast<unsigned long long>(bytes));
        auto& trace = instance();
        std::lock_guard<std::mutex> lock(trace.mutex_);
        DWORD written{};
        if (length > 0) WriteFile(trace.file_, line, length, &written, nullptr);
    }
};
}
