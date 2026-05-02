/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RenderManager.h"

/* to use the same as player */
#include "../VideoPlayer/DVDClock.h"
#include "RenderCapture.h"
#include "RenderFactory.h"
#include "RenderFlags.h"
#include "ServiceBroker.h"
#include "application/Application.h"
#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "guilib/GUIComponent.h"
#include "guilib/StereoscopicsManager.h"
#include "messaging/ApplicationMessenger.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "threads/SingleLock.h"
#include "utils/AMLUtils.h"
#include "utils/StreamDetails.h"
#include "utils/StringUtils.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>

using namespace std::chrono_literals;

namespace
{
const char* RenderStateToString(CRenderManager::ERENDERSTATE state)
{
  switch (state)
  {
    case CRenderManager::STATE_UNCONFIGURED:
      return "unconfigured";
    case CRenderManager::STATE_CONFIGURING:
      return "configuring";
    case CRenderManager::STATE_CONFIGURED:
      return "configured";
  }
  return "unknown";
}

const char* PresentStepToString(CRenderManager::EPRESENTSTEP step)
{
  switch (step)
  {
    case CRenderManager::PRESENT_IDLE:
      return "idle";
    case CRenderManager::PRESENT_FLIP:
      return "flip";
    case CRenderManager::PRESENT_FRAME:
      return "frame";
    case CRenderManager::PRESENT_FRAME2:
      return "frame2";
    case CRenderManager::PRESENT_READY:
      return "ready";
  }
  return "unknown";
}

struct WaitDebugInfo
{
  bool used = false;
  bool usedSlice = false;
  bool gui = false;
  bool kernelWindow = false;
  bool gotNextIn = false;
  bool gotNextInAfter = false;
  double waitUs = 0.0;
  double sleepUs = 0.0;
  double sliceSleepUs = 0.0;
  double frameTimeUs = 0.0;
  int nextInUs = 0;
  int nextInAfterUs = 0;
  int guardUs = 0;
  bool clamped = false;
};

const char* GetWaitMode(const WaitDebugInfo& waitDbg)
{
  if (!waitDbg.used) return "none";
  if (waitDbg.gui) return "gui";
  if (waitDbg.usedSlice) return "slice";
  if (waitDbg.kernelWindow) return "video-kernel";

  return "video";
}
} // namespace

void CRenderManager::CClockSync::Reset()
{
  m_error = 0;
  m_errCount = 0;
  m_syncOffset = 0;
  m_enabled = false;
}

unsigned int CRenderManager::m_nextCaptureId = 0;

CRenderManager::CRenderManager(CDVDClock &clock, IRenderMsg *player) :
  m_dvdClock(clock),
  m_playerPort(player),
  m_dataCacheCore(CServiceBroker::GetDataCacheCore())
{
}

CRenderManager::~CRenderManager()
{
  delete m_pRenderer;
}

void CRenderManager::GetVideoRect(CRect& source, CRect& dest, CRect& view) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    m_pRenderer->GetVideoRect(source, dest, view);
}

float CRenderManager::GetAspectRatio() const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetAspectRatio();
  else
    return 1.0f;
}

unsigned int CRenderManager::GetOrientation() const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->GetOrientation();
  else
    return 0;
}

void CRenderManager::SetVideoSettings(const CVideoSettings& settings)
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
  {
    m_pRenderer->SetVideoSettings(settings);
  }
}

bool CRenderManager::Configure(const VideoPicture& picture, float fps, unsigned int orientation,
  StreamHdrType hdrType, int buffers)
{

  // check if something has changed
  {
    std::unique_lock lock(m_statelock);

    if (!m_bRenderGUI)
      return true;

    if (m_pRenderer != nullptr && m_picture.IsSameParams(picture) && m_orientation == orientation &&
        m_NumberBuffers == buffers && !m_pRenderer->ConfigChanged(picture))
    {
      if (m_fps != fps)
      {
        CLog::Log(LOGDEBUG, "CRenderManager::Configure - framerate changed from {:4.2f} to {:4.2f}",
                  m_fps, fps);
        m_fps = fps;
        m_pRenderer->SetFps(fps);
        m_bTriggerUpdateResolution = true;
        // Clear stale vsync/late-frame state from the old framerate; CheckEnableClockSync() will recalibrate on the next FrameMove on the main thread.
        m_clockSync.Reset();
        m_dvdClock.SetVsyncAdjust(0);
        m_lateframes = -1;
      }
      return true;
    }
  }

  const std::string hdrStr = CStreamDetails::DynamicRangeToString(hdrType);
  CLog::Log(LOGDEBUG,
            "CRenderManager::Configure - change configuration. {}x{}. display: {}x{}. framerate: "
            "{:4.2f}. hdrType: {}.",
            picture.iWidth, picture.iHeight, picture.iDisplayWidth, picture.iDisplayHeight, fps,
            hdrStr.empty() ? "none" : hdrStr);

  // make sure any queued frame was fully presented
  {
    std::unique_lock lock(m_presentlock);
    XbmcThreads::EndTime<> endtime(5000ms);
    m_forceNext = true;
    while (m_presentstep != PRESENT_IDLE)
    {
      if(endtime.IsTimePast())
      {
        CLog::Log(LOGWARNING, "CRenderManager::Configure - timeout waiting for state");
        m_forceNext = false;
        return false;
      }
      WaitPresent(lock, endtime.GetTimeLeft());
    }
    m_forceNext = false;
  }

  {
    std::unique_lock lock(m_statelock);
    m_picture.SetParams(picture);
    m_fps = fps;
    m_orientation = orientation;
    m_NumberBuffers  = buffers;
    m_renderState = STATE_CONFIGURING;
    m_stateEvent.Reset();
    m_clockSync.Reset();
    m_dvdClock.SetVsyncAdjust(0);
    m_pConfigPicture = std::make_unique<VideoPicture>();
    m_pConfigPicture->CopyRef(picture);

    std::unique_lock lock2(m_presentlock);
    m_presentstep = PRESENT_READY;
    NotifyPresentWaiters();
  }

  // Waiting on m_stateEvent returns immediately once the render thread finishes configuring.
  // Keep this per-attempt timeout short; higher-level code can retry for a bounded time window
  // during slow display mode switches (refresh rate / HDR / DV / AVR handshakes).
  const auto configureWaitTimeout = 1200ms;
  const auto configureWaitStart = std::chrono::steady_clock::now();

  if (!m_stateEvent.Wait(configureWaitTimeout))
  {
    const auto waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - configureWaitStart)
                            .count();
    const auto timeoutMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(configureWaitTimeout).count();

    // Best-effort state dump without blocking in case the render thread is stuck holding locks.
    const char* renderStateStr = "(locked)";
    {
      std::unique_lock stateLock(m_statelock, std::try_to_lock);
      if (stateLock.owns_lock())
        renderStateStr = RenderStateToString(m_renderState);
    }

    const char* presentStepStr = "(locked)";
    {
      std::unique_lock presentLock(m_presentlock, std::try_to_lock);
      if (presentLock.owns_lock())
        presentStepStr = PresentStepToString(m_presentstep);
    }

    logM(LOGWARNING, "timeout waiting for configure (timeout={} ms, waited={} ms, renderState={}, presentStep={})",
                     timeoutMs, waitedMs, renderStateStr, presentStepStr);

    return false;
  }

  std::unique_lock lock(m_statelock);
  if (m_renderState != STATE_CONFIGURED)
  {
    CLog::Log(LOGWARNING, "CRenderManager::Configure - failed to configure");
    return false;
  }

  return true;
}

