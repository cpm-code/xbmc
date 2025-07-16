/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoSyncAML.h"
#include "WinSystemAmlogicGLESContext.h"
#include "platform/linux/SysfsPath.h"
#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/MathUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "windowing/GraphicContext.h"
#include "windowing/WindowSystemFactory.h"

using namespace KODI;
using namespace KODI::WINDOWING::AML;

CWinSystemAmlogicGLESContext::CWinSystemAmlogicGLESContext()
: m_pGLContext(new CEGLContextUtils(EGL_PLATFORM_GBM_MESA, "EGL_EXT_platform_base"))
{
}

void CWinSystemAmlogicGLESContext::Register()
{
  KODI::WINDOWING::CWindowSystemFactory::RegisterWindowSystem(CreateWinSystem, "aml");
}

std::unique_ptr<CWinSystemBase> CWinSystemAmlogicGLESContext::CreateWinSystem()
{
  return std::make_unique<CWinSystemAmlogicGLESContext>();
}

bool CWinSystemAmlogicGLESContext::InitWindowSystem()
{
  if (!CWinSystemAmlogic::InitWindowSystem())
  {
    return false;
  }

  if (m_amlGBMUtils)
  {
    if (!m_pGLContext->CreatePlatformDisplay(m_amlGBMUtils->GetDevice(), m_amlGBMUtils->GetDevice()))
      return false;

    if (m_amlDisplay->aml_get_display_connected())
      m_amlDisplay->aml_set_drmDevice_active(true);
  }
  else
  {
    if (!m_pGLContext->CreateDisplay(m_nativeDisplay))
      return false;
  }

  if (!m_pGLContext->InitializeDisplay(EGL_OPENGL_ES_API))
  {
    return false;
  }

  if (!m_pGLContext->ChooseConfig(EGL_OPENGL_ES2_BIT))
  {
    return false;
  }

  CEGLAttributesVec contextAttribs;
  contextAttribs.Add({{EGL_CONTEXT_CLIENT_VERSION, 2}});

  if (!m_pGLContext->CreateContext(contextAttribs))
  {
    return false;
  }

  if (m_amlGBMUtils &&
      CEGLUtils::HasExtension(GetEGLDisplay(), "EGL_ANDROID_native_fence_sync") &&
      CEGLUtils::HasExtension(GetEGLDisplay(), "EGL_KHR_fence_sync"))
  {
    m_eglFence = std::make_unique<KODI::UTILS::EGL::CEGLFence>(GetEGLDisplay());
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindowSystem()
{
  if (m_amlGBMUtils && m_amlDisplay->aml_get_display_connected())
    m_amlDisplay->aml_set_drmDevice_active(false);

  m_pGLContext->DestroyContext();
  m_pGLContext->Destroy();
  return CWinSystemAmlogic::DestroyWindowSystem();
}

bool CWinSystemAmlogicGLESContext::CreateNewWindow(const std::string& name,
                                               bool fullScreen,
                                               RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  const RenderStereoMode stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  // check for frac_rate_policy change
  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;
  int cur_fractional_rate = m_amlDisplay->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR);

  bool nativeGUI = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);

  StreamHdrType hdrType = CServiceBroker::GetWinSystem()->GetGfxContext().GetHDRType();
  bool force_mode_switch_by_dv = false;
  if (aml_dolby_vision_enabled() &&
     ((m_hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION && hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION) ||
      (m_hdrType != StreamHdrType::HDR_TYPE_DOLBYVISION && hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)))
      force_mode_switch_by_dv = true;

  // get current used resolution
  if (!m_amlDisplay->aml_get_native_resolution(&current_resolution))
  {
    CLog::Log(LOGERROR, "CWinSystemAmlogicGLESContext::{}: failed to receive current resolution", __FUNCTION__);
    return false;
  }

  const std::string new_hdrStr = CStreamDetails::HdrTypeToString(hdrType);
  const std::string old_hdrStr = CStreamDetails::HdrTypeToString(m_hdrType);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "m_bWindowCreated: {}, "
    "frac rate {:d}({:d}), "
    "hdrType: {}({}), force mode switch: {}",
    __FUNCTION__,
    m_bWindowCreated,
    fractional_rate, cur_fractional_rate,
    new_hdrStr.empty() ? "none" : new_hdrStr, old_hdrStr.empty() ? "none" : old_hdrStr, force_mode_switch_by_dv);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "cur: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    current_resolution.iWidth, current_resolution.iHeight, current_resolution.iScreenWidth, current_resolution.iScreenHeight,
    current_resolution.fRefreshRate, current_resolution.dwFlags, m_nativeGUI);
  CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: "
    "res: iWidth: {:04d}, iHeight: {:04d}, iScreenWidth: {:04d}, iScreenHeight: {:04d}, fRefreshRate: {:02.2f}, dwFlags: {:02x}, nativeGUI: {}",
    __FUNCTION__,
    res.iWidth, res.iHeight, res.iScreenWidth, res.iScreenHeight, res.fRefreshRate, res.dwFlags, nativeGUI);

  // check if mode switch is needed
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      m_bFullScreen == fullScreen && current_resolution.fRefreshRate == res.fRefreshRate &&
      (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
      m_stereo_mode == stereo_mode && m_bWindowCreated &&
      !force_mode_switch_by_dv &&
      (fractional_rate == cur_fractional_rate) &&
      nativeGUI == m_nativeGUI)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: No need to create a new window", __FUNCTION__);
    return true;
  }

  // destroy old window, then create a new one
  DestroyWindow();

  // check if a forced mode switch is required
  if (current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
      current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
      MathUtils::FloatEquals(current_resolution.fRefreshRate, res.fRefreshRate, 0.06f))
  {
    // same resolution, check frac rate and other parameter
    if ((cur_fractional_rate != fractional_rate) || force_mode_switch_by_dv || (m_stereo_mode != stereo_mode))
    {
      m_force_mode_switch = true;
      CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{}: force mode switch", __FUNCTION__);
    }
  }

  // refresh backup data
  m_hdrType = hdrType;
  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;
  m_nativeGUI = nativeGUI;

  if (!CWinSystemAmlogic::CreateNewWindow(name, fullScreen, res))
  {
    return false;
  }

  if (m_amlGBMUtils)
  {
    uint32_t format = m_pGLContext->GetConfigAttrib(EGL_NATIVE_VISUAL_ID);
    if (!m_amlGBMUtils->CreateSurface(res.iWidth, res.iHeight, format))
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create GBM surface", __FUNCTION__);
      return false;
    }

    if (!m_pGLContext->CreatePlatformSurface(
            m_amlGBMUtils->GetSurface(),
            reinterpret_cast<EGLNativeWindowType>(m_amlGBMUtils->GetSurface())))
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogicGLESContext::{} - failed to create CreatePlatformSurface", __FUNCTION__);
      return false;
    }
  }
  else
  {
    if (m_nativeWindow == NULL)
      m_nativeWindow = new fbdev_window;

    m_nativeWindow->width = res.iWidth;
    m_nativeWindow->height = res.iHeight;

    if (!m_pGLContext->CreateSurface(static_cast<EGLNativeWindowType>(m_nativeWindow)))
    {
      return false;
    }
  }

  if (!m_pGLContext->BindContext())
  {
    return false;
  }

  if (!m_delayDispReset)
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
      (*i)->OnResetDisplay();
  }

  return true;
}

