/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAmlogic.h"

#include <string.h>
#include <float.h>

#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "platform/linux/ScreenshotSurfaceAML.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/SettingsManager.h"
#include "settings/lib/Setting.h"
#include "guilib/DispResource.h"
#include "guilib/LocalizeStrings.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "threads/SingleLock.h"
#include "interfaces/AnnouncementManager.h"

#include "platform/linux/SysfsPath.h"

#include <linux/fb.h>
#include <linux/version.h>

#include "system_egl.h"

#define DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL (unsigned int)(1)
#define DOLBY_VISION_OUTPUT_MODE_HDR10      (unsigned int)(2)
#define DOLBY_VISION_OUTPUT_MODE_SDR10      (unsigned int)(3)
#define DOLBY_VISION_OUTPUT_MODE_SDR8       (unsigned int)(4)
#define DOLBY_VISION_OUTPUT_MODE_BYPASS     (unsigned int)(5)

using namespace KODI;

void dv_type_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data) {
  
  // Only do first time - becasue if injecting a Dolby VSVDB then more options will show which are not valid
  // Means user will need to restart kodi if they change the Display / EDID to see more options.
  const bool dv_std = aml_support_dolby_vision() && aml_dv_support_std();
  const bool dv_ll = aml_support_dolby_vision() && aml_dv_support_ll();
  const bool dv_hdr_pq = aml_support_dolby_vision() && aml_display_support_hdr_pq();
  const bool dv = aml_support_dolby_vision();

  list.clear();
  if (dv_std) list.emplace_back(g_localizeStrings.Get(50023), DV_TYPE_DISPLAY_LED); 
  if (dv_ll) list.emplace_back(g_localizeStrings.Get(50024), DV_TYPE_PLAYER_LED_LLDV);
  if (dv_hdr_pq) list.emplace_back(g_localizeStrings.Get(50025), DV_TYPE_PLAYER_LED_HDR); 
  if (dv) list.emplace_back(g_localizeStrings.Get(50026), DV_TYPE_VS10_ONLY); 
}

bool support_dv() {
  return (aml_support_dolby_vision() && (aml_dv_support_std() || aml_dv_support_ll() || aml_display_support_hdr_pq()));
}

void add_vs10_bypass(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50063), DOLBY_VISION_OUTPUT_MODE_BYPASS);}
void add_vs10_sdr8(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50064), DOLBY_VISION_OUTPUT_MODE_SDR8);}
void add_vs10_sdr10(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50065), DOLBY_VISION_OUTPUT_MODE_SDR10);}
void add_vs10_hdr10(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50066), DOLBY_VISION_OUTPUT_MODE_HDR10);}
void add_vs10_dv(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50067), DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL);}

void vs10_sdr8_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  add_vs10_bypass(list);
  add_vs10_sdr10(list);
  if (aml_display_support_hdr_pq()) add_vs10_hdr10(list);
  if (support_dv()) add_vs10_dv(list); 
}

void vs10_sdr10_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  add_vs10_bypass(list);
  add_vs10_sdr8(list);
  if (aml_display_support_hdr_pq()) add_vs10_hdr10(list);
  if (support_dv()) add_vs10_dv(list); 
}

void vs10_hdr10_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  if (aml_display_support_hdr_pq()) add_vs10_bypass(list);
  add_vs10_sdr8(list);
  add_vs10_sdr10(list);
  if (support_dv()) add_vs10_dv(list); 
}

void vs10_hdr_hlg_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  if (aml_display_support_hdr_hlg()) add_vs10_bypass(list);
  add_vs10_sdr8(list);
  add_vs10_sdr10(list);
  if (aml_display_support_hdr_pq()) add_vs10_hdr10(list);
  if (support_dv()) add_vs10_dv(list); 
}

void vs10_dv_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  if (support_dv()) add_vs10_bypass(list);
  add_vs10_sdr8(list);
  add_vs10_sdr10(list);
}

CWinSystemAmlogic::CWinSystemAmlogic()
:  m_nativeWindow(NULL)
,  m_libinput(new CLibInputHandler)
,  m_force_mode_switch(false)
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    m_framebuffer_name = framebuffer.substr(start);
  }

  m_nativeDisplay = EGL_NO_DISPLAY;

  m_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_delayDispReset = false;

  m_libinput->Start();
}

