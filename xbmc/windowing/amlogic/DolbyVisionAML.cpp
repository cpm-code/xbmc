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

using namespace KODI;

void dv_type_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data) {
  
  // Only do first time - becasue if injecting a Dolby VSVDB then more options will show which are not valid
  // Means user will need to restart kodi if they change the Display / EDID to see more options.
  const bool dv_std = aml_dv_support_std();
  const bool dv_ll = aml_dv_support_ll();
  const bool dv_hdr_pq = aml_display_support_hdr_pq();

  list.clear();
  if (dv_std) list.emplace_back(g_localizeStrings.Get(50023), DV_TYPE_DISPLAY_LED); 
  if (dv_ll) list.emplace_back(g_localizeStrings.Get(50024), DV_TYPE_PLAYER_LED_LLDV);
  if (dv_hdr_pq) list.emplace_back(g_localizeStrings.Get(50025), DV_TYPE_PLAYER_LED_HDR); 
  list.emplace_back(g_localizeStrings.Get(50026), DV_TYPE_VS10_ONLY); 
}

bool display_support_dv() {
  return (aml_dv_support_std() || aml_dv_support_ll() || aml_display_support_hdr_pq());
}

void add_vs10_bypass(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50063), DOLBY_VISION_OUTPUT_MODE_BYPASS);}
void add_vs10_dv_bypass(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50063), DOLBY_VISION_OUTPUT_MODE_IPT);}
void add_vs10_sdr(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50064), DOLBY_VISION_OUTPUT_MODE_SDR10);}
void add_vs10_hdr10(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50065), DOLBY_VISION_OUTPUT_MODE_HDR10);}
void add_vs10_dv(std::vector<IntegerSettingOption>& list) {list.emplace_back(g_localizeStrings.Get(50066), DOLBY_VISION_OUTPUT_MODE_IPT);}

void vs10_sdr_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  add_vs10_bypass(list);
  add_vs10_sdr(list);
  if (aml_display_support_hdr_pq()) add_vs10_hdr10(list);
  if (display_support_dv()) add_vs10_dv(list); 
}

void vs10_hdr10_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  if (aml_display_support_hdr_pq()) add_vs10_bypass(list);
  add_vs10_sdr(list);
  if (display_support_dv()) add_vs10_dv(list); 
}

void vs10_hdr_hlg_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  if (aml_display_support_hdr_hlg()) add_vs10_bypass(list);
  add_vs10_sdr(list);
  if (aml_display_support_hdr_pq()) add_vs10_hdr10(list);
  if (display_support_dv()) add_vs10_dv(list); 
}

void vs10_dv_filler(const SettingConstPtr& setting, std::vector<IntegerSettingOption>& list, int& current, void* data)
{
  list.clear();
  if (display_support_dv()) add_vs10_dv_bypass(list);
  add_vs10_sdr(list);
}

void set_visible(const std::shared_ptr<CSettings> settings, const std::string& id, bool visible) {
  if (auto setting = settings->GetSetting(id)) setting->SetVisible(visible);
}

CDolbyVisionAML::CDolbyVisionAML()
{
}

bool CDolbyVisionAML::Setup()
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  CLog::Log(LOGDEBUG, "CDolbyVisionAML::Setup - Begin");

  if (!aml_support_dolby_vision())
  {
    set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE, false);
    settings->SetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE, DV_MODE_OFF);
    CLog::Log(LOGDEBUG, "CDolbyVisionAML::Setup - Device does not support Dolby Vision - exiting setup");
    return false;
  }

  const auto settingsManager = settings->GetSettingsManager();

  settingsManager->RegisterSettingOptionsFiller("DolbyVisionType", dv_type_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10SDR8", vs10_sdr_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10SDR10", vs10_sdr_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10HDR10", vs10_hdr10_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10HDR10Plus", vs10_hdr10_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10HDRHLG", vs10_hdr_hlg_filler);
  settingsManager->RegisterSettingOptionsFiller("DolbyVisionVS10DV", vs10_dv_filler);

  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE_ON_LUMINANCE, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB_INJECT, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VSVDB, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_COLORIMETRY_FOR_STD, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR8, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR10, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDRHLG, true);
  set_visible(settings, CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_DV, true);

  // Register for ui dv mode change - to change on the fly.
  std::set<std::string> settingSet;
  settingSet.insert(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE);
  settingSet.insert(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE_ON_LUMINANCE);
  settingSet.insert(CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE);
  settingsManager->RegisterCallback(this, settingSet);

  // register for announcements to capture OnWake and re-apply DV if needed.
  auto announcer = CServiceBroker::GetAnnouncementManager();
  announcer->AddAnnouncer(this);

  // Turn on dv - if dv mode is on, limit the menu lumincance as menu now can be in DV/HDR. 
  aml_dv_start();

  CLog::Log(LOGDEBUG, "CDolbyVisionAML::Setup - Complete");

  return true;
}

void CDolbyVisionAML::OnSettingChanged(const std::shared_ptr<const CSetting>& setting) 
{
  if (!setting) return;

  const std::string& settingId = setting->GetId();
  if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE) 
  {
    // Not working for some cases - needs video playback for mode switch to work correctly everytime.
    // enum DV_MODE dv_mode(static_cast<DV_MODE>(std::dynamic_pointer_cast<const CSettingInt>(setting)->GetValue()));
    // if (dv_mode == DV_MODE_ON) ? aml_dv_on(DOLBY_VISION_OUTPUT_MODE_IPT) : aml_dv_off();
  } 
  else if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE_ON_LUMINANCE) 
  {
    int max(std::dynamic_pointer_cast<const CSettingInt>(setting)->GetValue());
    aml_dv_set_osd_max(max);
  }
  else if (settingId == CSettings::SETTING_COREELEC_AMLOGIC_DV_TYPE)
  {
    // Not working for some cases - needs video playback for mode switch to work correctly everytime.
    // aml_dv_start();
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