bool CRenderManager::Configure()
{
  // lock all interfaces
  std::unique_lock lock(m_statelock);
  std::unique_lock lock2(m_presentlock);
  std::unique_lock lock3(m_datalock);

  if (m_pRenderer)
  {
    DeleteRenderer();
  }

  if (!m_pRenderer)
  {
    CreateRenderer();
    if (!m_pRenderer)
      return false;
  }

  m_pRenderer->SetVideoSettings(m_playerPort->GetVideoSettings());
  bool result = m_pRenderer->Configure(*m_pConfigPicture, m_fps, m_orientation);
  if (result)
  {
    CRenderInfo info = m_pRenderer->GetRenderInfo();
    int renderbuffers = info.max_buffer_size;
    m_QueueSize = renderbuffers;
    if (m_NumberBuffers > 0)
      m_QueueSize = std::min(m_NumberBuffers, renderbuffers);

    if(m_QueueSize < 2)
    {
      m_QueueSize = 2;
      CLog::Log(LOGWARNING, "CRenderManager::Configure - queue size too small ({}, {}, {})",
                m_QueueSize.load(), renderbuffers, m_NumberBuffers);
    }

    m_pRenderer->SetBufferSize(m_QueueSize);
    m_pRenderer->Update();

    m_playerPort->UpdateRenderInfo(info);
    m_playerPort->UpdateGuiRender(true);
    m_playerPort->UpdateVideoRender(!m_pRenderer->IsGuiLayer());

    m_queued.clear();
    m_discard.clear();
    m_free.clear();
    m_presentstarted = false;
    m_presentsource = 0;
    for (int i = 0; i < m_QueueSize; i++)
      m_free.push_back(i);

    m_bRenderGUI = true;
    m_bTriggerUpdateResolution = true;
    m_presentstep = PRESENT_IDLE;
    m_presentpts = DVD_NOPTS_VALUE;
    m_lateframes = -1;
    NotifyPresentWaiters();
    m_renderedOverlay = false;
    m_renderDebug = false;
    m_clockSync.Reset();
    m_dvdClock.SetVsyncAdjust(0);
    m_overlays.Reset();
    m_overlays.SetStereoMode(m_picture.stereoMode);

    m_renderState = STATE_CONFIGURED;

    CLog::Log(LOGDEBUG, "CRenderManager::Configure - {}", m_QueueSize.load());
  }
  else
    m_renderState = STATE_UNCONFIGURED;

  m_pConfigPicture.reset();

  m_stateEvent.Set();
  m_playerPort->VideoParamsChange();
  return result;
}

bool CRenderManager::IsConfigured() const
{
  return m_renderState.load(std::memory_order_relaxed) == STATE_CONFIGURED;
}

bool CRenderManager::HasPendingResolutionChange()
{
  std::unique_lock<CCriticalSection> lock(m_resolutionlock);
  return m_bTriggerUpdateResolution;
}

void CRenderManager::ShowVideo(bool enable)
{
  m_showVideo = enable;
  if (!enable)
    DiscardBuffer();
}

void CRenderManager::DisplayReset()
{
  m_QueueSkip = 0;
}

void CRenderManager::FrameWait(std::chrono::milliseconds duration)
{
  XbmcThreads::EndTime<> timeout{duration};
  std::unique_lock lock(m_presentlock);
  while(m_presentstep == PRESENT_IDLE && !timeout.IsTimePast())
    WaitPresent(lock, timeout.GetTimeLeft());
}

bool CRenderManager::IsPresenting()
{
  if (!IsConfigured())
    return false;

  std::unique_lock lock(m_presentlock);
  return !m_presentTimer.IsTimePast();
}

void CRenderManager::FrameMove()
{
  bool firstFrame = false;
  UpdateResolution();

  {
    std::unique_lock lock(m_statelock);

    if (m_renderState == STATE_UNCONFIGURED)
      return;
    else if (m_renderState == STATE_CONFIGURING)
    {
      lock.unlock();
      if (!Configure())
        return;
      firstFrame = true;
      FrameWait(50ms);
    }

    CheckEnableClockSync();
  }
  {
    std::unique_lock lock2(m_presentlock);

    if (m_queued.empty())
    {
      m_presentstep = PRESENT_IDLE;
    }
    else
    {
      m_presentTimer.Set(1000ms);
    }

    if (m_presentstep == PRESENT_READY)
      PrepareNextRender();

    if (m_presentstep == PRESENT_FLIP)
    {
      m_presentstep = PRESENT_FRAME;
      NotifyPresentWaiters();
    }

    // release all previous
    for (std::deque<int>::iterator it = m_discard.begin(); it != m_discard.end(); )
    {
      // renderer may want to keep the frame for postprocessing
      if (!m_pRenderer->NeedBuffer(*it) || !m_bRenderGUI)
      {
        m_pRenderer->ReleaseBuffer(*it);
        m_overlays.Release(*it);
        m_free.push_back(*it);
        it = m_discard.erase(it);
      }
      else
        ++it;
    }

    m_bRenderGUI = true;
  }

  m_playerPort->UpdateGuiRender(IsGuiLayer() || firstFrame);

  ManageCaptures();
}

void CRenderManager::PreInit()
{
  {
    std::unique_lock lock(m_statelock);
    if (m_renderState != STATE_UNCONFIGURED)
      return;
  }

  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    m_initEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_PREINIT);
    if (!m_initEvent.Wait(2000ms))
    {
      CLog::Log(LOGERROR, "{} - timed out waiting for renderer to preinit", __FUNCTION__);
    }
  }

  std::unique_lock lock(m_statelock);

  if (!m_pRenderer)
  {
    CreateRenderer();
  }

  m_debugRenderer.Initialize();

  UpdateVideoLatencyTweak();

  m_QueueSize   = 2;
  m_QueueSkip   = 0;
  m_presentstep = PRESENT_IDLE;
  m_bRenderGUI = true;

  m_initEvent.Set();
}