bool CWinSystemAmlogic::InitWindowSystem()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const auto settingsManager = settings->GetSettingsManager();

  settingsManager->RegisterSettingOptionsFiller("DolbyVisionType", dv_type_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10SDR8", vs10_sdr8_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10SDR10", vs10_sdr10_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10HDR10", vs10_hdr10_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10HDR10Plus", vs10_hdr10_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10HDRHLG", vs10_hdr_hlg_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10DV", vs10_dv_filler);

  if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- disabling noise reduction");
     CSysfsPath("/sys/module/di/parameters/nr2_en", 0);
  }

  int sdr2hdr = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
  if (sdr2hdr)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- setting sdr2hdr mode to {:d}", sdr2hdr);
    CSysfsPath("/sys/module/am_vecm/parameters/sdr_mode", 1);
    CSysfsPath("/sys/module/amdolby_vision/parameters/dolby_vision_policy", 0);
    CSysfsPath("/sys/module/am_vecm/parameters/hdr_policy", 0);
  }

  int hdr2sdr = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
  if (hdr2sdr)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- setting hdr2sdr mode to {:d}", hdr2sdr);
    CSysfsPath("/sys/module/am_vecm/parameters/hdr_mode", 1);
  }

  auto setBln = [&](const std::string& id, const bool support, const bool value) {
    if (auto setting = settings->GetSetting(id)) setting->SetVisible(support);
    if (!support) settings->SetBool(id, value);
  };

  auto setInt = [&](const std::string& id, const bool support, const int value) {
    if (auto setting = settings->GetSetting(id)) setting->SetVisible(support);
    if (!support) settings->SetInt(id, value);
  };

  auto setStr = [&](const std::string& id, const bool support, const std::string& value) {
    if (auto setting = settings->GetSetting(id)) setting->SetVisible(support);
    if (!support) settings->SetString(id, value);
  };

  bool device_dv = aml_support_dolby_vision();
  bool device_and_display_std_dv = (aml_support_dolby_vision() && aml_display_support_dv());

  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE, device_dv, DV_MODE_OFF);
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE, device_dv, DV_TYPE_VS10_ONLY);
  setBln(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_INJECT, device_dv, false);
  setStr(CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB, device_dv, "");
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_COLORIMETRY_FOR_STD, device_and_display_std_dv, 0);
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR8, device_dv, static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS));
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR10, device_dv, static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS));
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10, device_dv, static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS));
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS, device_dv, static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS));
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDRHLG, device_dv, static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS));
  setInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_DV, device_dv, static_cast<int>(DOLBY_VISION_OUTPUT_MODE_BYPASS));

  // Always update (reset) the reg and lut on mode changes.
  CSysfsPath("/sys/module/amdolby_vision/parameters/force_update_reg", 31);

  // Turn on dv - if dv mode is on, limit the menu lumincance as menu now can be in DV/HDR. 
  aml_dv_start();

  if (((LINUX_VERSION_CODE >> 16) & 0xFF) < 5)
  {
    auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);
    if (setting)
    {
      setting->SetVisible(false);
      settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING, false);
    }
  }

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;

  CDVDVideoCodecAmlogic::Register();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CRendererAML::Register();
  CScreenshotSurfaceAML::Register();

  if (aml_get_cpufamily_id() <= AML_GXL)
    aml_set_framebuffer_resolution(1920, 1080, m_framebuffer_name);

  auto setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (setting)
  {
    setting->SetVisible(false);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK, false);
  }

  // Close the OpenVFD splash and switch the display into time mode.
  CSysfsPath("/tmp/openvfd_service", 0);

  // kill a running animation
  CLog::Log(LOGDEBUG,"CWinSystemAmlogic: Sending SIGUSR1 to 'splash-image'");
  std::system("killall -s SIGUSR1 splash-image &> /dev/null");

  // register for announcements to capture OnWake and re-apply DV if needed.
  auto announcer = CServiceBroker::GetAnnouncementManager();
  announcer->AddAnnouncer(this);

  return CWinSystemBase::InitWindowSystem();
}

void CWinSystemAmlogic::Announce(ANNOUNCEMENT::AnnouncementFlag flag,
              const std::string& sender,
              const std::string& message,
              const CVariant& data)
{
  // When Wake from Suspend re-trigger DV if in DV_MODE_ON
  if ((flag == ANNOUNCEMENT::System) && (message == "OnWake")) aml_dv_start();
}

bool CWinSystemAmlogic::DestroyWindowSystem()
{
  return true;
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_fRefreshRate  = res.fRefreshRate;

  if (m_nativeWindow == NULL)
    m_nativeWindow = new fbdev_window;

  m_nativeWindow->width = res.iWidth;
  m_nativeWindow->height = res.iHeight;

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(std::chrono::milliseconds(static_cast<unsigned int>(delay * 100)));
  }

  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
    }
  }

  aml_set_native_resolution(res, m_framebuffer_name, m_stereo_mode, m_force_mode_switch);
  // reset force mode switch
  m_force_mode_switch = false;

  if (!m_delayDispReset)
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnResetDisplay();
    }
  }

  // Make sure DV Display acitivates if enabled.
  aml_dv_display_trigger();

  m_bWindowCreated = true;
  return true;
}

