/*
 *  Copyright (C) 2007-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "RendererAML.h"

#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/DVDCodecs/Video/AMLCodec.h"
#include "utils/log.h"
#include "utils/AMLUtils.h"
#include "utils/ScreenshotAML.h"
#include "settings/MediaSettings.h"
#include "cores/VideoPlayer/VideoRenderers/RenderCapture.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFactory.h"
#include "cores/VideoPlayer/VideoRenderers/RenderFlags.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "windowing/GraphicContext.h"
#include "windowing/WinSystem.h"

CRendererAML::CRendererAML()
 : m_prevVPts(-1)
 , m_bConfigured(false)
{
  CLog::Log(LOGINFO, "Constructing CRendererAML");
}

CRendererAML::~CRendererAML()
{
  Reset();
}

CBaseRenderer* CRendererAML::Create(CVideoBuffer *buffer)
{
  if (buffer && dynamic_cast<CAMLVideoBuffer*>(buffer))
    return new CRendererAML();
  return nullptr;
}

bool CRendererAML::Register()
{
  VIDEOPLAYER::CRendererFactory::RegisterRenderer("amlogic", CRendererAML::Create);
  return true;
}

bool CRendererAML::Configure(const VideoPicture &picture, float fps, unsigned int orientation)
{
  m_sourceWidth = picture.iWidth;
  m_sourceHeight = picture.iHeight;
  m_renderOrientation = orientation;

  m_iFlags = GetFlagsChromaPosition(picture.chroma_position) |
             GetFlagsColorMatrix(picture.color_space, picture.iWidth, picture.iHeight) |
             GetFlagsColorPrimaries(picture.color_primaries) |
             GetFlagsStereoMode(picture.stereoMode);

  // Calculate the input frame aspect ratio.
  CalculateFrameAspectRatio(picture.iDisplayWidth, picture.iDisplayHeight);
  SetViewMode(m_videoSettings.m_ViewMode);
  ManageRenderArea();

  auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  // Configure GUI/OSD for HDR PQ when display is in HDR PQ mode

  bool display_is_hdr(CServiceBroker::GetWinSystem()->IsHDRDisplay());
  bool device_dv_ready(aml_support_dolby_vision());
  bool user_dv_disable(settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_DV_MODE) == DV_MODE_OFF);
  bool device_is_dv(device_dv_ready && !user_dv_disable);

  // FIXME: picture.hdrType will not be correct for hdr10+ until upstream can identify correctly.

  bool hdr10_is_used((picture.hdrType == StreamHdrType::HDR_TYPE_HDR10) && display_is_hdr);
  bool hdr10plus_is_used((picture.hdrType == StreamHdrType::HDR_TYPE_HDR10PLUS) && display_is_hdr);
  bool hdrhlg_is_used((picture.hdrType == StreamHdrType::HDR_TYPE_HLG) && display_is_hdr);
  bool dv_is_used((picture.hdrType == StreamHdrType::HDR_TYPE_DOLBYVISION) && device_is_dv);

  CLog::Log(LOGINFO, "CRendererAML::Configure Stream Hdr Type {}", picture.hdrType);

  // If device is dv - then need to check VS10 mapping.
  if (device_is_dv) {

    if (hdr10_is_used) {
      unsigned int mode(aml_vs10_mode(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10));
      hdr10_is_used = (mode <= DOLBY_VISION_OUTPUT_MODE_HDR10);
    }

    if (hdr10plus_is_used) {
      unsigned int mode(aml_vs10_mode(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDR10PLUS));
      hdr10plus_is_used = (mode <= DOLBY_VISION_OUTPUT_MODE_HDR10);
    }

    if (hdrhlg_is_used) {
      unsigned int mode(aml_vs10_mode(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_HDRHLG));
      hdrhlg_is_used = (mode <= DOLBY_VISION_OUTPUT_MODE_HDR10);
    }

    if (dv_is_used) {
      unsigned int mode(aml_vs10_mode(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_DV));
      dv_is_used = (mode <= DOLBY_VISION_OUTPUT_MODE_HDR10);
    }
  }

  unsigned int vs10_sdr8_mode(aml_vs10_mode(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR8));
  bool vs10_sdr8_is_used((picture.hdrType == StreamHdrType::HDR_TYPE_NONE) && device_is_dv && (vs10_sdr8_mode <= DOLBY_VISION_OUTPUT_MODE_HDR10));

  unsigned int vs10_sdr10_mode(aml_vs10_mode(CSettings::SETTING_COREELEC_AMLOGIC_DV_VS10_SDR10));
  bool vs10_sdr10_is_used((picture.hdrType == StreamHdrType::HDR_TYPE_NONE) && device_is_dv && (vs10_sdr10_mode <= DOLBY_VISION_OUTPUT_MODE_HDR10));

  bool hdr_is_used = (hdr10_is_used || hdr10plus_is_used || hdrhlg_is_used);

  CLog::Log(LOGDEBUG, "CRendererAML::Configure {}DV support, {}, DV system is {}, HDR is {}", device_dv_ready ? "" : "no ",
    user_dv_disable ? "disabled" : "enabled", dv_is_used ? "enabled" : "disabled", hdr_is_used ? "used" : "not used");

  CServiceBroker::GetWinSystem()->GetGfxContext().SetTransferPQ(hdr_is_used || dv_is_used || vs10_sdr8_is_used || vs10_sdr10_is_used) ;

  m_bConfigured = true;

  return true;
}

CRenderInfo CRendererAML::GetRenderInfo()
{
  CRenderInfo info;
  info.max_buffer_size = m_numRenderBuffers;
  info.opaque_pointer = (void *)this;
  return info;
}

bool CRendererAML::RenderCapture(int index, CRenderCapture* capture)
{
  capture->BeginRender();
  capture->EndRender();
  CScreenshotAML::CaptureVideoFrame((unsigned char *)capture->GetRenderBuffer(), capture->GetWidth(), capture->GetHeight());
  return true;
}

void CRendererAML::AddVideoPicture(const VideoPicture &picture, int index)
{
  ReleaseBuffer(index);

  BUFFER &buf(m_buffers[index]);
  if (picture.videoBuffer)
  {
    buf.videoBuffer = picture.videoBuffer;
    buf.videoBuffer->Acquire();
  }
}

void CRendererAML::ReleaseBuffer(int idx)
{
  BUFFER &buf(m_buffers[idx]);
  if (buf.videoBuffer)
  {
    CAMLVideoBuffer *amli(dynamic_cast<CAMLVideoBuffer*>(buf.videoBuffer));
    if (amli)
    {
      if (amli->m_amlCodec)
      {
        amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex, true);
        amli->m_amlCodec = nullptr; // Released
      }
      amli->Release();
    }
    buf.videoBuffer = nullptr;
  }
}

bool CRendererAML::Supports(ERENDERFEATURE feature) const
{
  if (feature == RENDERFEATURE_ZOOM ||
      feature == RENDERFEATURE_CONTRAST ||
      feature == RENDERFEATURE_BRIGHTNESS ||
      feature == RENDERFEATURE_NONLINSTRETCH ||
      feature == RENDERFEATURE_VERTICAL_SHIFT ||
      feature == RENDERFEATURE_STRETCH ||
      feature == RENDERFEATURE_PIXEL_RATIO ||
      feature == RENDERFEATURE_ROTATION)
    return true;

  return false;
}

void CRendererAML::Reset()
{
  std::array<int, 2> reset_arr[m_numRenderBuffers];
  m_prevVPts = -1;

  for (int i = 0 ; i < m_numRenderBuffers ; ++i)
  {
    reset_arr[i][0] = i;

    if (m_buffers[i].videoBuffer)
      reset_arr[i][1] = dynamic_cast<CAMLVideoBuffer *>(m_buffers[i].videoBuffer)->m_bufferIndex;
    else
      reset_arr[i][1] = 0;
  }

  std::sort(std::begin(reset_arr), std::end(reset_arr),
    [](const std::array<int, 2>& u, const std::array<int, 2>& v)
    {
      return u[1] < v[1];
    });

  for (int i = 0; i < m_numRenderBuffers; ++i)
  {
    if (m_buffers[reset_arr[i][0]].videoBuffer)
    {
      m_buffers[reset_arr[i][0]].videoBuffer->Release();
      m_buffers[reset_arr[i][0]].videoBuffer = nullptr;
    }
  }

  CServiceBroker::GetWinSystem()->GetGfxContext().SetTransferPQ(false);
}

bool CRendererAML::Flush(bool saveBuffers)
{
  Reset();
  return saveBuffers;
};

void CRendererAML::RenderUpdate(int index, int index2, bool clear, unsigned int flags, unsigned int alpha)
{
  ManageRenderArea();

  CAMLVideoBuffer *amli = dynamic_cast<CAMLVideoBuffer *>(m_buffers[index].videoBuffer);
  if(amli && amli->m_amlCodec)
  {
    int pts = amli->m_omxPts;
    if (pts != m_prevVPts)
    {
      amli->m_amlCodec->ReleaseFrame(amli->m_bufferIndex);
      amli->m_amlCodec->SetVideoRect(m_sourceRect, m_destRect);
      amli->m_amlCodec = nullptr; //Mark frame as processed
      m_prevVPts = pts;
    }
  }
  CAMLCodec::PollFrame();
}
