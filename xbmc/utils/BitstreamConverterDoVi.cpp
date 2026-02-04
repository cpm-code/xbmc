/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BitstreamConverter.h"

#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "utils/StringUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

extern "C"
{
#ifdef HAVE_LIBDOVI
#include <libdovi/rpu_parser.h>
#endif
}

enum
{
  HEVC_NAL_UNSPEC62 = 62, // Dolby Vision RPU
  HEVC_NAL_UNSPEC63 = 63 // Dolby Vision EL
};

namespace
{
bool IsValidPtsForInjection(double pts)
{
  return std::isfinite(pts) && pts >= 0.0;
}

constexpr char PTS_MARKER[] = "PTS_US64=";

bool AppendPtsToDoviRpuNalu(std::vector<uint8_t>& nalu, uint64_t ptsUs64)
{
  // Expect the final rbsp_trailing_bits byte.
  if (nalu.size() < 1 || nalu.back() != 0x80)
    return false;

  std::vector<uint8_t> trailer;
  trailer.reserve(sizeof(PTS_MARKER) - 1 + 16 + 1);
  trailer.insert(trailer.end(), reinterpret_cast<const uint8_t*>(PTS_MARKER),
                 reinterpret_cast<const uint8_t*>(PTS_MARKER) + (sizeof(PTS_MARKER) - 1));
  const std::string ptsHex = fmt::format("{:016X}", ptsUs64);
  trailer.insert(trailer.end(), ptsHex.begin(), ptsHex.end());
  trailer.push_back(static_cast<uint8_t>(';'));

  // Insert trailer right before the final 0x80 byte.
  nalu.insert(nalu.end() - 1, trailer.begin(), trailer.end());
  return true;
}

#ifdef HAVE_LIBDOVI

// The returned data must be freed with `dovi_data_free`
// May be nullptr if no conversion was done.
const DoviData* ConvertDoviRpuNal(uint8_t* nalBuf,
                                 uint32_t nalSize,
                                 int mode,
                                 bool firstFrame,
                                 DOVIELType& doviElType)
{
  DoviRpuOpaque* rpuOpaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
  const DoviRpuDataHeader* header = dovi_rpu_get_header(rpuOpaque);
  const DoviData* rpuData = nullptr;

  if (header && header->guessed_profile == 7)
  {
    if (firstFrame)
    {
      doviElType = DOVIELType::TYPE_NONE;
      if (header->el_type)
      {
        if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
          doviElType = DOVIELType::TYPE_FEL;
        else if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
          doviElType = DOVIELType::TYPE_MEL;
      }
    }

    if (dovi_convert_rpu_with_mode(rpuOpaque, mode) >= 0)
      rpuData = dovi_write_unspec62_nalu(rpuOpaque);
  }

  dovi_rpu_free_header(header);
  dovi_rpu_free(rpuOpaque);

  return rpuData;
}

void GetDoviRpuInfo(uint8_t* nalBuf,
                    uint32_t nalSize,
                    bool firstFrame,
                    DOVIELType& doviElType,
                    AVDOVIDecoderConfigurationRecord& dovi,
                    double pts,
                    CDataCacheCore& dataCacheCore)
{
  // https://professionalsupport.dolby.com/s/article/Dolby-Vision-Metadata-Levels?language=en_US

  DoviRpuOpaque* rpuOpaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);

  const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(rpuOpaque);

  if (vdrDmData)
  {
    DOVIFrameMetadata doviFrameMetadata;

    if (vdrDmData->dm_data.level1)
    {
      doviFrameMetadata.level1_min_pq = vdrDmData->dm_data.level1->min_pq;
      doviFrameMetadata.level1_max_pq = vdrDmData->dm_data.level1->max_pq;
      doviFrameMetadata.level1_avg_pq = vdrDmData->dm_data.level1->avg_pq;
      doviFrameMetadata.pts = pts;
    }

    if (vdrDmData->dm_data.level5)
    {
      doviFrameMetadata.has_level5_metadata = true;
      doviFrameMetadata.level5_active_area_left_offset =
          vdrDmData->dm_data.level5->active_area_left_offset;
      doviFrameMetadata.level5_active_area_right_offset =
          vdrDmData->dm_data.level5->active_area_right_offset;
      doviFrameMetadata.level5_active_area_top_offset =
          vdrDmData->dm_data.level5->active_area_top_offset;
      doviFrameMetadata.level5_active_area_bottom_offset =
          vdrDmData->dm_data.level5->active_area_bottom_offset;
    }

    dataCacheCore.SetVideoDoViFrameMetadata(doviFrameMetadata);
  }

