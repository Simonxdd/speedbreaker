// runtime/gpu/gpu_sync.h
#pragma once
#include <mutex>
#include <condition_variable>

namespace gpu_sync {
    inline std::mutex& GetMutex() {
        static std::mutex m;
        return m;
    }

    inline std::condition_variable& GetCV() {
        static std::condition_variable cv;
        return cv;
    }

    inline void NotifyGpuProgress() {
        GetCV().notify_all();
    }
}