void CRenderManager::UnInit()
{
  if (!CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    m_initEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_UNINIT);
    if (!m_initEvent.Wait(2000ms))
    {
      CLog::Log(LOGERROR, "{} - timed out waiting for renderer to uninit", __FUNCTION__);
    }
  }

  std::unique_lock lock(m_statelock);

  m_overlays.UnInit();
  m_debugRenderer.Dispose();

  DeleteRenderer();

  m_renderState = STATE_UNCONFIGURED;
  m_picture.Reset();
  m_bRenderGUI = false;
  CServiceBroker::GetWinSystem()->GetGfxContext().SetHDRType(m_picture.hdrType);
  RemoveCaptures();

  m_initEvent.Set();
}

bool CRenderManager::Flush(bool wait, bool saveBuffers)
{
  if (!m_pRenderer)
    return true;

  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    CLog::Log(LOGDEBUG, "{} - flushing renderer", __FUNCTION__);

// fix deadlock on Windows only when is enabled 'Sync playback to display'
#ifndef TARGET_WINDOWS
    CSingleExit exitlock(CServiceBroker::GetWinSystem()->GetGfxContext());
#endif

    std::unique_lock lock(m_statelock);
    std::unique_lock lock2(m_presentlock);
    std::unique_lock lock3(m_datalock);

    if (m_pRenderer)
    {
      m_overlays.Flush();
      m_debugRenderer.Flush();

      if (!m_pRenderer->Flush(saveBuffers))
      {
        m_queued.clear();
        m_discard.clear();
        m_free.clear();
        m_presentstarted = false;
        m_presentsource = 0;
        m_presentstep = PRESENT_IDLE;
        for (int i = 0; i < m_QueueSize; i++)
          m_free.push_back(i);
      }

      m_flushEvent.Set();
    }
  }
  else
  {
    m_flushEvent.Reset();
    CServiceBroker::GetAppMessenger()->PostMsg(TMSG_RENDERER_FLUSH);
    if (wait)
    {
      if (!m_flushEvent.Wait(1000ms))
      {
        CLog::Log(LOGERROR, "{} - timed out waiting for renderer to flush", __FUNCTION__);
        return false;
      }
      else
        return true;
    }
  }
  return true;
}

void CRenderManager::CreateRenderer()
{
  if (!m_pRenderer)
  {
    CVideoBuffer *buffer = nullptr;
    if (m_pConfigPicture)
      buffer = m_pConfigPicture->videoBuffer;

    auto renderers = VIDEOPLAYER::CRendererFactory::GetRenderers();
    for (auto &id : renderers)
    {
      if (id == "default")
        continue;

      m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer(id, buffer);
      if (m_pRenderer)
      {
        return;
      }
    }
    m_pRenderer = VIDEOPLAYER::CRendererFactory::CreateRenderer("default", buffer);
  }
}

void CRenderManager::DeleteRenderer()
{
  if (m_pRenderer)
  {
    CLog::Log(LOGDEBUG, "{} - deleting renderer", __FUNCTION__);

    delete m_pRenderer;
    m_pRenderer = NULL;
  }
}

unsigned int CRenderManager::AllocRenderCapture()
{
  if (m_pRenderer)
  {
    CRenderCapture* capture = m_pRenderer->GetRenderCapture();
    if (capture)
    {
      m_captures[m_nextCaptureId] = capture;
      return m_nextCaptureId++;
    }
  }

  return m_nextCaptureId;
}

void CRenderManager::ReleaseRenderCapture(unsigned int captureId)
{
  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it;
  it = m_captures.find(captureId);

  if (it != m_captures.end())
    it->second->SetState(CAPTURESTATE_NEEDSDELETE);
}

void CRenderManager::StartRenderCapture(unsigned int captureId, unsigned int width, unsigned int height, int flags)
{
  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it;
  it = m_captures.find(captureId);
  if (it == m_captures.end())
  {
    CLog::Log(LOGERROR, "CRenderManager::Capture - unknown capture id: {}", captureId);
    return;
  }

  CRenderCapture *capture = it->second;

  capture->SetState(CAPTURESTATE_NEEDSRENDER);
  capture->SetUserState(CAPTURESTATE_WORKING);
  capture->SetWidth(width);
  capture->SetHeight(height);
  capture->SetFlags(flags);
  capture->GetEvent().Reset();

  if (CServiceBroker::GetAppMessenger()->IsProcessThread())
  {
    if (flags & CAPTUREFLAG_IMMEDIATELY)
    {
      //render capture and read out immediately
      RenderCapture(capture);
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();
    }
  }

  if (!m_captures.empty())
    m_hasCaptures = true;
}

bool CRenderManager::RenderCaptureGetPixels(unsigned int captureId, unsigned int millis, uint8_t *buffer, unsigned int size)
{
  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it;
  it = m_captures.find(captureId);
  if (it == m_captures.end())
    return false;

  m_captureWaitCounter++;

  {
    if (!millis)
      millis = 1000;

    CSingleExit exitlock(m_captCritSect);
    if (!it->second->GetEvent().Wait(std::chrono::milliseconds(millis)))
    {
      m_captureWaitCounter--;
      return false;
    }
  }

  m_captureWaitCounter--;

  if (it->second->GetUserState() != CAPTURESTATE_DONE)
    return false;

  unsigned int srcSize = it->second->GetWidth() * it->second->GetHeight() * 4;
  unsigned int bytes = std::min(srcSize, size);

  memcpy(buffer, it->second->GetPixels(), bytes);
  return true;
}

void CRenderManager::ManageCaptures()
{
  //no captures, return here so we don't do an unnecessary lock
  if (!m_hasCaptures)
    return;

  std::unique_lock lock(m_captCritSect);

  std::map<unsigned int, CRenderCapture*>::iterator it = m_captures.begin();
  while (it != m_captures.end())
  {
    CRenderCapture* capture = it->second;

    if (capture->GetState() == CAPTURESTATE_NEEDSDELETE)
    {
      delete capture;
      it = m_captures.erase(it);
      continue;
    }

    if (capture->GetState() == CAPTURESTATE_NEEDSRENDER)
      RenderCapture(capture);
    else if (capture->GetState() == CAPTURESTATE_NEEDSREADOUT)
      capture->ReadOut();

    if (capture->GetState() == CAPTURESTATE_DONE || capture->GetState() == CAPTURESTATE_FAILED)
    {
      //tell the thread that the capture is done or has failed
      capture->SetUserState(capture->GetState());
      capture->GetEvent().Set();

      if (capture->GetFlags() & CAPTUREFLAG_CONTINUOUS)
      {
        capture->SetState(CAPTURESTATE_NEEDSRENDER);

        //if rendering this capture continuously, and readout is async, render a new capture immediately
        if (capture->IsAsync() && !(capture->GetFlags() & CAPTUREFLAG_IMMEDIATELY))
          RenderCapture(capture);
      }
      ++it;
    }
    else
    {
      ++it;
    }
  }

  if (m_captures.empty())
    m_hasCaptures = false;
}

