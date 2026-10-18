#pragma once

#if defined(PLATFORM_XBOX360)
#include <chrono>
#include <ctime>
#include <time.h>
#include <xtl.h>

// OXDK/XDK 头里没有 DbgPrint 声明，符号由 xboxkrnl 导出（与 pthread.c 的
// [stall] cond 探测同一个输出通道：主线程日志停摆时它依然能打印）。
extern "C" void DbgPrint(const char* fmt, ...);

// 探针日志总开关（CMake XBOX360_RUNTIME_PROBES，默认 OFF）。本头被大量 TU
// 包含，故在此兜底定义，保证 [stall] 行在默认构建下不输出。
#ifndef X360_RUNTIME_PROBES
#define X360_RUNTIME_PROBES 0
#endif

namespace brls {

// [stall] 行的墙钟时间戳（HH:MM:SS，与 Logger 同源 localtime）：到达顺序
// 晚于生成时刻 = 输出通道背压过；生成即到达 = 线程真冻结。极少调用，
// 手写数字避开 snprintf 可用性问题。XDK 的 time(NULL) 返回垃圾
// （posix_time.c 实测 7.69e18 → localtime 得 NULL → 曾全部显示 00:00:00），
// 与 Logger 同源改用 chrono::system_clock。
inline void xbox360StallStamp(char* buf)   // buf >= 9 字节
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::tm* p = std::localtime(&t);
    int h = 0, m = 0, s = 0;
    if (p) { h = p->tm_hour; m = p->tm_min; s = p->tm_sec; }
    buf[0] = (char)('0' + (h / 10) % 10);
    buf[1] = (char)('0' + h % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + (m / 10) % 10);
    buf[4] = (char)('0' + m % 10);
    buf[5] = ':';
    buf[6] = (char)('0' + (s / 10) % 10);
    buf[7] = (char)('0' + s % 10);
    buf[8] = '\0';
}

// XDK has no usable hosted std::mutex, but async HTTP/image workers still
// cross into Borealis queues. Protect those short critical sections with an
// interlocked spin lock.
class Xbox360Mutex {
public:
    void lock() noexcept {
        if (InterlockedCompareExchange(&value, 1, 0) == 0) {
            owner = (LONG)GetCurrentThreadId();
            return;
        }
        // P0 直播冻结排查：这条自旋路径此前完全静默——持锁者卡住时等待方
        // 在主线程上表现为"在跑但不前进"（cond_wait 有 [stall] 探测，
        // mutex 没有）。先做一小段快速让步（覆盖正常竞争：临界区都很短，
        // 1~2 次让步即能拿到），仍拿不到锁再按 1ms 计时阻塞轮询，持锁 ≥4s
        // 打一行 DbgPrint（带墙钟生成戳），之后每 60s 重复。无竞争时首条
        // 比较指令即成功，热路径零开销。
        //
        // 让步预算从 4096 降到 64：PIX 显示播放期 CPU5/CPU2 上下文切换异常
        // 高、大量线程阻塞在 critical section，而每次竞争失败都先走 4096 次
        // Sleep(0)（= 4096 次内核让步）才转入阻塞，本身就在制造上下文切换。
        // 短临界区 64 次让步足够覆盖，长持锁场景提前进入 1ms 阻塞更省。
        DWORD spins = 0;
        DWORD start = 0;
        DWORD next  = 4000;
        for (;;) {
            if (InterlockedCompareExchange(&value, 1, 0) == 0) {
                owner = (LONG)GetCurrentThreadId();
                return;
            }
            if (spins < 64) {
                spins++;
                Sleep(0);
                continue;
            }
            if (!start) start = GetTickCount();
            Sleep(1);
            const DWORD held = GetTickCount() - start;
            if (held >= next) {
#if X360_RUNTIME_PROBES
                char st[16];
                xbox360StallStamp(st);
                DbgPrint("[stall] %s spinlock m=%p owner=%lu held=%lums",
                         st, (const void*)this, (unsigned long)owner,
                         (unsigned long)held);
#endif
                next += 60000;
            }
        }
    }
    void unlock() noexcept {
        owner = 0;  // 先清 owner 再放锁：InterlockedExchange 是全屏障，顺序可见
        InterlockedExchange(&value, 0);
    }
    bool try_lock() noexcept {
        if (InterlockedCompareExchange(&value, 1, 0) == 0) {
            owner = (LONG)GetCurrentThreadId();
            return true;
        }
        return false;
    }

private:
    volatile LONG value = 0;
    // 持锁线程 id（仅诊断用）：获取成功后回填，等待方打印时可能短暂读到上任。
    volatile LONG owner = 0;
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
