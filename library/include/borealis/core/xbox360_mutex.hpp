#pragma once

#if defined(PLATFORM_XBOX360)
namespace brls {

// M2 is intentionally single-threaded: OXDK libc++ disables threads and
// mutexes. Keep the existing queue/RAII code source-compatible while making
// lock operations no-ops on this target.
class Xbox360Mutex {
public:
    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
};

using PlatformMutex = Xbox360Mutex;

class Xbox360LockGuard {
public:
    explicit Xbox360LockGuard(PlatformMutex& mutex) noexcept
        : mutex(mutex) {
        this->mutex.lock();
    }

    ~Xbox360LockGuard() {
        this->mutex.unlock();
    }

    Xbox360LockGuard(const Xbox360LockGuard&) = delete;
    Xbox360LockGuard& operator=(const Xbox360LockGuard&) = delete;

private:
    PlatformMutex& mutex;
};

using PlatformLockGuard = Xbox360LockGuard;

class Xbox360UniqueLock {
public:
    Xbox360UniqueLock() noexcept = default;
    explicit Xbox360UniqueLock(PlatformMutex& mutex) noexcept
        : mutex(&mutex) {
        this->mutex->lock();
    }

    ~Xbox360UniqueLock() {
        if (this->mutex)
            this->mutex->unlock();
    }

    Xbox360UniqueLock(const Xbox360UniqueLock&) = delete;
    Xbox360UniqueLock& operator=(const Xbox360UniqueLock&) = delete;

    Xbox360UniqueLock(Xbox360UniqueLock&& other) noexcept
        : mutex(other.mutex) {
        other.mutex = nullptr;
    }

    Xbox360UniqueLock& operator=(Xbox360UniqueLock&& other) noexcept {
        if (this != &other) {
            if (this->mutex)
                this->mutex->unlock();
            this->mutex = other.mutex;
            other.mutex = nullptr;
        }
        return *this;
    }

private:
    PlatformMutex* mutex = nullptr;
};

using PlatformUniqueLock = Xbox360UniqueLock;

} // namespace brls
#else
#include <mutex>
namespace brls {
using PlatformMutex = std::mutex;
using PlatformLockGuard = std::lock_guard<PlatformMutex>;
using PlatformUniqueLock = std::unique_lock<PlatformMutex>;
}
#endif