void CRenderManager::RenderCapture(CRenderCapture* capture) const {
  if (!m_pRenderer || !m_pRenderer->RenderCapture(m_presentsource, capture))
    capture->SetState(CAPTURESTATE_FAILED);
}

void CRenderManager::RemoveCaptures()
{
  std::unique_lock lock(m_captCritSect);

  while (m_captureWaitCounter > 0)
  {
    for (auto entry : m_captures)
    {
      entry.second->GetEvent().Set();
    }
    CSingleExit lockexit(m_captCritSect);
    KODI::TIME::Sleep(10ms);
  }

  for (auto entry : m_captures)
  {
    delete entry.second;
  }
  m_captures.clear();
}

void CRenderManager::SetViewMode(int iViewMode) {
  std::unique_lock<CCriticalSection> lock(m_statelock);

  if (m_pRenderer)
    m_pRenderer->SetViewMode(iViewMode);
  m_playerPort->VideoParamsChange();
}

RESOLUTION CRenderManager::GetResolution() const {
  RESOLUTION res = CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution();

  std::unique_lock<CCriticalSection> lock(m_statelock);

  if (m_renderState == STATE_UNCONFIGURED)
    return res;

  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF)
    res = CResolutionUtils::ChooseBestResolution(m_fps, m_picture.iWidth, m_picture.iHeight,
                                                 !m_picture.stereoMode.empty());

  return res;
}

bool CRenderManager::CalcOverlayActiveArea(CRect& src, CRect& dst) const {
  // Setup - DV Active Area (L5) Overlay handling.
  if ((m_picture.hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION) || !aml_dv_use_active_area())
    return false;

  // Calculate scaling factors from source to destination
  float scaleX = dst.Width() / src.Width();
  float scaleY = dst.Height() / src.Height();

  // Create active area rectangle based on scaled offsets
  const auto& doviMeta = m_dataCacheCore.GetVideoDoViFrameMetadata();
  dst.x1 += static_cast<int>(doviMeta.level5_active_area_left_offset   * scaleX);
  dst.x2 -= static_cast<int>(doviMeta.level5_active_area_right_offset  * scaleX);
  dst.y1 += static_cast<int>(doviMeta.level5_active_area_top_offset    * scaleY);
  dst.y2 -= static_cast<int>(doviMeta.level5_active_area_bottom_offset * scaleY);

  return true;
}

void CRenderManager::ClockAlign()
{
  if (m_dataCacheCore.IsPausedPlayback()) return;

  double speed = static_cast<double>(std::abs(m_dataCacheCore.GetSpeed()));

  WaitDebugInfo waitDbg;

  const auto WaitSlice = [&](double waitUs)
  {
    // Sleep a bounded slice of the remaining gap.
    // i.e. home in on the pts, ramping perceived frame rate until matching
    const double remainingWaitUs = std::max(0.0, (m_presentpts - m_dvdClock.GetClock()) / speed);
    const double frameTimeUs = m_presentframetime / speed;
    double sleepUs = (remainingWaitUs > 1000000)
      ? (frameTimeUs * 4)
      : (frameTimeUs * (((remainingWaitUs / frameTimeUs) / 10) + 1));

    // Never sleep past the remaining target gap.
    sleepUs = std::min(sleepUs, remainingWaitUs);

    waitDbg.used = true;
    waitDbg.usedSlice = true;
    waitDbg.waitUs = remainingWaitUs;
    waitDbg.sliceSleepUs = sleepUs;
    waitDbg.frameTimeUs = frameTimeUs;

    aml_wait(sleepUs);
  };

  const auto Wait = [&](double waitUs)
  {
    // GUI-layer renderers are paced by the normal swap/present path.
    if (!m_pRenderer || m_pRenderer->IsGuiLayer())
    {
      const double remainingWaitUs = std::max(0.0, (m_presentpts - m_dvdClock.GetClock()) / speed);
      waitDbg.used = true;
      waitDbg.gui = true;
      waitDbg.waitUs = remainingWaitUs;
      return aml_wait(remainingWaitUs);
    }

    int nextInUs{0};
    if (!aml_get_time_to_next_vsync_us(nextInUs))
    {
      const double remainingWaitUs = std::max(0.0, (m_presentpts - m_dvdClock.GetClock()) / speed);
      waitDbg.used = true;
      waitDbg.waitUs = remainingWaitUs;
      aml_wait(remainingWaitUs);
      return;
    }

    const double remainingWaitUs = std::max(0.0, (m_presentpts - m_dvdClock.GetClock()) / speed);
    waitDbg.used = true;
    waitDbg.waitUs = remainingWaitUs;
    waitDbg.gotNextIn = true;
    waitDbg.nextInUs = nextInUs;

    // Wait for the pts, but never closer than 8ms to the next vsync.
    // If the pts would land inside the guard window, stop early to leave setup time.
    constexpr int vsyncGuardUs = 8000;
    waitDbg.guardUs = vsyncGuardUs;

    double sleepUs = remainingWaitUs;
    if (remainingWaitUs < nextInUs)
    {
      // pts is before the upcoming vsync: stop early if we'd land inside the guard window.
      const int maxBeforeGuardUs = std::max(0, nextInUs - vsyncGuardUs);

      if (remainingWaitUs > maxBeforeGuardUs)
      {
        int nextInAfterUs{0};
        if (aml_wait_until_next_vsync_window_us(vsyncGuardUs, nextInAfterUs))
        {
          waitDbg.kernelWindow = true;
          waitDbg.sleepUs = static_cast<double>(maxBeforeGuardUs);
          waitDbg.clamped = true;
          waitDbg.gotNextInAfter = true;
          waitDbg.nextInAfterUs = nextInAfterUs;
          return;
        }
      }

      sleepUs = std::min(remainingWaitUs, static_cast<double>(maxBeforeGuardUs));
    }

    waitDbg.sleepUs = sleepUs;
    waitDbg.clamped = (sleepUs < waitUs);

    if (sleepUs > 0)
    {
      aml_wait(sleepUs);

      int nextInAfterUs{0};
      if (aml_get_time_to_next_vsync_us(nextInAfterUs))
      {
        waitDbg.gotNextInAfter = true;
        waitDbg.nextInAfterUs = nextInAfterUs;
      }
    }
  };

  double renderPts = m_dvdClock.GetClock();
  double diff = (renderPts - m_presentpts) / speed;

  // Seek may push the diff to a large negative value, make sure it is sensible.
  // TODO: should be better protected elsewhere.
  if (diff < 0)
  {
    double wait = -diff;

    // Only use the coarse slice path for large gaps; for near-frame waits
    // it can oversleep and consistently land late.
    if (wait > (m_presentframetime * 3.0))
      WaitSlice(wait);
    else
      Wait(wait);

    renderPts = m_dvdClock.GetClock();
    diff = (renderPts - m_presentpts);

    const double initialGapUs = wait;
    const double finalGapUs = -diff;

    // Emit one consolidated line per alignment attempt.
    logM(LOGDEBUG, "gapInit:[{:.0f}] gapFinal:[{:.0f}] ft:[{:.0f}] wait:[{:.0f}] mode:[{}] nextIn:[{}] guard:[{}] sleep:[{:.0f}] clamped:[{}] nextAfter:[{}] sliceSleep:[{:.0f}] presenting:[{:02d}] queued:[{}] skip:[{:02d}]",
                   initialGapUs, finalGapUs, m_presentframetime,
                   waitDbg.used ? waitDbg.waitUs : wait,
                   GetWaitMode(waitDbg),
                   waitDbg.gotNextIn ? waitDbg.nextInUs : -1,
                   waitDbg.guardUs,
                   waitDbg.usedSlice ? 0 : waitDbg.sleepUs,
                   waitDbg.clamped,
                   waitDbg.gotNextInAfter ? waitDbg.nextInAfterUs : -1,
                   waitDbg.usedSlice ? waitDbg.sliceSleepUs : 0.0,
                   m_presentsource, m_queued.size(), m_QueueSkip);

    // Escalate only when we're meaningfully late (gapFinal is negative when late).
    if (finalGapUs < -2000.0)
      logM(LOGWARNING, "late gapFinal:[{:.0f}] ft:[{:.0f}] wait:[{:.0f}] mode:[{}] nextIn:[{}] guard:[{}] sleep:[{:.0f}] clamped:[{}] nextAfter:[{}]",
                       finalGapUs, m_presentframetime,
                       waitDbg.waitUs,
                       GetWaitMode(waitDbg),
                       waitDbg.gotNextIn ? waitDbg.nextInUs : -1,
                       waitDbg.guardUs,
                       waitDbg.usedSlice ? 0 : waitDbg.sleepUs,
                       waitDbg.clamped,
                       waitDbg.gotNextInAfter ? waitDbg.nextInAfterUs : -1);
  }
}

