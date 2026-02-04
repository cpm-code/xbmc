/*
 *  Copyright (c) 2002 Frodo
 *      Portions Copyright (c) by the authors of ffmpeg and xvid
 *  Copyright (C) 2002-2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Thread.h"

#include "IRunnable.h"
#include "commons/Exception.h"
#include "threads/IThreadImpl.h"
#include "threads/SingleLock.h"
#include "utils/log.h"

#include <atomic>
#include <inttypes.h>
#include <iostream>
#include <mutex>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <fmt/format.h>
#if FMT_VERSION >= 90000
#include <fmt/ostream.h>
#endif

#if FMT_VERSION >= 90000
template<>
struct fmt::formatter<std::thread::id> : ostream_formatter
{
};
#else
template<>
struct fmt::formatter<std::thread::id> : fmt::formatter<std::string>
{
  template<class FormatContext>
  auto format(const std::thread::id& e, FormatContext& ctx)
  {
    std::ostringstream str;
    str << e;
    return fmt::formatter<std::string>::format(str.str(), ctx);
  }
};
#endif

static thread_local CThread* currentThread;

static std::atomic<int> g_reservedCpu{-1};

void CThread::SetGlobalExcludedCpu(int cpu)
{
  if (cpu >= 0)
    g_reservedCpu.store(cpu, std::memory_order_relaxed);
  else
    g_reservedCpu.store(-1, std::memory_order_relaxed);
}

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CThread::CThread(const char* ThreadName)
:
    m_bStop(false), m_StopEvent(true, true), m_StartEvent(true, true), m_pRunnable(nullptr)
{
  if (ThreadName)
    m_ThreadName = ThreadName;
}

CThread::CThread(IRunnable* pRunnable, const char* ThreadName)
:
    m_bStop(false), m_StopEvent(true, true), m_StartEvent(true, true), m_pRunnable(pRunnable)
{
  if (ThreadName)
    m_ThreadName = ThreadName;
}

CThread::~CThread()
{
  StopThread();
  if (m_thread != nullptr)
  {
    m_thread->detach();
    delete m_thread;
  }
}

void CThread::Create(bool bAutoDelete)
{
  if (m_thread != nullptr)
  {
    // if the thread exited on it's own, without a call to StopThread, then we can get here
    // incorrectly. We should be able to determine this by checking the promise.
    std::future_status stat = m_future.wait_for(std::chrono::milliseconds(0));
    // a status of 'ready' means the future contains the value so the thread has exited
    // since the thread can't exit without setting the future.
    if (stat == std::future_status::ready) // this is an indication the thread has exited.
      StopThread(true);  // so let's just clean up
    else
    { // otherwise we have a problem.
      CLog::Log(LOGERROR, "{} - fatal error creating thread {} - old thread id not null",
                __FUNCTION__, m_ThreadName);
      exit(1);
    }
  }

  m_bAutoDelete = bAutoDelete;
  m_bStop = false;
  m_StopEvent.Reset();
  m_StartEvent.Reset();

  // lock?
  //std::unique_lock l(m_CriticalSection);

  std::promise<bool> prom;
  m_future = prom.get_future();

  {
    // The std::thread internals must be set prior to the lambda doing
    //   any work. This will cause the lambda to wait until m_thread
    //   is fully initialized. Interestingly, using a std::atomic doesn't
    //   have the appropriate memory barrier behavior to accomplish the
    //   same thing so a full system mutex needs to be used.
    std::unique_lock blockLambdaTillDone(m_CriticalSection);
    m_thread = new std::thread([](CThread* pThread, std::promise<bool> promise)
    {
      try
      {

        {
          // Wait for the pThread->m_thread internals to be set. Otherwise we could
          // get to a place where we're reading, say, the thread id inside this
          // lambda's call stack prior to the thread that kicked off this lambda
          // having it set. Once this lock is released, the CThread::Create function
          // that kicked this off is done so everything should be set.
          std::unique_lock waitForThreadInternalsToBeSet(pThread->m_CriticalSection);
        }

        // This is used in various helper methods like GetCurrentThread so it needs
        // to be set before anything else is done.
        currentThread = pThread;

        if (pThread == nullptr)
        {
          CLog::Log(LOGERROR, "{}, sanity failed. thread is NULL.", __FUNCTION__);
          promise.set_value(false);
          return;
        }

        pThread->m_impl = IThreadImpl::CreateThreadImpl(pThread->m_thread->native_handle());
        pThread->m_impl->SetThreadInfo(pThread->m_ThreadName);

        // If the application reserved a single core (by pinning the main kodi.bin thread),
        // keep other Kodi threads off that core to reduce scheduling jitter.
        const int reservedCpu = g_reservedCpu.load(std::memory_order_relaxed);
        if (reservedCpu >= 0)
        {
          cpu_set_t mask;
          CPU_ZERO(&mask);
          if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &mask) == 0)
          {
            // Preserve the main pinned kodi.bin thread.
            const bool isMainPinnedKodiThread =
                (pThread->m_ThreadName == "kodi.bin") && CPU_ISSET(reservedCpu, &mask) &&
                (CPU_COUNT(&mask) == 1);

            if (!isMainPinnedKodiThread && CPU_ISSET(reservedCpu, &mask))
            {
              CPU_CLR(reservedCpu, &mask);
              if (CPU_COUNT(&mask) == 0)
              {
                const int onlineCpus = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
                for (int cpu = 0; cpu < onlineCpus && cpu < CPU_SETSIZE; ++cpu)
                  CPU_SET(cpu, &mask);
              }
              pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &mask);
            }
          }
        }

        CLog::Log(LOGDEBUG, "Thread {} start, auto delete: {}", pThread->m_ThreadName,
                  (pThread->m_bAutoDelete ? "true" : "false"));

        pThread->m_StartEvent.Set();

        pThread->Action();

        if (pThread->m_bAutoDelete)
        {
          CLog::Log(LOGDEBUG, "Thread {} {} terminating (autodelete)", pThread->m_ThreadName,
                    std::this_thread::get_id());
          delete pThread;
          pThread = NULL;
        }
        else
          CLog::Log(LOGDEBUG, "Thread {} {} terminating", pThread->m_ThreadName,
                    std::this_thread::get_id());
      }
      catch (const std::exception& e)
      {
        CLog::Log(LOGDEBUG, "Thread Terminating with Exception: {}", e.what());
      }
      catch (...)
      {
        CLog::Log(LOGDEBUG,"Thread Terminating with Exception");
      }

      promise.set_value(true);
    }, this, std::move(prom));

    // On Linux, new threads inherit the creating thread's CPU affinity mask.
    // Since CoreELEC/Amlogic configurations often pin the main Kodi thread (kodi.bin)
    // to a single core, threads created by it would otherwise be stuck on that core.
    // Avoid that by widening child thread affinity to all online CPUs except the
    // creator's single pinned CPU.
    char creatorName[16] = {0};
    pthread_getname_np(pthread_self(), creatorName, sizeof(creatorName));
    if (strcmp(creatorName, "kodi.bin") == 0)
    {
      cpu_set_t creatorMask;
      CPU_ZERO(&creatorMask);
      if (pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &creatorMask) == 0)
      {
        const int onlineCpus = static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN));
        int pinnedCpu = -1;
        int count = 0;
        for (int cpu = 0; cpu < onlineCpus && cpu < CPU_SETSIZE; ++cpu)
        {
          if (CPU_ISSET(cpu, &creatorMask))
          {
            pinnedCpu = cpu;
            ++count;
          }
        }

        if (count == 1 && pinnedCpu >= 0)
        {
          g_reservedCpu.store(pinnedCpu, std::memory_order_relaxed);
          cpu_set_t childMask;
          CPU_ZERO(&childMask);
          for (int cpu = 0; cpu < onlineCpus && cpu < CPU_SETSIZE; ++cpu)
          {
            if (cpu != pinnedCpu)
              CPU_SET(cpu, &childMask);
          }
          pthread_setaffinity_np(m_thread->native_handle(), sizeof(cpu_set_t), &childMask);
        }
      }
    }
  } // let the lambda proceed

  m_StartEvent.Wait(); // wait for the thread just spawned to set its internals
}

bool CThread::IsRunning() const
{
  if (m_thread != nullptr) {
    // it's possible that the thread exited on it's own without a call to StopThread. If so then
    // the promise should be fulfilled.
    std::future_status stat = m_future.wait_for(std::chrono::milliseconds(0));
    // a status of 'ready' means the future contains the value so the thread has exited
    // since the thread can't exit without setting the future.
    if (stat == std::future_status::ready) // this is an indication the thread has exited.
      return false;
    return true; // otherwise the thread is still active.
  } else
    return false;
}

bool CThread::SetPriority(const ThreadPriority& priority)
{
  return m_impl->SetPriority(priority);
}

bool CThread::SetTask(const ThreadTask& task)
{
  return m_impl->SetTask(task);
}

bool CThread::RevertTask()
{
  return m_impl->RevertTask();
}

bool CThread::IsAutoDelete() const
{
  return m_bAutoDelete;
}

void CThread::StopThread(bool bWait /*= true*/)
{
  m_StartEvent.Wait();

  m_bStop = true;
  m_StopEvent.Set();
  std::unique_lock lock(m_CriticalSection);
  std::thread* lthread = m_thread;
  if (lthread != nullptr && bWait && !IsCurrentThread())
  {
    lock.unlock();
    if (!Join(std::chrono::milliseconds::max())) // eh?
      lthread->join();
    m_thread = nullptr;
  }
}

