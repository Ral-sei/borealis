//
// High resolution timing for the Xbox 360 performance logs.
//
// brls::getCPUTimeUsec() is only GetTickCount() on this platform (see
// libretro-common/features/features_cpu.c) and std::chrono::system_clock is no
// better, so every sample below ~1ms used to read back as 0.00 ms.  The XDK's
// QueryPerformanceCounter is the only microsecond grade source available here,
// and it lives in xapilib, which is already linked.
//
// The frame probe splits Application::frame() into the phases that can each
// independently dominate a frame, so one log line every couple of seconds
// answers "where does the time actually go" instead of guessing.
//

#pragma once

#include <cstdint>

// 探针日志总开关（CMake 选项 XBOX360_RUNTIME_PROBES，默认 OFF）。关闭时
// [x360-*] 探针行不输出，默认构建的日志与其它平台一致；打开后恢复全部探针。
// 宏体用 `if (false)` 而非空语句，是为了让参数仍被"使用"，避免
// -Wunused-variable 告警；死分支在 -O3 下被消除，运行期零成本。
#ifndef X360_RUNTIME_PROBES
#define X360_RUNTIME_PROBES 0
#endif

#if defined(PLATFORM_XBOX360) && X360_RUNTIME_PROBES
#define X360_PROBE_INFO(...)  ::brls::Logger::info(__VA_ARGS__)
#define X360_PROBE_DEBUG(...) ::brls::Logger::debug(__VA_ARGS__)
#else
#define X360_PROBE_INFO(...)  do { if (false) ::brls::Logger::info(__VA_ARGS__); } while (0)
#define X360_PROBE_DEBUG(...) do { if (false) ::brls::Logger::debug(__VA_ARGS__); } while (0)
#endif

namespace brls
{
namespace xbox360perf
{

// Rolling mean over one report window.  Header only, usable on any platform.
struct Sample
{
    double totalMs = 0;
    int samples    = 0;

    void add(double ms)
    {
        if (ms < 0) ms = 0;
        totalMs += ms;
        samples++;
    }

    double avg() const
    {
        return samples ? totalMs / samples : 0.0;
    }

    void reset()
    {
        totalMs = 0;
        samples = 0;
    }
};

// Fixed-capacity sample ring for percentile reporting (plan P0).  add() is a
// couple of stores and safe for the hot path; quantile() copies and sorts at
// report time only, so no allocation or logging ever happens while timing.
struct Ring
{
    static constexpr int CAPACITY = 256;

    float samples[CAPACITY] = {};
    int count = 0;
    int next  = 0;

    void add(float ms)
    {
        if (ms < 0) ms = 0;
        samples[next] = ms;
        next = (next + 1) % CAPACITY;
        if (count < CAPACITY) count++;
    }

    void reset()
    {
        count = 0;
        next  = 0;
    }

    // p in [0, 1].  Insertion sort of at most CAPACITY entries, report time
    // only: 65k comparisons every couple of seconds is nothing.
    float quantile(float p) const
    {
        if (count <= 0) return 0.0f;
        float tmp[CAPACITY];
        // When count < CAPACITY only samples[0..count-1] were ever written.
        for (int i = 0; i < count; i++) tmp[i] = samples[i];
        for (int i = 1; i < count; i++)
        {
            const float v = tmp[i];
            int j         = i - 1;
            while (j >= 0 && tmp[j] > v)
            {
                tmp[j + 1] = tmp[j];
                j--;
            }
            tmp[j + 1] = v;
        }
        int idx = (int)(p * (float)(count - 1) + 0.5f);
        if (idx < 0) idx = 0;
        if (idx >= count) idx = count - 1;
        return tmp[idx];
    }