void CRenderManager::Render(bool clear, DWORD flags, DWORD alpha, bool gui)
{
  CSingleExit exitLock(CServiceBroker::GetWinSystem()->GetGfxContext());

  {
    std::unique_lock<CCriticalSection> lock(m_statelock);

    if (!m_presentstarted || (m_renderState != STATE_CONFIGURED))
      return;
  }

  if (!gui && m_pRenderer->IsGuiLayer())
    return;

  const SPresent& present = m_Queue[m_presentsource];

  if (!gui || m_pRenderer->IsGuiLayer())
  {
    if (present.presentmethod == PRESENT_METHOD_BOB)
      PresentFields(clear, flags, alpha);
    else if (present.presentmethod == PRESENT_METHOD_BLEND)
      PresentBlend(clear, flags, alpha);
    else
      PresentSingle(clear, flags, alpha);
  }

  if (gui)
  {
    if (!m_pRenderer->IsGuiLayer())
      m_pRenderer->Update();

    const bool hasOverlay = m_overlays.HasOverlay(m_presentsource);
    m_renderedOverlay = hasOverlay;

    if (hasOverlay || m_renderDebug)
    {
      CRect src, dst, view;
      m_pRenderer->GetVideoRect(src, dst, view);

      if (hasOverlay)
      {
        m_overlays.SetForceInside(CalcOverlayActiveArea(src, dst));
        m_overlays.SetVideoRect(src, dst, view);
        m_overlays.Render(m_presentsource);
      }

      if (m_renderDebug)
      {
        if (m_renderDebugVideo)
        {
          DEBUG_INFO_VIDEO video = m_pRenderer->GetDebugInfo(m_presentsource);
          DEBUG_INFO_RENDER render = CServiceBroker::GetWinSystem()->GetDebugInfo();

          m_debugRenderer.SetInfo(video, render);
        }
        else
        {
          DEBUG_INFO_PLAYER info;

          m_playerPort->GetDebugInfo(info.audio1, info.audio2, info.video, info.player);
          const auto formatMs = [](double milliseconds)
          {
            return StringUtils::Format("{:+07.2f}ms", milliseconds);
          };

          double refreshrate, clockspeed;
          int missedvblanks;

          info.vsync = StringUtils::Format("VSync: off:{}",
                                           formatMs(m_clockSync.m_syncOffset / 1000.0));

          if (m_dvdClock.GetClockInfo(missedvblanks, clockspeed, refreshrate))
            info.vsync += StringUtils::Format(", refresh:{:.3f}Hz, missed:{}, speed:{:.3f}%",
                                              refreshrate, missedvblanks, (clockspeed * 100));

          const double videoLatencyMs = m_videoLatencyTweak;
          const double audioLatencyMs = m_audioLatencyTweak;
          const double userLatencyMs = -m_videoDelay;
          const double totalLatencyMs = videoLatencyMs + audioLatencyMs + userLatencyMs;

          info.latency = StringUtils::Format(
              "Latency: video:{} audio:{} user:{} total:{}",
              formatMs(videoLatencyMs), formatMs(audioLatencyMs),
              formatMs(userLatencyMs), formatMs(totalLatencyMs));

          m_debugRenderer.SetInfo(info);
        }

        m_debugRenderer.Render(src, dst, view);

        m_debugTimer.Set(1000ms);
        m_renderedOverlay = true;
      }
    }
  }

  {
    std::unique_lock<CCriticalSection> lock(m_presentlock);

    if (m_presentstep == PRESENT_FRAME)
    {
      if (present.presentmethod == PRESENT_METHOD_BOB)
        m_presentstep = PRESENT_FRAME2;
      else
        m_presentstep = PRESENT_IDLE;
    }
    else if (m_presentstep == PRESENT_FRAME2)
      m_presentstep = PRESENT_IDLE;

    if (m_presentstep == PRESENT_IDLE)
    {
      if (!m_queued.empty())
        m_presentstep = PRESENT_READY;
    }

    NotifyPresentWaiters();
  }
}

bool CRenderManager::IsGuiLayer()
{
  if (!IsConfigured()) return false;

  std::unique_lock<CCriticalSection> lock(m_statelock);

  if (!m_pRenderer)
    return false;

  // Inline the IsPresenting() check to avoid:
  //  - recursive re-lock of m_statelock (via IsConfigured())
  //  - an extra m_presentlock acquisition when we'd return true via overlay/debug paths anyway
  if (m_pRenderer->IsGuiLayer() && m_renderState == STATE_CONFIGURED)
  {
    std::unique_lock<CCriticalSection> plock(m_presentlock);
    if (!m_presentTimer.IsTimePast())
      return true;
  }

  if (m_renderedOverlay || m_overlays.HasOverlay(m_presentsource))
    return true;

  if (m_renderDebug && m_debugTimer.IsTimePast())
    return true;

  return false;
}