bool CWinSystemAmlogicGLESContext::DestroyWindow()
{
  m_pGLContext->DestroySurface();
  return CWinSystemAmlogic::DestroyWindow();
}

bool CWinSystemAmlogicGLESContext::ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop)
{
  CRenderSystemGLES::ResetRenderSystem(newWidth, newHeight);
  return true;
}

bool CWinSystemAmlogicGLESContext::SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays)
{
  CreateNewWindow("", fullScreen, res);
  CRenderSystemGLES::ResetRenderSystem(res.iWidth, res.iHeight);
  return true;
}

void CWinSystemAmlogicGLESContext::SetVSyncImpl(bool enable)
{
  if (!m_pGLContext->SetVSync(enable))
  {
    CLog::Log(LOGERROR, "{},Could not set egl vsync", __FUNCTION__);
  }
}

void CWinSystemAmlogicGLESContext::PresentRender(bool rendered, bool videoLayer)
{
  SetVSync(true);
  if (rendered || (videoLayer && m_amlGBMUtils))
  {
#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
    if (m_eglFence)
    {
      int fd = m_amlDisplay->TakeOutFenceFd();
      if (fd != -1)
      {
        m_eglFence->CreateKMSFence(fd);
        m_eglFence->WaitSyncGPU();
      }

      m_eglFence->CreateGPUFence();
    }
#endif

    // Ignore errors - eglSwapBuffers() sometimes fails during modeswaps on AML,
    // there is probably nothing we can do about it
    m_pGLContext->TrySwapBuffers();

#if defined(EGL_ANDROID_native_fence_sync) && defined(EGL_KHR_fence_sync)
    if (m_eglFence)
    {
      int fd = m_eglFence->FlushFence();
      m_amlDisplay->SetInFenceFd(fd);

      m_eglFence->WaitSyncCPU();
    }
#endif

    if (m_amlGBMUtils)
    {
      m_amlGBMUtils->LockFrontBuffer(m_amlDisplay->aml_get_Device_handle());
      m_amlDisplay->FlipPage(m_amlGBMUtils->GetFBId());
      m_amlGBMUtils->UnlockFrontBuffer();
    }

    if (m_delayDispReset && m_dispResetTimer.IsTimePast())
    {
      m_delayDispReset = false;
      std::unique_lock<CCriticalSection> lock(m_resourceSection);
      // tell any shared resources
      for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
        (*i)->OnResetDisplay();
    }
  }
  else if (!rendered && !videoLayer)
  {
    m_amlDisplay->aml_drmDevice_vsync();
  }
}

EGLDisplay CWinSystemAmlogicGLESContext::GetEGLDisplay() const
{
  return m_pGLContext->GetEGLDisplay();
}

EGLSurface CWinSystemAmlogicGLESContext::GetEGLSurface() const
{
  return m_pGLContext->GetEGLSurface();
}

EGLContext CWinSystemAmlogicGLESContext::GetEGLContext() const
{
  return m_pGLContext->GetEGLContext();
}

EGLConfig  CWinSystemAmlogicGLESContext::GetEGLConfig() const
{
  return m_pGLContext->GetEGLConfig();
}

std::unique_ptr<CVideoSync> CWinSystemAmlogicGLESContext::GetVideoSync(CVideoReferenceClock *clock)
{
  std::unique_ptr<CVideoSync> pVSync(new CVideoSyncAML(clock));
  return pVSync;
}

bool CWinSystemAmlogicGLESContext::SupportsStereo(const RenderStereoMode mode) const
{
  if (aml_display_support_3d() &&
      mode == RenderStereoMode::HARDWAREBASED) {
    // yes, we support hardware based MVC decoding
    return true;
  }

  return CRenderSystemGLES::SupportsStereo(mode);
}
