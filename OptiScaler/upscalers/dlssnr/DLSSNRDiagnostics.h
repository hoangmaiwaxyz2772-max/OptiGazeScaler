#pragma once
#include <atomic>
#include <chrono>

// Each call site emits its first three observations, then at most once per
// five seconds. Arguments are evaluated only when an observation is emitted.
namespace DLSSNRDiagnostics
{
struct Gate
{
    std::atomic<unsigned long long> count {0};
    std::atomic<long long> next {0};
    bool Sample()
    {
        const auto n = count.fetch_add(1, std::memory_order_relaxed);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto deadline = next.load(std::memory_order_relaxed);
        if (n >= 3 && now < deadline) return false;
        return next.compare_exchange_strong(deadline, now + 5000, std::memory_order_relaxed);
    }
};
}
#define DLSSNR_DIAG(node, format, ...) do { \
    static DLSSNRDiagnostics::Gate diagnosticGate; \
    if (diagnosticGate.Sample()) \
        LOG_INFO("[DLSSNR_DIAG][" node "] " format, __VA_ARGS__); \
} while (false)