bool CRenderManager::IsVideoLayer() {
  if (!IsConfigured()) return false;

  {
    std::unique_lock<CCriticalSection> lock(m_statelock);

    if (!m_pRenderer)
      return false;

    if (!m_pRenderer->IsGuiLayer())
      return true;
  }
  return false;
}

void inline CRenderManager::RenderUpdate(bool clear, unsigned int flags, unsigned int alpha)
{
  ClockAlign();
  m_pRenderer->RenderUpdate(m_presentsource, m_presentsource, clear, flags, alpha);
  m_dataCacheCore.SetRenderPts(m_presentpts);
}

/* simple present method */
void CRenderManager::PresentSingle(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& present = m_Queue[m_presentsource];

  if (present.presentfield == FS_BOT)
    RenderUpdate(clear, flags | RENDER_FLAG_BOT, alpha);
  else if (present.presentfield == FS_TOP)
    RenderUpdate(clear, flags | RENDER_FLAG_TOP, alpha);
  else
    RenderUpdate(clear, flags, alpha);
}

/* new simpler method of handling interlaced material, *
 * we just render the two fields right after each other */
void CRenderManager::PresentFields(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& present = m_Queue[m_presentsource];

  if (m_presentstep == PRESENT_FRAME)
  {
    if (present.presentfield == FS_BOT)
      RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD0, alpha);
    else
      RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD0, alpha);
  }
  else
  {
    if (present.presentfield == FS_TOP)
      RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_FIELD1, alpha);
    else
      RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_FIELD1, alpha);
  }
}

void CRenderManager::PresentBlend(bool clear, DWORD flags, DWORD alpha)
{
  const SPresent& present = m_Queue[m_presentsource];

  if (present.presentfield == FS_BOT)
  {
    RenderUpdate(clear, flags | RENDER_FLAG_BOT | RENDER_FLAG_NOOSD, alpha);
    RenderUpdate(false, flags | RENDER_FLAG_TOP, alpha / 2);
  }
  else
  {
    RenderUpdate(clear, flags | RENDER_FLAG_TOP | RENDER_FLAG_NOOSD, alpha);
    RenderUpdate(false, flags | RENDER_FLAG_BOT, alpha / 2);
  }
}

void CRenderManager::UpdateVideoLatencyTweak()
{
  float fps = CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS();
  const RESOLUTION_INFO res = CServiceBroker::GetWinSystem()->GetGfxContext().GetResInfo();

  float refresh = fps;
  if (CServiceBroker::GetWinSystem()->GetGfxContext().GetVideoResolution() == RES_WINDOW)
    refresh = 0; // No idea about refresh rate when windowed, just get the default latency

  m_videoLatencyTweak = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->GetVideoLatencyTweak(refresh, res.iScreenHeight);
}

void CRenderManager::UpdateResolution(bool force)
{
  std::unique_lock<CCriticalSection> lock(m_resolutionlock);

  if (!m_bTriggerUpdateResolution) return;

  auto& gfxContext = CServiceBroker::GetWinSystem()->GetGfxContext();

  if (!gfxContext.IsFullScreenRoot()) return;

  const RenderStereoMode user_stereo_mode =
      CServiceBroker::GetGUI()->GetStereoscopicsManager().GetStereoModeByUser();
  auto playbackMode = static_cast<STEREOSCOPIC_PLAYBACK_MODE>(
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
          CSettings::SETTING_VIDEOPLAYER_STEREOSCOPICPLAYBACKMODE));

  if (!m_picture.stereoMode.empty() &&
      playbackMode == STEREOSCOPIC_PLAYBACK_MODE_ASK &&
      user_stereo_mode == RenderStereoMode::UNDEFINED)
    m_bTriggerUpdateResolution = false;

  if (m_bTriggerUpdateResolution)
  {
    // Some platforms send a follow-up "reassert" trigger with no params
    // (fps/width/height = 0) after a mode switch/reset. In that case, prefer
    // keeping the currently applied HDR type (if any) to avoid an unnecessary
    // second mode switch (e.g. VS10 HDR10->DV mapping).
    StreamHdrType desiredHdrType = m_hasHdrTypeOverride ? m_hdrType_override : m_picture.hdrType;
    if (!m_hasHdrTypeOverride && m_bTriggerUpdateResolutionNoParams)
    {
      const auto currentHdrType = gfxContext.GetHDRType();
      if (currentHdrType != StreamHdrType::HDR_TYPE_NONE)
        desiredHdrType = currentHdrType;
    }

    RESOLUTION desiredRes = gfxContext.GetVideoResolution();
    if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt(
            CSettings::SETTING_VIDEOPLAYER_ADJUSTREFRESHRATE) != ADJUST_REFRESHRATE_OFF &&
        m_fps > 0.0f)
    {
      desiredRes = CResolutionUtils::ChooseBestResolution(
          m_fps, m_picture.iWidth, m_picture.iHeight, !m_picture.stereoMode.empty());
    }

    if (m_pendingHdrPolicy) aml_dv_open(*m_pendingHdrPolicy);

    const bool needsApply = gfxContext.GetHDRType() != desiredHdrType ||
                gfxContext.GetVideoResolution() != desiredRes;

    if (needsApply)
    {
      logM(LOGINFO, "Before - Set fps [{}] width [{}] height [{}] stereomode empty [{}] hdr type [{}]",
                    m_fps, m_picture.iWidth, m_picture.iHeight, m_picture.stereoMode.empty(),
                    CStreamDetails::DynamicRangeToString(desiredHdrType));

      gfxContext.SetHDRType(desiredHdrType);
      gfxContext.SetVideoResolution(desiredRes, false);
      UpdateVideoLatencyTweak();

      logM(LOGINFO, "After - Set fps [{}] width [{}] height [{}] stereomode empty [{}] hdr type [{}]",
                    m_fps, m_picture.iWidth, m_picture.iHeight, m_picture.stereoMode.empty(),
                    CStreamDetails::DynamicRangeToString(desiredHdrType));

      if (m_pRenderer)
        m_pRenderer->Update();
    }
  }

  m_bTriggerUpdateResolution = false;
  m_bTriggerUpdateResolutionNoParams = false;
  m_hdrType_override = StreamHdrType::HDR_TYPE_NONE;
  m_hasHdrTypeOverride = false;
  m_pendingHdrPolicy.reset();

  if (m_pendingResolutionTimingActive)
  {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_pendingResolutionStartTime);
    logM(LOGINFO, "Pending resolution change cleared after [{:d}] ms", elapsed.count());
    m_pendingResolutionTimingActive = false;
  }

  m_playerPort->VideoParamsChange();
}

void CRenderManager::TriggerUpdateResolutionHdr(StreamHdrType hdr)
{
  logM(LOGINFO, "hdr type [{}] current trigger [{}]",
                CStreamDetails::DynamicRangeToString(hdr), m_bTriggerUpdateResolution);

  if (!m_bTriggerUpdateResolution)
  {
    m_pendingResolutionStartTime = std::chrono::steady_clock::now();
    m_pendingResolutionTimingActive = true;
    logM(LOGINFO, "Pending resolution change armed for hdr-only trigger");
  }

  m_hdrType_override = hdr;
  m_hasHdrTypeOverride = true;
  m_pendingHdrPolicy.reset();
  m_bTriggerUpdateResolution = true;
}

