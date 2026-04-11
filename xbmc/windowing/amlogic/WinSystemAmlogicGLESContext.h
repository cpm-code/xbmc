/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/EGLUtils.h"
#include "rendering/gles/RenderSystemGLES.h"
#include "utils/GlobalsHandling.h"
#include "utils/StreamDetails.h"
#include "WinSystemAmlogic.h"

namespace KODI
{
namespace WINDOWING
{
namespace AML
{

class CWinSystemAmlogicGLESContext : public CWinSystemAmlogic, public CRenderSystemGLES
{
public:
  CWinSystemAmlogicGLESContext() = default;
  virtual ~CWinSystemAmlogicGLESContext() = default;

  using CWinSystemAmlogic::Register;
  static void Register();
  static std::unique_ptr<CWinSystemBase> CreateWinSystem();

  // Implementation of CWinSystemBase via CWinSystemAmlogic
  CRenderSystemBase *GetRenderSystem() override { return this; }
  bool InitWindowSystem() override;
  bool DestroyWindowSystem() override;
  bool CreateNewWindow(const std::string& name,
                       bool fullScreen,
                       RESOLUTION_INFO& res) override;
  bool DestroyWindow() override;
  void SetDirtyRegions(const CDirtyRegionList& dirtyRegions) override
  {
    m_pGLContext.SetDamagedRegions(dirtyRegions);
  }
  int GetBufferAge() override
  {
    // keep at 0 for now, cost seems higher if trying to just do partial updates.
    return 0;

    int bufferAge = m_pGLContext.GetBufferAge();
    if (bufferAge <= 0) return bufferAge;

    // DirtyRegionTracker already keeps one extra frame of history. AML only
    // needs a small additional guard in case the preserved GUI buffer age is
    // reported one frame younger than the recycled buffer actually is.
    constexpr int AML_GUI_BUFFER_AGE_MARGIN{1};
    constexpr int AML_GUI_BUFFER_AGE_CAP{4};
    bufferAge += AML_GUI_BUFFER_AGE_MARGIN;

    return (bufferAge > AML_GUI_BUFFER_AGE_CAP)
      ? AML_GUI_BUFFER_AGE_CAP
      : bufferAge;
  }

  bool BindTextureUploadContext() override;
  bool UnbindTextureUploadContext() override;
  bool HasContext() override;

  bool ResizeWindow(int newWidth, int newHeight, int newLeft, int newTop) override;
  bool SetFullScreen(bool fullScreen, RESOLUTION_INFO& res, bool blankOtherDisplays) override;

  virtual std::unique_ptr<CVideoSync> GetVideoSync(CVideoReferenceClock *clock) override;

  bool SupportsStereo(RenderStereoMode mode) const override;

  EGLDisplay GetEGLDisplay() const;
  EGLSurface GetEGLSurface() const;
  EGLContext GetEGLContext() const;
  EGLConfig  GetEGLConfig() const;
protected:
  void SetVSyncImpl(bool enable) override;
  void PresentRenderImpl(bool rendered) override;

private:
  CEGLContextUtils m_pGLContext;
  StreamHdrType m_hdrType = StreamHdrType::HDR_TYPE_NONE;
};

}
}
}
