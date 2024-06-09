/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DolbyVisionAML.h"

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/SettingsManager.h"
#include "settings/lib/Setting.h"
#include "guilib/LocalizeStrings.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "interfaces/AnnouncementManager.h"
 
#include "platform/linux/SysfsPath.h"

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

CDolbyVisionAML::CDolbyVisionAML()
{
}

void CDolbyVisionAML::Setup()
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

  // Register for ui dv mode change - to change on the fly.
  std::set<std::string> settingSet;
  settingSet.insert(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE);
  settingSet.insert(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE_ON_LUMINANCE);
  settingsManager->RegisterCallback(this, settingSet);

  // register for announcements to capture OnWake and re-apply DV if needed.
  auto announcer = CServiceBroker::GetAnnouncementManager();
  announcer->AddAnnouncer(this);

  // Turn on dv - if dv mode is on, limit the menu lumincance as menu now can be in DV/HDR. 
  aml_dv_start();
}

void CDolbyVisionAML::OnSettingChanged(const std::shared_ptr<const CSetting>& setting) 
{
  if (!setting) return;

  const std::string& settingId = setting->GetId();

  if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE) {
    enum DV_MODE dv_mode(static_cast<DV_MODE>(std::dynamic_pointer_cast<const CSettingInt>(setting)->GetValue()));
    if (dv_mode == DV_MODE_ON) {
      aml_dv_on(DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL, true);
    } else if (aml_is_dv_enable()) {
      aml_dv_off(true);
    }
  } else if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE_ON_LUMINANCE) {
    int max(std::dynamic_pointer_cast<const CSettingInt>(setting)->GetValue());
    aml_dv_set_osd_max(max);
  }
}

void CDolbyVisionAML::Announce(ANNOUNCEMENT::AnnouncementFlag flag,
              const std::string& sender,
              const std::string& message,
              const CVariant& data)
{
  // When Wake from Suspend re-trigger DV if in DV_MODE_ON
  if ((flag == ANNOUNCEMENT::System) && (message == "OnWake")) aml_dv_start();
}