void CRenderManager::TriggerUpdateResolutionHdr(const AMLHdrSetupPolicy& hdrPolicy)
{
  logM(LOGINFO, "hdr type [{}] current trigger [{}]",
                CStreamDetails::DynamicRangeToString(hdrPolicy.finalHdr), m_bTriggerUpdateResolution);

  if (!m_bTriggerUpdateResolution)
  {
    m_pendingResolutionStartTime = std::chrono::steady_clock::now();
    m_pendingResolutionTimingActive = true;
    logM(LOGINFO, "Pending resolution change armed for hdr-policy trigger");
  }

  m_hdrType_override = hdrPolicy.finalHdr;
  m_hasHdrTypeOverride = true;
  m_pendingHdrPolicy = std::make_unique<AMLHdrSetupPolicy>(hdrPolicy);
  m_bTriggerUpdateResolution = true;
}

void CRenderManager::TriggerUpdateResolution(float fps, int width, int height, std::string& stereo)
{
  logM(LOGINFO, "fps [{}] width [{}] height [{}] stereomode empty [{}] current trigger [{}]",
                fps, width, height, m_picture.stereoMode.empty(), m_bTriggerUpdateResolution);

  if (!m_bTriggerUpdateResolution)
  {
    m_pendingResolutionStartTime = std::chrono::steady_clock::now();
    m_pendingResolutionTimingActive = true;
    logM(LOGINFO, "Pending resolution change armed for fps/resolution trigger");
  }

  m_bTriggerUpdateResolutionNoParams = (width == 0);
  if (width)
  {
    m_fps = fps;
    m_picture.iWidth = width;
    m_picture.iHeight = height;
    m_picture.stereoMode = stereo;
  }
  m_bTriggerUpdateResolution = true;
}

void CRenderManager::ToggleDebug()
{
  m_renderDebug = !m_renderDebug;
  m_debugTimer.SetExpired();
  m_renderDebugVideo = false;
}

void CRenderManager::ToggleDebugVideo()
{
  m_renderDebug = !m_renderDebug;
  m_debugTimer.SetExpired();
  m_renderDebugVideo = true;
}

void CRenderManager::SetSubtitleVerticalPosition(int value, bool save)
{
  m_overlays.SetSubtitleVerticalPosition(value, save);
}

bool CRenderManager::AddVideoPicture(const VideoPicture& picture, volatile std::atomic_bool& bStop, EINTERLACEMETHOD deintMethod, bool wait)
{
  int index;
  {
    std::unique_lock lock(m_presentlock);
    if (m_free.empty())
      return false;
    index = m_free.front();
    m_free.pop_front();
  }

  bool wantsDoublePass = false;
  {
    std::lock_guard lock2(m_datalock);
    if (!m_pRenderer)
    {
      std::unique_lock lock(m_presentlock);
      m_free.push_front(index);
      return false;
    }

    m_pRenderer->AddVideoPicture(picture, index);
    wantsDoublePass = m_pRenderer->WantsDoublePass();
  }

  // set fieldsync if picture is interlaced
  EFIELDSYNC displayField = FS_NONE;
  if (picture.iFlags & DVP_FLAG_INTERLACED)
  {
    if (deintMethod != EINTERLACEMETHOD::VS_INTERLACEMETHOD_NONE)
    {
      if (picture.iFlags & DVP_FLAG_TOP_FIELD_FIRST)
        displayField = FS_TOP;
      else
        displayField = FS_BOT;
    }
  }

  EPRESENTMETHOD presentmethod = PRESENT_METHOD_SINGLE;
  if (deintMethod == VS_INTERLACEMETHOD_NONE)
  {
    presentmethod = PRESENT_METHOD_SINGLE;
    displayField = FS_NONE;
  }
  else
  {
    if (displayField == FS_NONE)
      presentmethod = PRESENT_METHOD_SINGLE;
    else
    {
      if (deintMethod == VS_INTERLACEMETHOD_RENDER_BLEND)
        presentmethod = PRESENT_METHOD_BLEND;
      else if (deintMethod == VS_INTERLACEMETHOD_RENDER_BOB)
        presentmethod = PRESENT_METHOD_BOB;
      else
      {
        if (!wantsDoublePass)
          presentmethod = PRESENT_METHOD_SINGLE;
        else
          presentmethod = PRESENT_METHOD_BOB;
      }
    }
  }

  std::unique_lock lock(m_presentlock);

  SPresent& present = m_Queue[index];
  present.presentfield = displayField;
  present.presentmethod = presentmethod;
  present.pts = picture.pts;
  present.duration = picture.iDuration;

  // Keep the queue sorted by pts so the render tick doesn't have to scan/sort.
  // Index uniqueness is guaranteed by the free-list design (m_free -> m_queued -> m_discard -> m_free).
  const double pts = present.pts;
  if (m_queued.empty() || m_Queue[m_queued.back()].pts <= pts)
  {
    m_queued.push_back(index);
  }
  else
  {
    auto insertPos = std::upper_bound(
        m_queued.begin(), m_queued.end(), pts,
        [this](double ptsValue, int queuedIndex) { return ptsValue < m_Queue[queuedIndex].pts; });
    m_queued.insert(insertPos, index);
  }

  // signal to any waiters to check state
  if (m_presentstep == PRESENT_IDLE)
  {
    m_presentstep = PRESENT_READY;
    NotifyPresentWaiters();
  }

  if (wait)
  {
    m_forceNext = true;
    XbmcThreads::EndTime<> endtime(200ms);
    while (m_presentstep == PRESENT_READY)
    {
      WaitPresent(lock, 20ms);
      if (endtime.IsTimePast() || bStop)
      {
        if (!bStop)
        {
          CLog::Log(LOGWARNING, "CRenderManager::AddVideoPicture - timeout waiting for render");
        }
        break;
      }
    }
    m_forceNext = false;
  }

  return true;
}

void CRenderManager::AddOverlay(std::shared_ptr<CDVDOverlay> o, double pts)
{
  int idx;
  {
    std::unique_lock<CCriticalSection> lock(m_presentlock);

    if (m_free.empty())
      return;
    idx = m_free.front();
  }
  std::unique_lock<CCriticalSection> lock(m_datalock);

  m_overlays.AddOverlay(std::move(o), pts, idx);
}

bool CRenderManager::Supports(ERENDERFEATURE feature) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(feature);
  else
    return false;
}

bool CRenderManager::Supports(ESCALINGMETHOD method) const
{
  std::unique_lock lock(m_statelock);
  if (m_pRenderer)
    return m_pRenderer->Supports(method);
  else
    return false;
}