  if (firstFrame)
  {
    DOVIStreamMetadata doviStreamMetadata;

    if (vdrDmData)
    {
      doviStreamMetadata.source_min_pq = vdrDmData->source_min_pq;
      doviStreamMetadata.source_max_pq = vdrDmData->source_max_pq;
    }

    if (vdrDmData && vdrDmData->dm_data.level6)
    {
      doviStreamMetadata.has_level6_metadata = true;

      doviStreamMetadata.level6_max_lum =
          vdrDmData->dm_data.level6->max_display_mastering_luminance;
      doviStreamMetadata.level6_min_lum =
          vdrDmData->dm_data.level6->min_display_mastering_luminance;

      doviStreamMetadata.level6_max_cll = vdrDmData->dm_data.level6->max_content_light_level;
      doviStreamMetadata.level6_max_fall =
          vdrDmData->dm_data.level6->max_frame_average_light_level;
    }

    std::string metaVersion;
    if (vdrDmData && vdrDmData->dm_data.level254)
    {
      const unsigned int noL8 = vdrDmData->dm_data.level8.len;
      if (noL8 > 0)
        metaVersion = fmt::format("CMv4.0 {}-{} {}-L8", vdrDmData->dm_data.level254->dm_version_index,
                                 vdrDmData->dm_data.level254->dm_mode, noL8);
      else
        metaVersion = fmt::format("CMv4.0 {}-{}", vdrDmData->dm_data.level254->dm_version_index,
                                 vdrDmData->dm_data.level254->dm_mode);
    }
    else if (vdrDmData && vdrDmData->dm_data.level1)
    {
      const unsigned int noL2 = vdrDmData->dm_data.level2.len;
      if (noL2 > 0)
        metaVersion = fmt::format("CMv2.9 {}-L2", noL2);
      else
        metaVersion = "CMv2.9";
    }

    doviStreamMetadata.meta_version = metaVersion;
    dataCacheCore.SetVideoDoViStreamMetadata(doviStreamMetadata);

    DOVIStreamInfo doviStreamInfo;
    const DoviRpuDataHeader* header = dovi_rpu_get_header(rpuOpaque);
    doviElType = DOVIELType::TYPE_NONE;

    if (header && ((header->guessed_profile == 4) || (header->guessed_profile == 7)) && header->el_type)
    {
      if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
        doviElType = DOVIELType::TYPE_FEL;
      else if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
        doviElType = DOVIELType::TYPE_MEL;
    }

    doviStreamInfo.dovi_el_type = doviElType;
    doviStreamInfo.dovi = dovi;

    doviStreamInfo.has_config =
        (memcmp(&dovi, &CDVDStreamInfo::empty_dovi, sizeof(AVDOVIDecoderConfigurationRecord)) != 0);
    doviStreamInfo.has_header = (header != nullptr);

    dataCacheCore.SetVideoDoViStreamInfo(doviStreamInfo);
    dovi_rpu_free_header(header);
  }

  dovi_rpu_free_vdr_dm_data(vdrDmData);
  dovi_rpu_free(rpuOpaque);
}

#endif
} // namespace

void CBitstreamConverter::ProcessDoViRpuWrap(
  uint8_t* nalBuf,
  int32_t nalSize,
  uint8_t** poutbuf,
  uint32_t& poutbufSize,
  double pts) const
{
  int intPoutbufSize = poutbufSize;
  ProcessDoViRpu(nalBuf, nalSize, poutbuf, &intPoutbufSize, pts);
  poutbufSize = static_cast<uint32_t>(intPoutbufSize);
}

