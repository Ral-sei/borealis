//
// See xbox360_perf.hpp.
//

#include <borealis/platforms/xbox360/xbox360_perf.hpp>

#if defined(PLATFORM_XBOX360)

// 运行时诊断探针开关，由 CMake 的 XBOX360_RUNTIME_PROBES 注入
// （library/borealis/library/cmake/toolchain.cmake）。默认关闭：这些探针是纯
// 诊断，release 热路径不该为它们付费。打开后恢复 [heap-slow] 分配计时与堆
// 可用量试探，行为与历史构建一致。
#ifndef X360_RUNTIME_PROBES
#define X360_RUNTIME_PROBES 0
#endif

#include <borealis/core/application.hpp>
#include <borealis/core/cache_helper.hpp>
#include <borealis/core/logger.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <windows.h>

// OXDK/XDK 头里没有 DbgPrint 声明 —— xboxkrnl 导出（同 cbffmpeg pthread.c
// 的 [stall] 探测一个输出通道，主线程日志停摆时依然可用）。
extern "C" void DbgPrint(const char* fmt, ...);

namespace brls
{
namespace xbox360perf
{

namespace
{
// The frequency is fixed for the lifetime of the process.  Kept as a plain
// long long so the struct is constant initialised, no static guards needed.
long long g_freq = 0;

void ensureClock()
{
    if (g_freq > 0) return;
    LARGE_INTEGER f;
    f.QuadPart = 0;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart > 0 ? f.QuadPart : 1;
}

Sample g_phase[PHASE_COUNT];
Ring g_ring[PHASE_COUNT];
std::uint64_t g_phaseStart[PHASE_COUNT];
Sample g_sync[SYNC_PART_COUNT];
Ring g_syncRing[SYNC_PART_COUNT];
std::uint64_t g_syncStart[SYNC_PART_COUNT];
int g_syncTasks    = 0;
int g_syncTasksMax = 0;
std::uint64_t g_windowStart = 0;
bool g_windowOpen           = false;
// P0 §5.11 健康探针状态
std::uintptr_t g_stackTop = 0;  // 主线程栈基线（首次采样）
std::uintptr_t g_stackMin = 0;  // 历史最深栈帧
int g_windowCount         = 0;  // 已输出的报告窗口数
std::size_t g_lastHeap    = 0;  // 最近一次堆探针结果（字节）
} // namespace

std::uint64_t nowUsec()
{
    ensureClock();
    LARGE_INTEGER c;
    c.QuadPart = 0;
    QueryPerformanceCounter(&c);

    // Two 64-bit divisions; the remainder stays below g_freq, so multiplying
    // it by 1e6 cannot overflow a 64-bit long long even for a multi GHz
    // frequency.
    const long long ticks = c.QuadPart;
    return (std::uint64_t)(ticks / g_freq) * 1000000ULL +
           (std::uint64_t)((ticks % g_freq) * 1000000LL / g_freq);
}

// --- 主线程阶段标记 + 看门狗（P0 直播冻结排查）--------------------------
#if X360_RUNTIME_PROBES
namespace
{
// 只有主线程写入、看门狗线程读取；诊断用途，不做同步（字节拷贝 + DWORD
// 对齐读写在 PPC 上安全，最坏情况是字符串中间撕裂一帧，不影响定位）。
volatile char g_stage[128] = {};
volatile DWORD g_stageTick = 0;   // 阶段进入时刻（0 = 无阶段）
volatile LONG g_stageArmed = 0;   // 看门狗只启动一次
volatile DWORD g_mainTid = 0;     // 主线程 id（stageSetMainThread 记录）

// 第 5 轮盲区修复（round-4 实测）：探针自身的 Logger 调用在主线程会打
// "log:*" 阶段标记，直接喂饱 g_stageTick —— 5.5s 的 hints 段被自己的探针
// 日志切成 <1.5s 的碎片，end 行一条都出不来。现拆成两套时钟：
//   g_stage/g_stageTick  —— "主线程活动"视角，任何标记（含 log:*）都刷新，
//                            看门狗 busy 行用它（活动着就不算死锁）；
//   g_phaseDesc/g_phaseTick —— "真实阶段"视角，log:* 不参与；end 行用它，
//                            于是 hints:create 这种阶段即使内部疯狂打探针
//                            日志，也能在结束时留下一条实名总时长。
volatile char g_phaseDesc[128] = {};
volatile DWORD g_phaseTick = 0;

// 停摆阈值：历史冻结 5~81s，20s 会漏掉 4~19s 的短段；帧内 build 峰值
// ~1s（p95 171ms），4s 起报不会误报。
const DWORD STALL_THRESHOLD = 4000;

// end 行单独用低阈值（第 4 轮）：busy 看门狗只看"单阶段 ≥4s"，而实测的
// 7.2s 冻结（00:14:23 live_title_box 焦点切换）全程零 stall 行——说明它被
// 拆成若干 <4s 的阶段（每段 stageNoteRelease 都不到 4s，看门狗也没机会），
// busy/end 两头都看不见。end 阈值降到 1.5s 后，任何 ≥1.5s 的阶段在切换时
// 都会留一行带时长的记录，7.2s 冻结至少被切成几段实名行暴露出来。
const DWORD STAGE_END_THRESHOLD = 1500;

void stageCopyDesc(char* desc, size_t n)
{
    size_t i = 0;
    for (; i < n - 1; i++)
    {
        desc[i] = g_stage[i];
        if (!desc[i]) break;
    }
    desc[i < n ? i : n - 1] = '\0';
}

void stageCopyPhaseDesc(char* desc, size_t n)
{
    size_t i = 0;
    for (; i < n - 1; i++)
    {
        desc[i] = g_phaseDesc[i];
        if (!desc[i]) break;
    }
    desc[i < n ? i : n - 1] = '\0';
}

// "log:*" 阶段只刷新活动时钟，不动阶段时钟（见 g_phaseDesc 注释）。
bool isLogDesc(const char* desc)
{
    return desc && desc[0] == 'l' && desc[1] == 'o' && desc[2] == 'g' && desc[3] == ':' && desc[4] != '\0';
}

// 墙钟时间戳（HH:MM:SS，与 Logger 同源 localtime）。用来区分两种停摆：
// 行的生成时刻 = 它在文件里的位置 → 线程真冻结；生成时刻早于到达顺序
// （夹进更晚的日志后一起涌出）→ 输出通道背压（日志本身把主线程堵住了）。
// XDK CRT 的 time()/_time64() 返回垃圾（posix_time.c 实测 7.69e18，localtime
// 得 NULL → 戳曾全部显示 00:00:00），改用与 Logger 完全同源的
// chrono::system_clock（Logger 时间戳在本机一直正确）。
void stageStamp(char* buf, size_t n)
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::tm* p    = std::localtime(&t);
    int h = 0, m = 0, s = 0;
    if (p)
    {
        h = p->tm_hour;
        m = p->tm_min;
        s = p->tm_sec;
    }
    std::snprintf(buf, n, "%02d:%02d:%02d", h, m, s);
}

// 换阶段时发现上一"真实阶段"已经持续 ≥1.5s：补一行带生成戳的 end，
// 给停摆窗口一个闭合端点。注意用 g_phaseTick/g_phaseDesc（log:* 不参与
// 的那套时钟）—— round-4 用活动时钟时，探针自己的日志会把 5.5s 阶段
// 切成碎片，end 行一条都出不来。
void stageNoteRelease()
{
    const DWORD t = g_phaseTick;
    if (!t) return;
    const DWORD now = GetTickCount();
    if (now - t < STAGE_END_THRESHOLD) return;
    char desc[128];
    char stamp[16];
    stageCopyPhaseDesc(desc, sizeof(desc));
    stageStamp(stamp, sizeof(stamp));
    DbgPrint("[stall-task] %s end %lus at %s", stamp,
             (unsigned long)((now - t) / 1000), desc);
}

DWORD WINAPI stageWatchdog(LPVOID)
{
    DWORD lastPrint = 0;
    for (;;)
    {
        Sleep(1000);
        const DWORD t = g_stageTick;
        if (!t)
        {
            lastPrint = 0;
            continue;
        }
        const DWORD now  = GetTickCount();
        const DWORD held = now - t;
        if (held >= STALL_THRESHOLD && (lastPrint == 0 || now - lastPrint >= STALL_THRESHOLD))
        {
            lastPrint = now;
            char desc[128];
            char stamp[16];
            stageCopyDesc(desc, sizeof(desc));
            stageStamp(stamp, sizeof(stamp));
            DbgPrint("[stall-task] %s busy %lus at %s", stamp,
                     (unsigned long)(held / 1000), desc);
        }
    }
    return 0;
}
} // namespace
#endif // X360_RUNTIME_PROBES

void stageMark(const char* desc)
{
#if !X360_RUNTIME_PROBES
    // 探针关闭：阶段机/看门狗整套不参与——主线程不再为此每帧/每条日志写
    // volatile 全局、也不再创建看门狗线程（PIX Thread0 LHS/IPC 与
    // critical-section 阻塞的减负项）。
    (void)desc;
#else
    const DWORD now = GetTickCount();
    const DWORD tick = now ? now : 1;  // GetTickCount 理论上可能返回 0，规避"未标记"

    if (!isLogDesc(desc))
    {
        // 真实阶段：先结账上一阶段（≥1.5s 打 end 行），再更新阶段时钟。
        stageNoteRelease();
        size_t i = 0;
        if (desc)
            for (; desc[i] && i < sizeof(g_phaseDesc) - 1; i++) g_phaseDesc[i] = desc[i];
        g_phaseDesc[i] = '\0';
        g_phaseTick = tick;
    }

    // 活动时钟：log:* 也刷新（主线程确实在动，看门狗不该报 busy）。
    size_t i = 0;
    if (desc)
        for (; desc[i] && i < sizeof(g_stage) - 1; i++) g_stage[i] = desc[i];
    g_stage[i] = '\0';
    g_stageTick = tick;
    if (InterlockedCompareExchange(&g_stageArmed, 1, 0) == 0)
        CreateThread(nullptr, 0, stageWatchdog, nullptr, 0, nullptr);
#endif // X360_RUNTIME_PROBES
}

// 不再清零：阶段间隙改记一个 "clear" 阶段并刷新时刻。若清成 0，任务间隙
// 里发生的停摆（恰是 Logger/输出通道背压的高危点）会让看门狗直接跳过，
// 整段停摆无痕——这正是物种 B（sync 之外的冻结）此前的盲区。
void stageClear()
{
    stageMark("clear");
}

void stageSetMainThread()
{
#if X360_RUNTIME_PROBES
    if (!g_mainTid) g_mainTid = GetCurrentThreadId();
#endif
}

// 只有主线程的调用会改阶段：Logger 模板在任意线程实例化，工作线程的日志
// 若也打阶段，看门狗会把"主线程卡在 X"报成工作线程刚写的 "log:*"。
void stageMarkIfMain(const char* desc)
{
#if X360_RUNTIME_PROBES
    const DWORD tid = g_mainTid;
    if (!tid || GetCurrentThreadId() != tid) return;
    stageMark(desc);
#else
    (void)desc;
#endif
}

void begin(int phase)
{
    if (phase < 0 || phase >= PHASE_COUNT) return;
    // 帧内阶段同时是看门狗的阶段名：主线程卡在 build/flush/present 里时，
    // [stall-task] 直接点名（此前只有 sync 任务有阶段，帧阶段是盲区）。
    static const char* const kPhaseName[PHASE_COUNT] = {
        "frame:begin", "frame:build", "frame:flush", "frame:present", "frame:sync",
    };
    stageMark(kPhaseName[phase]);
    g_phaseStart[phase] = nowUsec();
}

void end(int phase)
{
    if (phase < 0 || phase >= PHASE_COUNT) return;
    const std::uint64_t start = g_phaseStart[phase];
    g_phaseStart[phase]       = 0;
    if (!start) return;
    const double ms = msSince(start);
    g_phase[phase].add(ms);
    g_ring[phase].add((float)ms);
}

void syncBegin(int part)
{
    if (part < 0 || part >= SYNC_PART_COUNT) return;
    g_syncStart[part] = nowUsec();
}

void syncEnd(int part)
{
    if (part < 0 || part >= SYNC_PART_COUNT) return;
    const std::uint64_t start = g_syncStart[part];
    g_syncStart[part]         = 0;
    if (!start) return;
    const double ms = msSince(start);
    g_sync[part].add(ms);
    g_syncRing[part].add((float)ms);
}

void noteSyncTasks(int n)
{
    if (n <= 0) return;
    g_syncTasks += n;
    if (n > g_syncTasksMax) g_syncTasksMax = n;
}

// P0 §5.11 健康探针：g_stackTop = 主线程栈基线（首次采样，地址最浅），
// g_stackMin = 历史最深栈帧。差值就是本进程用过的最大栈深，
// 用来判断 0xc00000fd（栈溢出）是不是主线程的问题。
void noteStack(const void* sp)
{
    const std::uintptr_t v = reinterpret_cast<std::uintptr_t>(sp);
    if (v == 0) return;
    if (g_stackTop == 0)
    {
        g_stackTop = v;
        g_stackMin = v;
        return;
    }
    if (v > g_stackTop) g_stackTop = v;
    if (v < g_stackMin) g_stackMin = v;
}

// 每 10s 试分配一次大块内存并立刻释放，报告当前还能拿到的最大连续堆块。
// 结果掉到 0/1MB 说明堆已耗尽 —— 对应图片解码失败、纹理创建失败。
#if X360_RUNTIME_PROBES
static size_t probeHeap()
{
    static const std::size_t kSizes[] = { 64u << 20, 32u << 20, 16u << 20, 8u << 20,
                                          4u << 20,  2u << 20,  1u << 20,  512u << 10 };
    for (std::size_t s : kSizes)
    {
        void* p = std::malloc(s);
        if (p)
        {
            std::free(p);
            return s;
        }
    }
    return 0;
}
#endif

void report()
{
#if !X360_RUNTIME_PROBES
    // 探针关闭：帧率/分位数/健康行不再输出。begin/end 仍记录相位（无输出、极廉价）。
    return;
#endif
    const std::uint64_t now = nowUsec();
    if (!g_windowOpen)
    {
        g_windowOpen  = true;
        g_windowStart = now;
        return;
    }

    const double windowMs = (double)(now - g_windowStart) / 1000.0;
    if (windowMs < 2000.0) return;

    const int frames = g_phase[PHASE_BEGIN].samples;
    if (frames > 0)
    {
        const double fps = frames * 1000.0 / windowMs;
        brls::Logger::info(
            "[x360-perf] frame {:.0f} fps ({} frames) | begin {:.2f} build {:.2f} flush {:.2f} present {:.2f} sync {:.2f} ms",
            fps, frames, g_phase[PHASE_BEGIN].avg(), g_phase[PHASE_BUILD].avg(), g_phase[PHASE_FLUSH].avg(),
            g_phase[PHASE_PRESENT].avg(), g_phase[PHASE_SYNC].avg());
        // Second line: the averages above hide the intermittent stalls the
        // plan is chasing, so report the spread from the sample rings.
        brls::Logger::info(
            "[x360-perf] p50/p95/max ms | begin {:.2f}/{:.2f}/{:.2f} build {:.2f}/{:.2f}/{:.2f} flush {:.2f}/{:.2f}/{:.2f} present {:.2f}/{:.2f}/{:.2f} sync {:.2f}/{:.2f}/{:.2f}",
            g_ring[PHASE_BEGIN].quantile(0.5f), g_ring[PHASE_BEGIN].quantile(0.95f), g_ring[PHASE_BEGIN].maxMs(),
            g_ring[PHASE_BUILD].quantile(0.5f), g_ring[PHASE_BUILD].quantile(0.95f), g_ring[PHASE_BUILD].maxMs(),
            g_ring[PHASE_FLUSH].quantile(0.5f), g_ring[PHASE_FLUSH].quantile(0.95f), g_ring[PHASE_FLUSH].maxMs(),
            g_ring[PHASE_PRESENT].quantile(0.5f), g_ring[PHASE_PRESENT].quantile(0.95f), g_ring[PHASE_PRESENT].maxMs(),
            g_ring[PHASE_SYNC].quantile(0.5f), g_ring[PHASE_SYNC].quantile(0.95f), g_ring[PHASE_SYNC].maxMs());
    }

    if (g_syncTasks > 0 || g_sync[SYNC_IMAGE].samples > 0 || g_sync[SYNC_DANMAKU].samples > 0)
    {
        // other 口径修正（§5.5-8）：原先用 avg 相减——image/danmaku 的取样数是
        // "回调次数"，PHASE_SYNC 是 "帧数"，两者分母不同，差值无意义甚至恒为 0。
        // 改为总量口径：窗口内 sync 总耗时 - 图片总 - 弹幕总，再除以帧数，
        // 得到"每帧平均的未归类 sync 耗时"（含 mpv render 及其它未打标签回调）。
        const double framesD = frames > 0 ? (double)frames : 1.0;
        double other = (g_phase[PHASE_SYNC].totalMs - g_sync[SYNC_IMAGE].totalMs - g_sync[SYNC_DANMAKU].totalMs) / framesD;
        if (other < 0) other = 0;
        double imgAvg = g_sync[SYNC_IMAGE].avg();
        double danAvg = g_sync[SYNC_DANMAKU].avg();
        brls::Logger::info(
            "[x360-perf] sync split avg/max ms | image {:.2f}/{:.2f} danmaku {:.2f}/{:.2f} other {:.2f}/frame | tasks {} (max {})",
            imgAvg, g_syncRing[SYNC_IMAGE].maxMs(), danAvg, g_syncRing[SYNC_DANMAKU].maxMs(), other, g_syncTasks,
            g_syncTasksMax);
    }

    // P0 §5.11 健康探针：栈深 / 堆可用量 / 纹理缓存规模，用来把"封面解码失败、
    // 设置卡死、0xc0000005 写 NULL 崩溃"归到 栈溢出 / 堆耗尽 / 堆破坏 三者之一。
    // 堆探针每 10s（5 个窗口）跑一次，其余窗口沿用上次结果。
    g_windowCount++;
#if X360_RUNTIME_PROBES
    if (g_windowCount % 5 == 0)
        g_lastHeap = probeHeap();
#endif
    const std::size_t stackKiB = (g_stackTop > g_stackMin) ? (g_stackTop - g_stackMin) / 1024 : 0;
    const std::size_t texCount = brls::TextureCache::instance().cache.getCacheList().size();
    // 有多少条目引用计数已归零（可被 LRU 淘汰）。若 size 顶到容量上限而 evictable 恒为 0，
    // 说明引用计数仍在泄漏（§5.11 的换图泄漏路径未修干净）。
    std::size_t evictable = 0;
    for (const auto& n : brls::TextureCache::instance().cache.getCacheList())
        if (n.count <= 0)
            evictable++;
    brls::Logger::info(
        "[x360-perf] health | stack max {} KiB (sp {:#x}..{:#x}) | heap>={} KiB | tex cache {} ({} evictable)",
        stackKiB, g_stackMin, g_stackTop, g_lastHeap / 1024, texCount, evictable);

    g_windowStart = now;
    for (int i = 0; i < PHASE_COUNT; i++)
    {
        g_phase[i].reset();
        g_ring[i].reset();
    }
    for (int i = 0; i < SYNC_PART_COUNT; i++)
    {
        g_sync[i].reset();
        g_syncRing[i].reset();
    }
    g_syncTasks    = 0;
    g_syncTasksMax = 0;
}

} // namespace xbox360perf
} // namespace brls

