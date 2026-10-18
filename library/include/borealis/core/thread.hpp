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

#pragma once

#if !defined(PLATFORM_XBOX360)
#include <unistd.h>
#endif

#include <chrono>
#include <functional>
#include <string>
#include <borealis/core/xbox360_mutex.hpp>
#if !defined(PLATFORM_XBOX360)
#include <thread>
#endif
#include <vector>
#include <set>

namespace brls
{

struct DelayOperation
{
#ifdef PS4
    uint64_t startPoint;
#else
    std::chrono::high_resolution_clock::time_point startPoint;
#endif
    long delayMilliseconds;
    size_t index;
    std::function<void()> func;
};

typedef std::vector<DelayOperation>::iterator DelayOperationIterator;

/**
 * Enqueue a function to be executed before
 * the application is redrawn the next time.
 *
 * Borealis is not thread-safe, and sync() provides a mechanism
 * for queuing up UI-related state changes from other threads.
 *
 * It's a shortcut for brls::Application::sync(&func);
 *
 * tag（仅诊断）：显式指定本次入队的来源标签，优先于 HTTP 回调期间由
 * setSyncEnqueueTag 设置的全局标签；performSyncTasks 打印慢任务(>=500ms)
 * 与 [stall-task] 看门狗都会带上它。
 */
extern void sync(const std::function<void()>& func, const char* tag = nullptr);

/**
 * Enqueue a function to be executed in
 * parallel with application's main thread.
 *
 * It's a shortcut for brls::Application::async(&func);
 */
extern void async(const std::function<void()>& func);

extern size_t delay(long milliseconds, const std::function<void()>& func);

extern void cancelDelay(size_t iter);

class Threading
{
  public:
    Threading();

    /**
     * Enqueue a function to be executed before
     * the application is redrawn the next time.
     *
     * Borealis is not thread-safe, and sync() provides a mechanism
     * for queuing up UI-related state changes from other threads.
     *
     * tag（仅诊断）：显式来源标签，优先于全局 setSyncEnqueueTag 标签。
     */
    static void sync(const std::function<void()>& func, const char* tag = nullptr);

    /**
     * P0 §5.6-4 测量：为本次 sync 入队打上"来源标签"（如 HTTP 回调的接口名）。
     * performSyncTasks 打印慢任务(>=500ms)时一并输出，用来定位是哪条业务链路
     * 在主线程上耗时。全局单值：并发回调时会互相覆盖（仅诊断用，允许近似）。
     */
    static void setSyncEnqueueTag(const char* tag);

    /**
     * Enqueue a function to be executed in
     * parallel with application's main thread.
     */
    static void async(const std::function<void()>& func);

    static size_t delay(long milliseconds, const std::function<void()>& func);

    static void cancelDelay(size_t iter);

    static void start();

    static void stop();

    static void performSyncTasks();

    static std::vector<std::function<void()>>* getSyncFunctions()
    {
        PlatformLockGuard guard(m_sync_mutex);
        return &m_sync_functions;
    }

    static std::vector<std::function<void()>>* getAsyncTasks()
    {
        PlatformLockGuard guard(m_async_mutex);
        return &m_async_tasks;
    }

  private:
    inline static PlatformMutex m_sync_mutex;
    inline static std::vector<std::function<void()>> m_sync_functions;
    /* 与 m_sync_functions 同步增删的来源标签（P0 §5.6-4 诊断用）。 */
    inline static std::vector<std::string> m_sync_tags;

    inline static PlatformMutex m_async_mutex;
    inline static std::vector<std::function<void()>> m_async_tasks;

    inline static PlatformMutex m_delay_mutex;
    inline static std::vector<DelayOperation> m_delay_tasks;
    inline static std::set<size_t> m_delay_cancel_set;
    inline static size_t m_delay_index = 0;

    inline static volatile bool task_loop_active = true;

    static void* task_loop(void* a);
    static void std_task_loop();

    static void start_task_loop();
};

} // namespace brls
