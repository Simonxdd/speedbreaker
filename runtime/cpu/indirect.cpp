// SpeedBreaker runtime. GPL-3.0-or-later (see COPYING).
#include <stdafx.h>
#include "indirect.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct JumpPairHash {
    size_t operator()(const std::pair<uint32_t, uint32_t>& p) const noexcept {
        return (static_cast<size_t>(p.first) << 32) ^ static_cast<size_t>(p.second);
    }
};

class IndirectJumpLogger {
public:
    static IndirectJumpLogger& instance() {
        static IndirectJumpLogger logger;
        return logger;
    }

    void record(uint32_t source, uint32_t target) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::pair<uint32_t, uint32_t> pair{source, target};
        if (seen_.insert(pair).second) {
            pending_.push_back(pair);
            has_pending_ = true;
        }
    }

    ~IndirectJumpLogger() {
        stop_.store(true, std::memory_order_release);
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        flush_now();
    }

private:
    IndirectJumpLogger() {
        file_ = std::fopen("PPC_CALL_INDIRECT_FUNC.txt", "a");
        worker_ = std::thread([this]() { run(); });
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_.load(std::memory_order_acquire)) {
            cv_.wait_for(lock, std::chrono::seconds(1), [this]() {
                return stop_.load(std::memory_order_acquire);
            });
            flush_locked();
        }
    }

    void flush_now() {
        std::lock_guard<std::mutex> lock(mutex_);
        flush_locked();
        if (file_) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    void flush_locked() {
        if (!has_pending_ || pending_.empty()) {
            return;
        }
        if (!file_) {
            file_ = std::fopen("PPC_CALL_INDIRECT_FUNC.txt", "a");
        }
        if (file_) {
            for (const auto& [src, dst] : pending_) {
                std::fprintf(file_, "0x%08X 0x%08X\n", src, dst);
            }
            std::fflush(file_);
        }
        pending_.clear();
        has_pending_ = false;
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_set<std::pair<uint32_t, uint32_t>, JumpPairHash> seen_;
    std::vector<std::pair<uint32_t, uint32_t>> pending_;
    bool has_pending_ = false;
    FILE* file_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

} // namespace

// The running release's code end (region::Select moves it for the PAL code).
uint64_t g_indirectCodeEnd = PPC_CODE_BASE + PPC_CODE_SIZE;

void PPCCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    PPCFunc* fn = nullptr;
    if (target >= PPC_CODE_BASE && target < g_indirectCodeEnd && (target & 3) == 0)
        fn = PPC_LOOKUP_FUNC(base, target);
    if (fn == nullptr)
    {
        fprintf(stderr, "[cpu] indirect call to %08X, which is not a recompiled function (lr=%08X r3=%08X r4=%08X)\n",
            target, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32);
        fflush(stderr);
        std::abort();
    }

    IndirectJumpLogger::instance().record(static_cast<uint32_t>(ctx.lr), target);
    fn(ctx, base);
}