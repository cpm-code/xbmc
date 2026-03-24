/*
 *  Copyright (C) 2013-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"

#include <memory>
#include <vector>

class CAdvancedSettings;
class CVariant;
class CSettingList;
class CSetting;
class CSettings;

class CSettingUtils
{
public:
  /*!
   \brief Gets the values of the given list setting.

   \param settingList List setting
   \return List of values of the given list setting
   */
  static std::vector<CVariant> GetList(const std::shared_ptr<const CSettingList>& settingList);
  /*!
   \brief Sets the values of the given list setting.

   \param settingList List setting
   \param value Values to set
   \return True if setting the values was successful, false otherwise
   */
  static bool SetList(const std::shared_ptr<CSettingList>& settingList,
                      const std::vector<CVariant>& value);

  static std::vector<CVariant> ListToValues(const std::shared_ptr<const CSettingList>& setting,
                                            const std::vector<std::shared_ptr<CSetting>>& values);
  static bool ValuesToList(const std::shared_ptr<const CSettingList>& setting,
                           const std::vector<CVariant>& values,
                           std::vector<std::shared_ptr<CSetting>>& newValues);

  /*!
   \brief Search in a list of Ints for a given value.

   \param settingList CSettingList instance
   \param value value to search for
   \return True if value was found in list, false otherwise
  */
  static bool FindIntInList(const std::shared_ptr<const CSettingList>& settingList, int value);

  template<typename T, typename MemberT>
  static T GetAdvancedSettingValue(MemberT CAdvancedSettings::*member, T defaultValue)
  {
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    if (!settingsComponent)
      return defaultValue;

    const auto advancedSettings = settingsComponent->GetAdvancedSettings();
    if (!advancedSettings)
      return defaultValue;

    return static_cast<T>(advancedSettings.get()->*member);
  }

  template<typename T, typename Getter>
  static T GetAdvancedSettingValue(Getter&& getter, T defaultValue)
  {
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    if (!settingsComponent)
      return defaultValue;

    const auto advancedSettings = settingsComponent->GetAdvancedSettings();
    if (!advancedSettings)
      return defaultValue;

    return getter(*advancedSettings);
  }

  template<typename T, typename MemberT>
  static T GetSettingValue(MemberT CSettings::*member, T defaultValue)
  {
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    if (!settingsComponent)
      return defaultValue;

    const auto settings = settingsComponent->GetSettings();
    if (!settings)
      return defaultValue;

    return static_cast<T>(settings.get()->*member);
  }

  template<typename T, typename Getter>
  static T GetSettingValue(Getter&& getter, T defaultValue)
  {
    const auto settingsComponent = CServiceBroker::GetSettingsComponent();
    if (!settingsComponent)
      return defaultValue;

    const auto settings = settingsComponent->GetSettings();
    if (!settings)
      return defaultValue;

    return getter(*settings);
  }
};
