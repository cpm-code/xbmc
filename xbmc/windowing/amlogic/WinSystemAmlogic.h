/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "AMLDisplay.h"
#include "DynamicDll.h"

#include "platform/linux/input/LibInputHandler.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "threads/CriticalSection.h"
#include "windowing/WinSystem.h"
#include "threads/SystemClock.h"
#include "system_egl.h"
#include "utils/EGLFence.h"
#include "utils/EGLUtils.h"
#include <EGL/fbdev_window.h>
#include <gbm.h>

class IDispResource;

class DllMaliInterface
{
public:
  virtual ~DllMaliInterface() = default;
  virtual struct gbm_device *gbm_create_device(int fd) = 0;
};

class DllMali : public DllDynamic, public DllMaliInterface
{
public:
  DECLARE_DLL_WRAPPER(DllMali, "libMali.so")
  DEFINE_METHOD1(struct gbm_device *, gbm_create_device, (int p1))
  BEGIN_METHOD_RESOLVE()
    RESOLVE_METHOD(gbm_create_device)
  END_METHOD_RESOLVE()
};

class CWinSystemAmlogic : public CWinSystemBase
{
public:
  CWinSystemAmlogic();
  virtual ~CWinSystemAmlogic();

  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;

  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;

  bool DestroyWindow() override;
  void UpdateResolutions() override;
  bool IsHDRDisplay() override;
  CHDRCapabilities GetDisplayHDRCapabilities() const override;
  float GetGuiSdrPeakLuminance() const override;
  HDR_STATUS GetOSHDRStatus() override;

  bool Hide() override;
  bool Show(bool show = true) override;
  virtual void Register(IDispResource *resource);
  virtual void Unregister(IDispResource *resource);

  static void SettingOptionsComponentsFiller(const std::shared_ptr<const CSetting>& setting,
                                             std::vector<IntegerSettingOption>& list,
                                             int& current);

  void MonitorStart();
  void MonitorStop();
protected:
  std::string m_framebuffer_name;
  EGLDisplay m_nativeDisplay;
  fbdev_window *m_nativeWindow;

  RenderStereoMode m_stereo_mode;

  bool m_delayDispReset;
  XbmcThreads::EndTime<> m_dispResetTimer;

  CCriticalSection m_resourceSection;
  std::vector<IDispResource*> m_resources;
  std::unique_ptr<CLibInputHandler> m_libinput;
  CHDRCapabilities m_hdr_caps;
  bool m_force_mode_switch;
  bool m_nativeGUI;
  static std::unique_ptr<CAMLDisplay> m_amlDisplay;
  std::unique_ptr<CAMLGBMUtils> m_amlGBMUtils = nullptr;
  std::unique_ptr<KODI::UTILS::EGL::CEGLFence> m_eglFence;
private:
  struct callback_data
  {
    struct udev_monitor* udevMonitor;
    CWinSystemAmlogic* object;
  };

  void RefreshResolutions();
  void HotplugEvent();
  static void FDEventCallback(int id, int fd, short revents, void *data);

  int m_fdMonitorId;

  struct udev *m_udev;
  struct callback_data m_callback_data;
};