void CThread::Process()
{
  if (m_pRunnable)
    m_pRunnable->Run();
}

bool CThread::IsCurrentThread() const
{
  CThread* pThread = currentThread;
  if (pThread != nullptr)
    return pThread == this;
  else
    return false;
}

CThread* CThread::GetCurrentThread()
{
  return currentThread;
}

bool CThread::Join(std::chrono::milliseconds duration)
{
  std::unique_lock l(m_CriticalSection);
  std::thread* lthread = m_thread;
  if (lthread != nullptr)
  {
    if (IsCurrentThread())
      return false;

    {
      CSingleExit exit(m_CriticalSection); // don't hold the thread lock while we're waiting
      std::future_status stat = m_future.wait_for(duration);
      if (stat != std::future_status::ready)
        return false;
    }

    // it's possible it's already joined since we released the lock above.
    if (lthread->joinable())
      m_thread->join();
    return true;
  }
  else
    return false;
}

void CThread::Action()
{
  try
  {
    OnStartup();
  }
  catch (const XbmcCommons::UncheckedException &e)
  {
    e.LogThrowMessage("OnStartup");
    if (IsAutoDelete())
      return;
  }

  try
  {
    Process();
  }
  catch (const XbmcCommons::UncheckedException &e)
  {
    e.LogThrowMessage("Process");
  }

  try
  {
    OnExit();
  }
  catch (const XbmcCommons::UncheckedException &e)
  {
    e.LogThrowMessage("OnExit");
  }
}
