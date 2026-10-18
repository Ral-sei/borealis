/*
    Copyright 2021 XITRIX

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <libretro-common/retro_timers.h>

#include <borealis/core/logger.hpp>
#include <borealis/core/thread.hpp>
#include <cstdint>
#include <cstdio>
#include <exception>

#if defined(PLATFORM_XBOX360)
#include <xtl.h>
// 亲和性/优先级复核探针（round-8，§15.5）；xboxkrnl 导出，OXDK/XDK 头无声明。
// 此 clang 不接受块作用域的 extern "C"，声明放文件作用域。
extern "C" void DbgPrint(const char*, ...);
#endif

#if defined(BOREALIS_USE_STD_THREAD) || defined(PLATFORM_XBOX360)
// Xbox: BOREALIS_USE_STD_THREAD is off, but <thread> resolves to the
// xbox360-cshim header (CreateThread-backed std::thread), and the XDK sleep
// path used by task_loop() maps to Sleep(). Running the loop for real is what
// keeps brls::async() from becoming a write-only queue — see start() below.
#include <thread>
#elif !defined(PLATFORM_XBOX360)
#include <pthread.h>
#endif

#if defined(PLATFORM_XBOX360)
#include <borealis/platforms/xbox360/xbox360_perf.hpp>
#endif

#ifdef PS4
#include <orbis/libkernel.h>
#endif

namespace brls
{

#if defined(BOREALIS_USE_STD_THREAD) || defined(PLATFORM_XBOX360)
static std::thread *task_loop_thread = nullptr;
#elif !defined(PLATFORM_XBOX360)
static pthread_t task_loop_thread = pthread_t(0);
#endif

Threading::Threading()
{
    start_task_loop();
}

/* P0 §5.6-4：sync 任务的来源标签（HTTP 回调期间由 RequestLog 设置）。 */
static PlatformMutex s_sync_tag_mutex;
static std::string s_sync_tag;

void sync(const std::function<void()>& func, const char* tag)
{
    Threading::sync(func, tag);
}

void async(const std::function<void()>& task)
{
    Threading::async(task);
}

size_t delay(long milliseconds, const std::function<void()>& func)
{
    return Threading::delay(milliseconds, func);
}

void cancelDelay(size_t iter)
{
    Threading::cancelDelay(iter);
}

void Threading::sync(const std::function<void()>& func, const char* tag)
{
    // P0 §5.6-4：拷贝来源标签（显式 tag 优先；否则取 HTTP RequestLog 回调
    // 期间设置的全局标签），与任务成对入队。
    std::string effectiveTag;
    if (tag && *tag)
    {
        effectiveTag = tag;
    }
    else
    {
        PlatformLockGuard tagGuard(s_sync_tag_mutex);
        effectiveTag = s_sync_tag;
    }
    PlatformLockGuard guard(m_sync_mutex);
    m_sync_functions.push_back(func);
    m_sync_tags.push_back(effectiveTag);
}

void Threading::setSyncEnqueueTag(const char* tag)
{
    PlatformLockGuard tagGuard(s_sync_tag_mutex);
    s_sync_tag = tag ? tag : "";
}

void Threading::async(const std::function<void()>& task)
{
    PlatformLockGuard guard(m_async_mutex);
    m_async_tasks.push_back(task);
}

size_t Threading::delay(long milliseconds, const std::function<void()>& func)
{
    PlatformLockGuard guard(m_delay_mutex);
    DelayOperation operation;
#ifdef PS4
    operation.startPoint        = sceKernelGetProcessTime();
    operation.delayMilliseconds = milliseconds * 1000;
#else
    operation.startPoint        = std::chrono::high_resolution_clock::now();
    operation.delayMilliseconds = milliseconds;
#endif
    operation.func              = func;
    operation.index             = ++m_delay_index;
    m_delay_tasks.push_back(operation);
    return m_delay_index;
}

void Threading::cancelDelay(size_t iter)
{
    PlatformLockGuard guard(m_delay_mutex);
    m_delay_cancel_set.insert(iter);
}