#if X360_RUNTIME_PROBES
// --- [heap-slow] 全局分配/释放计时（第 5 轮堆锁争用假说）--------------------
// round-4 分段实测：焦点切换冻结里，构造侧 attrs 段（registerCommonAttributes
// ≈90 次 map 插入 = 分配密集）120~660ms、析构侧 child dtor（成员 map/set 析构
// + operator delete = 释放密集）120~660ms 对称变慢，而零分配的 style 段恒 0ms、
// 单次分配的 yoga 段大多 0ms 偶发 13~20ms —— 不是均匀 CPU 饥饿，指向 CRT 堆锁
// 被别的线程（直播软解跑着的 FFmpeg/TLS/网络线程）长期占用。
// 钩法：operator new/delete 的实现在 OXDK builtins.c（强符号，直接 malloc/free），
// 不能在本文件再定义同名替换（链接期重复符号）。改为 toolchain.cmake 给
// xbox360_runtime target 加 -Dmalloc/free=探针名，builtins.c 的调用改道到
// 下面两个 extern "C" 包装，包装计时后再调真正的 CRT malloc/free。
// 5ms 阈值：既抓"持锁者"（单次长操作），也留下"受害者"集群的时刻。
// ffmpeg 侧对应探针在 cbffmpeg libavutil/mem.c（av_malloc/av_realloc/av_free）。
// 只走 DbgPrint 不走 Logger —— Logger 的 fmt 分配会再次进包装，且 DbgPrint 无锁。
namespace
{
// 令牌桶限流：64 枚、每 5s 回填；≥50ms 的大鱼绕过令牌必放行。
// round-5b 教训：一次性 256 枚预算在启动风暴（14:05 两波共 100+ 条）后耗尽，
// 12s 析构风暴期间整段失明 —— 持锁者恰好在预算耗尽后才出现。
volatile long g_heapBudget = 64;
volatile DWORD g_heapTokTick = 0;

void heapNote(const char* op, unsigned long bytes, DWORD t0)
{
    const DWORD d = GetTickCount() - t0;
    if (d < 5) return;
    if (d < 50)
    {
        const DWORD now = GetTickCount();
        if (now - g_heapTokTick >= 5000)
        {
            g_heapTokTick = now;
            __sync_lock_test_and_set(&g_heapBudget, 64);
        }
        if (__sync_fetch_and_add(&g_heapBudget, 0) <= 0) return;
        __sync_fetch_and_sub(&g_heapBudget, 1);
    }
    // tid 用于区分持锁者/受害者：持锁者打出长操作后，紧随的等待者集群
    // 若来自其它线程即可对上时间序列。
    if (bytes)
        DbgPrint("[heap-slow] %s %lu B %lu ms tid %lu", op, bytes,
                 (unsigned long)d, (unsigned long)GetCurrentThreadId());
    else
        DbgPrint("[heap-slow] %s %lu ms tid %lu", op, (unsigned long)d,
                 (unsigned long)GetCurrentThreadId());
}
} // namespace

// builtins.c（-Dmalloc/free 改名后）唯一期望的符号；签名与其改名后的声明一致。
// OOM 语义与原 builtins.c 完全相同：原样返回 NULL（不 abort，不抛异常）。
extern "C" void* x360_heap_probe_malloc(unsigned long n)
{
    const DWORD t0 = GetTickCount();
    void* p        = std::malloc(n);
    heapNote("new", n, t0);
    return p;
}

extern "C" void x360_heap_probe_free(void* p)
{
    if (!p) return;
    const DWORD t0 = GetTickCount();
    std::free(p);
    heapNote("del", 0, t0);
}
#endif // X360_RUNTIME_PROBES

#endif
