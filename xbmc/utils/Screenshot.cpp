/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "Screenshot.h"

#include "ServiceBroker.h"
#include "URL.h"
#include "Util.h"
#include "filesystem/File.h"
#include "jobs/JobManager.h"
#include "pictures/Picture.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "settings/SettingPath.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/windows/GUIControlSettings.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

using namespace XFILE;

std::vector<std::function<std::unique_ptr<IScreenshotSurface>()>> CScreenShot::m_screenShotSurfaces;

void CScreenShot::Register(const std::function<std::unique_ptr<IScreenshotSurface>()>& createFunc)
{
  m_screenShotSurfaces.emplace_back(createFunc);
}

void CScreenShot::TakeScreenshot(const std::string& filename, bool sync)
{
  auto surface = m_screenShotSurfaces.back()();

  if (!surface)
  {
    CLog::Log(LOGERROR, "failed to create screenshot surface");
    return;
  }

  if (!surface->Capture())
  {
    CLog::Log(LOGERROR, "Screenshot {} failed", CURL::GetRedacted(filename));
    return;
  }

  CServiceBroker::GetJobManager()->AddJob(new CCaptureVideo(surface, filename, sync), nullptr);
}

void CScreenShot::TakeScreenshot()
{
  std::shared_ptr<CSettingPath> screenshotSetting = std::static_pointer_cast<CSettingPath>(CServiceBroker::GetSettingsComponent()->GetSettings()->GetSetting(CSettings::SETTING_DEBUG_SCREENSHOTPATH));
  if (!screenshotSetting)
    return;

  std::string strDir = screenshotSetting->GetValue();
  if (strDir.empty())
  {
    if (!CGUIControlButtonSetting::GetPath(
            screenshotSetting, &CServiceBroker::GetResourcesComponent().GetLocalizeStrings()))
      return;

    strDir = screenshotSetting->GetValue();
  }

  URIUtils::RemoveSlashAtEnd(strDir);

  if (!strDir.empty())
  {
    std::string file =
        CUtil::GetNextFilename(URIUtils::AddFileToFolder(strDir, "screenshot{:05}.png"), 65535);

    if (!file.empty())
    {
      TakeScreenshot(file, false);
    }
    else
    {
      CLog::Log(LOGWARNING, "Too many screen shots or invalid folder");
    }
  }
}

CCaptureVideo::CCaptureVideo(std::unique_ptr<IScreenshotSurface> &surface, const std::string filename, bool sync)
 : m_surface(std::move(surface)), m_filename(filename), m_sync(sync)
{
}

bool CCaptureVideo::DoWork()
{
  m_surface->CaptureVideo(true);

  CLog::Log(LOGDEBUG, "Saving screenshot {}", CURL::GetRedacted(m_filename));

  //set alpha byte to 0xFF
  for (int y = 0; y < m_surface->GetHeight(); y++)
  {
    unsigned char* alphaptr = m_surface->GetBuffer() - 1 + y * m_surface->GetStride();
    for (int x = 0; x < m_surface->GetWidth(); x++)
      *(alphaptr += 4) = 0xFF;
  }

  //if sync is true, the png file needs to be completely written when this function returns
  if (m_sync)
  {
    if (!CPicture::CreateThumbnailFromSurface(m_surface->GetBuffer(), m_surface->GetWidth(), m_surface->GetHeight(), m_surface->GetStride(), m_filename))
      CLog::Log(LOGERROR, "Unable to write screenshot {}", CURL::GetRedacted(m_filename));

    m_surface->ReleaseBuffer();
  }
  else
  {
    //make sure the file exists to avoid concurrency issues
    XFILE::CFile file;
    if (file.OpenForWrite(m_filename))
      file.Close();
    else
      CLog::Log(LOGERROR, "Unable to create file {}", CURL::GetRedacted(m_filename));

    //write .png file asynchronous with CThumbnailWriter, prevents stalling of the render thread
    //buffer is deleted from CThumbnailWriter
    CThumbnailWriter* thumbnailwriter = new CThumbnailWriter(m_surface->GetBuffer(), m_surface->GetWidth(), m_surface->GetHeight(), m_surface->GetStride(), m_filename);
    CServiceBroker::GetJobManager()->AddJob(thumbnailwriter, nullptr);
  }

  return true;
}