void Threading::performSyncTasks()
{
    m_sync_mutex.lock();
    auto local = m_sync_functions;
    m_sync_functions.clear();
    auto tags = m_sync_tags;   // 与 local 一一对应（同锁内增删）
    m_sync_tags.clear();
    m_sync_mutex.unlock();
#if defined(PLATFORM_XBOX360)
    // P0 §3.1 测量：统计每帧一次性倾倒的 sync 任务数量（窗口内累加 + 单帧最大值），
    // 用来验证"图片纹理创建堆在同一帧执行"的假设。
    brls::xbox360perf::noteSyncTasks((int)local.size());
#endif

#if defined(PLATFORM_XBOX360)
    // P0 §5.11：记录主线程 sync 执行时的栈深基准（配合 View::View() 的采样）
    brls::xbox360perf::noteStack(__builtin_frame_address(0));
#endif
    size_t taskIndex = 0;
    for (auto& f : local)
    {
#if defined(PLATFORM_XBOX360)
        // P0 §5.5-3 测量：逐任务计时，>500ms 打印序号与批次大小，
        // 用于定位 12.3s 冻结具体是哪个 sync lambda（配合调用点标签日志）。
        const std::uint64_t t0 = brls::xbox360perf::nowUsec();
        // §5.6-4：标签提前取出；同时作为 [stall-task] 看门狗的阶段名——
        // 主线程卡在任务里时，日志会停摆，看门狗直接 DbgPrint 这个阶段。
        const char* tag = (taskIndex < tags.size()) ? tags[taskIndex].c_str() : "";
        char stageBuf[96];
        std::snprintf(stageBuf, sizeof(stageBuf), "sync:%s", tag[0] ? tag : "?");
        brls::xbox360perf::stageMark(stageBuf);
#endif
#if defined(__cpp_exceptions)
        try { f(); } catch (std::exception& e) { brls::Logger::error("error: performSyncTasks: {}", e.what()); }
#else
        f();
#endif
#if defined(PLATFORM_XBOX360)
        brls::xbox360perf::stageClear();
        const double tookMs = (double)(brls::xbox360perf::nowUsec() - t0) / 1000.0;
        if (tookMs >= 500.0) {
            // §5.6-4：带上入队时的来源标签，直接定位慢任务属于哪条业务链路。
            X360_PROBE_INFO("[x360-perf] slow sync task took {:.0f} ms (batch {}) from {}",
                               tookMs, local.size(), tag[0] ? tag : "(untagged)");
        }
#endif
        taskIndex++;
    }

    m_delay_mutex.lock();
    auto delay_local = m_delay_tasks;
    m_delay_tasks.clear();
    m_delay_mutex.unlock();

    for (auto& d : delay_local)
    {
        // Check cancel
        m_delay_mutex.lock();
        if (m_delay_cancel_set.count(d.index))
        {
            m_delay_cancel_set.erase(d.index);
            m_delay_mutex.unlock();
            continue;
        }
        m_delay_mutex.unlock();

#ifdef PS4
        uint64_t duration = sceKernelGetProcessTime() - d.startPoint;
#else
        auto timeNow  = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(timeNow - d.startPoint).count();
#endif

        if (duration >= d.delayMilliseconds)
        {
#if defined(PLATFORM_XBOX360)
            // 物种 B 探针：delay 任务此前完全没有阶段覆盖（跑在 sync 任务
            // 之后的空档里，卡住时看门狗读到空阶段直接跳过）。
            const std::uint64_t dt0 = brls::xbox360perf::nowUsec();
            brls::xbox360perf::stageMark("delay-task");
#endif
#if defined(__cpp_exceptions)
            try { d.func(); } catch (std::exception& e) { brls::Logger::error("error: performSyncTasks(delay): {}", e.what()); }
#else
            d.func();
#endif
#if defined(PLATFORM_XBOX360)
            brls::xbox360perf::stageClear();
            const double dtMs = (double)(brls::xbox360perf::nowUsec() - dt0) / 1000.0;
            if (dtMs >= 500.0) {
                X360_PROBE_INFO("[x360-perf] slow delay task took {:.0f} ms (batch {})",
                                   dtMs, delay_local.size());
            }
#endif

            m_delay_mutex.lock();
            if (m_delay_cancel_set.count(d.index)) m_delay_cancel_set.erase(d.index);
            m_delay_mutex.unlock();

        }
        else
        {
            m_delay_mutex.lock();
            m_delay_tasks.push_back(d);
            m_delay_mutex.unlock();
        }
    }

}

void Threading::start()
{
    task_loop_active = true;
    start_task_loop();
}

void Threading::stop()
{
    task_loop_active = false;

#if defined(BOREALIS_USE_STD_THREAD) || defined(PLATFORM_XBOX360)
    // Xbox: task_loop_thread is only ever set by start_task_loop(); guard it
    // so stop() stays safe when start() was never reached.
    if (task_loop_thread)
    {
        task_loop_thread->join();
        delete task_loop_thread;
        task_loop_thread = nullptr;
    }
#else
    pthread_join(task_loop_thread, NULL);
#endif
}
void Threading::std_task_loop() {
    task_loop(nullptr);
}
void* Threading::task_loop(void* a)
{
#if defined(PLATFORM_XBOX360)
    // Static thread partitioning, following the Xbox 360 YouTube client
    // (every media/UI thread pinned to a dedicated hardware thread) and the
    // XDK guidance that background CPU-intensive threads run below normal:
    // the async queue is background work, so park it on hardware thread 5
    // (the libmpv worker spread table is {2,4,1,3} and never uses hw 5) and
    // drop it under the main/UI thread.  Together this stops an async batch
    // from time-slicing against frame rendering on hw 0.  Deliberately NOT
    // done for the main or mpv threads: the kernel has no priority-inversion
    // mitigation, so only threads nobody blocks on get lowered.
    XSetThreadProcessor(GetCurrentThread(), 5);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#if X360_RUNTIME_PROBES
    DbgPrint("[x360-thread] task_loop hw=5 prio=below");
#endif
#endif
    while (task_loop_active)
    {
        std::vector<std::function<void()>> m_tasks_copy;
        {
            PlatformLockGuard guard(m_async_mutex);
            m_tasks_copy = m_async_tasks;
            m_async_tasks.clear();
        }

        for (auto task : m_tasks_copy)
        {
            task();
        }

        retro_sleep(500);
    }
    return NULL;
}

void Threading::start_task_loop()
{
#if defined(BOREALIS_USE_STD_THREAD) || defined(PLATFORM_XBOX360)
    if (task_loop_thread) return; // already running (Threading ctor + Application::start)
    task_loop_thread = new std::thread(std_task_loop);
#else
    pthread_create(&task_loop_thread, NULL, task_loop, NULL);
#endif
}

} // namespace brls