int CRenderManager::WaitForBuffer(volatile std::atomic_bool& bStop,
                                  std::chrono::milliseconds timeout)
{
  std::unique_lock lock(m_presentlock);

  // check if gui is active and discard buffer if not
  // this keeps videoplayer going
  if (!m_bRenderGUI || !g_application.GetRenderGUI())
  {
    m_bRenderGUI = false;
    double presenttime = 0;
    double clock = m_dvdClock.GetClock();
    if (!m_queued.empty())
    {
      int idx = m_queued.front();
      presenttime = m_Queue[idx].pts;
    }
    else
      presenttime = clock + 0.02;

    auto sleeptime = std::chrono::milliseconds(static_cast<int>((presenttime - clock) * 1000));
    if (sleeptime < 0ms)
      sleeptime = 0ms;
    sleeptime = std::min(sleeptime, 20ms);
    WaitPresent(lock, sleeptime);
    DiscardBufferLocked();
    return 0;
  }

  XbmcThreads::EndTime<> endtime{timeout};
  while(m_free.empty())
  {
    WaitPresent(lock, std::min(50ms, timeout));
    if (endtime.IsTimePast() || bStop)
    {
      return -1;
    }
  }

  // make sure overlay buffer is released, this won't happen on AddOverlay
  m_overlays.Release(m_free.front());

  // return buffer level
  return m_queued.size() + m_discard.size();
}

void inline CRenderManager::SetPresentSource()
{
  if (m_presentstarted)
  {
    if (m_discard.empty() || (m_discard.back() != m_presentsource))
      m_discard.push_back(m_presentsource);
  }
  m_presentsource = m_queued.front();
  m_presentpts = m_Queue[m_presentsource].pts;
  m_presentframetime = m_Queue[m_presentsource].duration;
}

bool inline CRenderManager::Paused(bool paused, double clock)
{
  // for pause, check for frame advance
  bool check = paused ? (clock == m_previousPauseClock) : paused;

  m_previousPauseClock = clock;

  return check;
}

void CRenderManager::PrepareNextRender()
{
  if (!m_showVideo && !m_forceNext) return;

  double renderPts = m_dvdClock.GetClock();

  float speed = m_dataCacheCore.GetSpeed();
  bool paused = Paused((speed == 0.0f), renderPts);

  if (paused) return;

  bool playing = (speed == 1.0f);
  SetPresentSource(); // get next frame

  if (m_dvdClock.GetClockSpeed() < 0)
  {
    logM(LOGINFO, "negative clock speed detected!");
    m_presentpts = renderPts;
  }

  // How far away are we from the clock (renderPts)
  double diff = (renderPts - m_presentpts);

  if (m_clockSync.m_enabled)
  {
    m_presentframetime = 1.0 / static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS()) * DVD_TIME_BASE;
    double err = fmod(diff, m_presentframetime);
    m_clockSync.m_error += err;
    m_clockSync.m_errCount ++;
    if (m_clockSync.m_errCount > 30)
    {
      double average = m_clockSync.m_error / m_clockSync.m_errCount;
      m_clockSync.m_syncOffset = average;
      m_clockSync.m_error = 0;
      m_clockSync.m_errCount = 0;

      m_dvdClock.SetVsyncAdjust(-average);
    }
    renderPts += m_presentframetime / 2 - m_clockSync.m_syncOffset;
    diff = (renderPts - m_presentpts);
  }
  else
  {
    m_dvdClock.SetVsyncAdjust(0);
  }

  // remove late frames from queue - present from next up frame.
  if (diff > 0)
    while ((diff > -500) && (m_queued.size() > 2))
    {
      if (playing) m_QueueSkip++;
      m_queued.pop_front(); // skip this frame
      SetPresentSource();   // get next frame
      diff = (renderPts - m_presentpts);
    }

  m_lateframes = static_cast<int>(std::max(0.0, diff / m_presentframetime));
  m_presentstep = PRESENT_FLIP;
  m_presentstarted = true;
  m_queued.pop_front();
  NotifyPresentWaiters();

  logComponentM(LOGDEBUG, LOGAVTIMING, "render:[{:.3f}] presenting:[{:02d}] [{:.3f}] "
                                       "diff:[{:.3f}] "
                                       "queued:[{:02d}] frametime:[{:.3f}] skip:[{:02d}] force:[{:d}] speed:[{:.1f}]",
                                       (renderPts / DVD_TIME_BASE), m_presentsource, (m_presentpts / DVD_TIME_BASE),
                                       (diff / DVD_TIME_BASE),
                                       m_queued.size(), (m_presentframetime / DVD_TIME_BASE), m_QueueSkip, m_forceNext, speed);
}

void CRenderManager::DiscardBuffer()
{
  std::unique_lock lock2(m_presentlock);

  DiscardBufferLocked();
}

void CRenderManager::DiscardBufferLocked()
{
  while (!m_queued.empty())
  {
    m_discard.push_back(m_queued.front());
    m_queued.pop_front();
  }

  if (m_presentstep == PRESENT_READY)
    m_presentstep = PRESENT_IDLE;
  NotifyPresentWaiters();
}

bool CRenderManager::GetStats(int &lateframes, double &pts, int &queued, int &discard)
{
  std::unique_lock lock(m_presentlock);
  lateframes = m_lateframes / 10;
  pts = m_presentpts;
  queued = m_queued.size();
  discard  = m_discard.size();
  return true;
}

int CRenderManager::GetQueuedFrames() const
{
  std::unique_lock<CCriticalSection> lock(m_presentlock);
  return static_cast<int>(m_queued.size());
}

int CRenderManager::GetQueueSize() const
{
  return m_QueueSize.load(std::memory_order_relaxed);
}

double CRenderManager::GetRenderPts()
{
  return m_presentpts.load(std::memory_order_relaxed);
}

void CRenderManager::CheckEnableClockSync()
{
  // refresh rate can be a multiple of video fps
  double diff = 1.0;

  if (m_fps != 0)
  {
    double fps = static_cast<double>(m_fps);
    double refreshrate, clockspeed;
    int missedvblanks;
    if (m_dvdClock.GetClockInfo(missedvblanks, clockspeed, refreshrate))
    {
      fps *= clockspeed;
    }

    diff = static_cast<double>(CServiceBroker::GetWinSystem()->GetGfxContext().GetFPS()) / fps;
    if (diff < 1.0)
      diff = 1.0 / diff;

    // Calculate distance from nearest integer proportion
    diff = std::abs(std::round(diff) - diff);
  }

  if (diff && (diff > 0.0005))
  {
    m_clockSync.m_enabled = true;
  }
  else
  {
    m_clockSync.m_enabled = false;
    m_dvdClock.SetVsyncAdjust(0);
  }

  m_playerPort->UpdateClockSync(m_clockSync.m_enabled);
}