void CBitstreamConverter::ProcessDoViRpu(
  uint8_t* nalBuf,
  int32_t nalSize,
  uint8_t** poutbuf,
  int* poutbufSize,
  double pts) const
{
#ifdef HAVE_LIBDOVI

  const DoviData* rpuData = nullptr;

  if (m_convert_dovi != DOVIMode::MODE_NONE)
  {
    DOVIELType doviElType = DOVIELType::TYPE_NONE;
    rpuData = ConvertDoviRpuNal(nalBuf, nalSize, m_convert_dovi, m_first_frame, doviElType);
    if (rpuData)
    {
      nalBuf = const_cast<uint8_t*>(rpuData->data);
      nalSize = rpuData->len;

      // Capture the DOVI source details - about to be replaced.
      if (m_first_frame)
      {
        DOVIStreamInfo doviStreamInfo;
        doviStreamInfo.dovi_el_type = doviElType;
        doviStreamInfo.dovi = m_hints.dovi;
        m_dataCacheCore.SetVideoSourceDoViStreamInfo(doviStreamInfo);
      }

      m_hints.dovi.el_present_flag = 0; // EL removed in both conversion cases - to MEL and to P8.1
      if (m_convert_dovi == DOVIMode::MODE_TO81)
      {
        m_hints.dovi.dv_profile = 8;
        m_hints.dovi.dv_bl_signal_compatibility_id = 1;
      }
    }
  }

  GetDoviRpuInfo(nalBuf, nalSize, m_first_frame, m_hints.dovi_el_type, m_hints.dovi, pts, m_dataCacheCore);

  std::vector<uint8_t> nalu;

  // Inject the pts into the DoVi RPU NALU for FEL (STREAM_TYPE_STREAM)
  // so it can be obtained in sync with frame by the kernel driver.
  if ((m_convert_dovi == DOVIMode::MODE_NONE) && (m_hints.dovi_el_type == DOVIELType::TYPE_FEL) &&
      IsValidPtsForInjection(pts) && nalBuf && (nalSize > 0))
  {
    nalu.assign(nalBuf, nalBuf + nalSize);
    if (AppendPtsToDoviRpuNalu(nalu, static_cast<uint64_t>(pts)))
    {
      nalBuf = nalu.data();
      nalSize = static_cast<int32_t>(nalu.size());
    }
  }
#endif

  // HEVC NAL unit type 62 is Dolby Vision RPU (UNSPEC62)
  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalBuf, nalSize, 62);

#ifdef HAVE_LIBDOVI
  if (rpuData)
    dovi_data_free(rpuData);
#endif
}

void CBitstreamConverter::AddDoViRpuNaluWrap(const Hdr10PlusMetadata& meta,
                                            uint8_t** poutbuf,
                                            uint32_t& poutbufSize,
                                            double pts)
{
  int intPoutbufSize = poutbufSize;
  AddDoViRpuNalu(meta, poutbuf, &intPoutbufSize, pts);
  poutbufSize = static_cast<uint32_t>(intPoutbufSize);
}

void CBitstreamConverter::AddDoViRpuNalu(const Hdr10PlusMetadata& meta,
                                        uint8_t** poutbuf,
                                        int* poutbufSize,
                                        double pts) const
{
  auto nalu = create_rpu_nalu_for_hdr10plus(meta, m_convert_Hdr10Plus_peak_brightness_source,
                                           m_hdrStaticMetadataInfo);

  if (nalu.empty())
    return;

  if (m_first_frame)
  {
    m_hints.hdrType = StreamHdrType::HDR_TYPE_DOLBYVISION;
    m_hints.dovi.dv_version_major = 1;
    m_hints.dovi.dv_version_minor = 0;
    m_hints.dovi.dv_profile = 8;
    m_hints.dovi.dv_level = 6;
    m_hints.dovi.rpu_present_flag = 1;
    m_hints.dovi.el_present_flag = 0;
    m_hints.dovi.bl_present_flag = 1;
    m_hints.dovi.dv_bl_signal_compatibility_id = 1;
  }

#ifdef HAVE_LIBDOVI
  GetDoviRpuInfo(nalu.data(), static_cast<uint32_t>(nalu.size()), m_first_frame, m_hints.dovi_el_type,
                m_hints.dovi, pts, m_dataCacheCore);
#endif

  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalu.data(),
                        static_cast<uint32_t>(nalu.size()), HEVC_NAL_UNSPEC62);
}