bool CWinSystemAmlogic::DestroyWindow()
{
  if (m_nativeWindow != NULL)
  {
    delete(m_nativeWindow);
    m_nativeWindow = NULL;
  }

  m_bWindowCreated = false;
  return true;
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  RESOLUTION_INFO resDesktop, curDisplay;
  std::vector<RESOLUTION_INFO> resolutions;

  if (!aml_probe_resolutions(resolutions) || resolutions.empty())
  {
    CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.",__FUNCTION__);
  }

  /* ProbeResolutions includes already all resolutions.
   * Only get desktop resolution so we can replace xbmc's desktop res
   */
  if (aml_get_native_resolution(&curDisplay))
  {
    resDesktop = curDisplay;
  }

  RESOLUTION ResDesktop = RES_INVALID;
  RESOLUTION res_index  = RES_DESKTOP;

  for (size_t i = 0; i < resolutions.size(); i++)
  {
    // if this is a new setting,
    // create a new empty setting to fill in.
    if ((int)CDisplaySettings::GetInstance().ResolutionInfoSize() <= res_index)
    {
      RESOLUTION_INFO res;
      CDisplaySettings::GetInstance().AddResolutionInfo(res);
    }

    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(resolutions[i]);
    CDisplaySettings::GetInstance().GetResolutionInfo(res_index) = resolutions[i];

    CLog::Log(LOGINFO, "Found resolution {:d} x {:d} with {:d} x {:d}{} @ {:f} Hz",
      resolutions[i].iWidth,
      resolutions[i].iHeight,
      resolutions[i].iScreenWidth,
      resolutions[i].iScreenHeight,
      resolutions[i].dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      resolutions[i].fRefreshRate);

    if(resDesktop.iWidth == resolutions[i].iWidth &&
       resDesktop.iHeight == resolutions[i].iHeight &&
       resDesktop.iScreenWidth == resolutions[i].iScreenWidth &&
       resDesktop.iScreenHeight == resolutions[i].iScreenHeight &&
       (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK) == (resolutions[i].dwFlags & D3DPRESENTFLAG_MODEMASK) &&
       fabs(resDesktop.fRefreshRate - resolutions[i].fRefreshRate) < FLT_EPSILON)
    {
      ResDesktop = res_index;
    }

    res_index = (RESOLUTION)((int)res_index + 1);
  }

  // set RES_DESKTOP
  if (ResDesktop != RES_INVALID)
  {
    CLog::Log(LOGINFO, "Found ({:d}x{:d}{}@{:f}) at {:d}, setting to RES_DESKTOP at {:d}",
      resDesktop.iWidth, resDesktop.iHeight,
      resDesktop.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      resDesktop.fRefreshRate,
      (int)ResDesktop, (int)RES_DESKTOP);

    CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = CDisplaySettings::GetInstance().GetResolutionInfo(ResDesktop);
  }
}

bool CWinSystemAmlogic::IsHDRDisplay()
{
  CSysfsPath hdr_cap{"/sys/class/amhdmitx/amhdmitx0/hdr_cap"};
  CSysfsPath dv_cap{"/sys/class/amhdmitx/amhdmitx0/dv_cap"};
  std::string valstr;

  if (hdr_cap.Exists())
  {
    valstr = hdr_cap.Get<std::string>().value();
    if (valstr.find("Traditional HDR: 1") != std::string::npos)
      m_hdr_caps.SetHDR10();

    if (valstr.find("HDR10Plus Supported: 1") != std::string::npos)
      m_hdr_caps.SetHDR10Plus();

    if (valstr.find("Hybrid Log-Gamma: 1") != std::string::npos)
      m_hdr_caps.SetHLG();
  }

  if (dv_cap.Exists())
  {
    valstr = dv_cap.Get<std::string>().value();
    if (valstr.find("DolbyVision RX support list") != std::string::npos)
      m_hdr_caps.SetDolbyVision();
  }

  return (m_hdr_caps.SupportsHDR10() | m_hdr_caps.SupportsHDR10Plus() | m_hdr_caps.SupportsHLG());
}

CHDRCapabilities CWinSystemAmlogic::GetDisplayHDRCapabilities() const
{
  return m_hdr_caps;
}

float CWinSystemAmlogic::GetGuiSdrPeakLuminance() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int guiSdrPeak = settings->GetInt(CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE);

  return ((0.7f * guiSdrPeak + 30.0f) / 100.0f);
}

bool CWinSystemAmlogic::Hide()
{
  return false;
}

bool CWinSystemAmlogic::Show(bool show)
{
  CSysfsPath("/sys/class/graphics/" + m_framebuffer_name + "/blank", (show ? 0 : 1));
  return true;
}

void CWinSystemAmlogic::Register(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemAmlogic::Unregister(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
    m_resources.erase(i);
}