    float maxMs() const
    {
        return quantile(1.0f);
    }
};

#if defined(PLATFORM_XBOX360)

// Microseconds since an arbitrary origin (QPC based, sub-microsecond).
std::uint64_t nowUsec();

// Milliseconds elapsed since nowUsec() was taken at t0.  Unsigned subtraction,
// so a backwards clock step cannot produce a negative sample.
inline double msSince(std::uint64_t t0)
{
    return (double)(nowUsec() - t0) / 1000.0;
}

void begin(int phase);
void end(int phase);

// Logs one line when at least 2s of wall time went by since the previous one,
// then starts a fresh window.  Call once per main loop iteration.
void report();

// Sync 回调归类计时（P0 §3.1）：把 performSyncTasks() 内的耗时按来源拆开。
void syncBegin(int part);
void syncEnd(int part);
// 每次 performSyncTasks() 执行的 sync 任务数（窗口内累加，报告输出总量与单帧最大值）。
void noteSyncTasks(int n);

// P0 §5.11 健康探针：记录一次栈指针，取历史最深（值最小）作为最大栈深基准。
// 用来区分三类故障：栈溢出(0xc00000fd) / 堆耗尽(解码、纹理创建失败) / 堆破坏(写 NULL 崩溃)。
void noteStack(const void* sp);

// P0 直播冻结排查：主线程阶段标记 + 独立看门狗线程。
// 阶段覆盖整个主循环：loop:head/input/tick/report/runloop-evt/delete
// （Application::internalMainLoop）、frame:begin/build/flush/present/sync
// （xbox360perf::begin 内自动打）、sync:<tag>（performSyncTasks 逐任务）、
// delay-task、focus（giveFocus）。任务结束时 stageClear() 记为 "clear" 阶段
// （不清零，保证任意时刻都有阶段可查）。
// 主线程一旦卡住，Logger 输出会一起停摆——看门狗线程直接 DbgPrint 当前
// 阶段：4s 起报、每 4s 重复，阶段恢复时补一行 end。所有停摆行带 HH:MM:SS
// 生成戳：与 Logger 时间对齐后可区分"线程真冻结"与"输出通道背压"。与
// [stall] mutex/cond 探测（cbffmpeg pthread.c / Xbox360Mutex）互相印证：
//   [stall-task] 说"卡在哪"，[stall] spinlock/mutex 说"卡在等谁"。
void stageMark(const char* desc);
void stageClear();

// 主线程身份：internalMainLoop 入口调一次（幂等）。stageMarkIfMain 只在该
// 线程生效——Logger 会被 mpv-wk/HTTP 等任意线程调用，直接 stageMark 会让
// 工作线程的日志把主线程阶段覆盖掉，看门狗随后点错名。非 Xbox 是空内联。
void stageSetMainThread();
void stageMarkIfMain(const char* desc);

#else

inline std::uint64_t nowUsec()
{
    return 0;
}

inline double msSince(std::uint64_t)
{
    return 0;
}

inline void begin(int)
{
}
inline void end(int)
{
}
inline void report()
{
}
inline void syncBegin(int)
{
}
inline void syncEnd(int)
{
}
inline void noteSyncTasks(int)
{
}

inline void noteStack(const void*)
{
}

inline void stageMark(const char*)
{
}

inline void stageClear()
{
}

inline void stageSetMainThread()
{
}

inline void stageMarkIfMain(const char*)
{
}

#endif

// Frame phases, in Application::frame() order.  PHASE_SYNC covers
// Threading::performSyncTasks(), which is where mpv's software render call
// runs.  Constants only — declared outside the platform branch so call sites
// in Application::frame() need no #ifdef (begin()/end() are empty inline
// stubs elsewhere, see above).
enum Phase
{
    PHASE_BEGIN = 0, // beginFrame + clear + nvgBeginFrame + nvgScale
    PHASE_BUILD,     // every view's frame(): NanoVG command building
    PHASE_FLUSH,     // nvgEndFrame() -> the NanoVG flush
    PHASE_PRESENT,   // endFrame() -> swap
    PHASE_SYNC,      // Threading::performSyncTasks() -> mpv render
    PHASE_COUNT,
};

// performSyncTasks() 内的回调归类：图片纹理创建 / 弹幕加载 / 其余（含 mpv render）。
// 声明在平台分支之外，调用点无需 #ifdef。
enum SyncPart
{
    SYNC_IMAGE = 0, // image_helper 的纹理创建回调
    SYNC_DANMAKU,   // DanmakuCore::loadDanmakuData
    SYNC_PART_COUNT,
};

struct SyncScope
{
    explicit SyncScope(int part) : part(part)
    {
        syncBegin(part);
    }
    ~SyncScope()
    {
        syncEnd(part);
    }
    SyncScope(const SyncScope&)            = delete;
    SyncScope& operator=(const SyncScope&) = delete;

    int part;
};

} // namespace xbox360perf
} // namespace brls
