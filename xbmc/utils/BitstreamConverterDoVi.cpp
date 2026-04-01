/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "BitstreamConverter.h"
#include "BitstreamIoReader.h"
#include "BitstreamIoWriter.h"
#include "Crc32.h"

#include "cores/DataCacheCore.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

extern "C"
{
#include <libdovi/rpu_parser.h>
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

bool CachedRpuInputMatches(const std::vector<uint8_t>& cachedNalu,
                           const uint8_t* nalBuf,
                           int32_t nalSize)
{
  if (!nalBuf || nalSize <= 0) return false;

  const size_t size = static_cast<size_t>(nalSize);

  if (cachedNalu.size() != size) return false;

  // DoVi RPU RBSPs end with a CRC32 followed by rbsp_trailing_bits (0x80).
  // On the encoded NAL bytes, start-code emulation prevention can insert up to
  // two 0x03 bytes inside that 5-byte tail, so compare the last 7 bytes first
  // for a cheap early reject, then fall back to a full compare on a match.
  constexpr size_t crcAndTrailingSize = 7;
  if (size <= crcAndTrailingSize)
    return std::equal(cachedNalu.begin(), cachedNalu.end(), nalBuf);

  const auto cachedSuffixBegin = cachedNalu.end() - crcAndTrailingSize;
  if (!std::equal(cachedSuffixBegin, cachedNalu.end(), nalBuf + (size - crcAndTrailingSize)))
    return false;

  return std::equal(cachedNalu.begin(), cachedSuffixBegin, nalBuf);
}

std::string BuildMetaVersionString(const DoviVdrDmData* vdrDmData)
{
  if (vdrDmData && vdrDmData->dm_data.level254)
  {
    const unsigned int level8Count = vdrDmData->dm_data.level8.len;
    if (level8Count > 0)
      return fmt::format("CMv4.0 {}-{} {}-L8", vdrDmData->dm_data.level254->dm_version_index,
                         vdrDmData->dm_data.level254->dm_mode, level8Count);

    return fmt::format("CMv4.0 {}-{}", vdrDmData->dm_data.level254->dm_version_index,
                       vdrDmData->dm_data.level254->dm_mode);
  }

  if (vdrDmData && vdrDmData->dm_data.level1)
  {
    const unsigned int level2Count = vdrDmData->dm_data.level2.len;
    if (level2Count > 0)
      return fmt::format("CMv2.9 {}-L2", level2Count);

    return "CMv2.9";
  }

  return "";
}

inline void PopulateDoviFrameMetadata(const DoviVdrDmData* vdrDmData,
                                      const DoviVdrDmData* sourceVdrDmData,
                                      double pts,
                                      CDataCacheCore& dataCacheCore,
                                      DOVIFrameMetadata* outDoViFrameMetadata)
{
  DOVIFrameMetadata doviFrameMetadata;
  doviFrameMetadata.pts = pts;
  doviFrameMetadata.meta_version = BuildMetaVersionString(vdrDmData);
  doviFrameMetadata.source_meta_version = BuildMetaVersionString(sourceVdrDmData);

  if (vdrDmData == nullptr)
  {
    dataCacheCore.SetVideoDoViFrameMetadata(doviFrameMetadata);
    if (outDoViFrameMetadata)
      *outDoViFrameMetadata = doviFrameMetadata;
    return;
  }

  if (vdrDmData->dm_data.level1)
  {
    doviFrameMetadata.level1_min_pq = vdrDmData->dm_data.level1->min_pq;
    doviFrameMetadata.level1_max_pq = vdrDmData->dm_data.level1->max_pq;
    doviFrameMetadata.level1_avg_pq = vdrDmData->dm_data.level1->avg_pq;
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
  if (outDoViFrameMetadata)
    *outDoViFrameMetadata = doviFrameMetadata;
}

inline void PopulateDoviStreamMetadata(const DoviVdrDmData* vdrDmData,
                                       const DoviVdrDmData* sourceVdrDmData,
                                       CDataCacheCore& dataCacheCore)
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
    doviStreamMetadata.level6_max_lum = vdrDmData->dm_data.level6->max_display_mastering_luminance;
    doviStreamMetadata.level6_min_lum = vdrDmData->dm_data.level6->min_display_mastering_luminance;
    doviStreamMetadata.level6_max_cll = vdrDmData->dm_data.level6->max_content_light_level;
    doviStreamMetadata.level6_max_fall = vdrDmData->dm_data.level6->max_frame_average_light_level;
  }

  const std::string metaVersion = BuildMetaVersionString(vdrDmData);
  const std::string sourceMetaVersion = BuildMetaVersionString(sourceVdrDmData);
  bool hasLevel254 = false;
  unsigned int level2Count = 0;
  unsigned int level8Count = 0;
  if (vdrDmData && vdrDmData->dm_data.level254)
  {
    hasLevel254 = true;
    level8Count = vdrDmData->dm_data.level8.len;
  }
  else if (vdrDmData && vdrDmData->dm_data.level1)
  {
    level2Count = vdrDmData->dm_data.level2.len;
  }

  logM(LOGINFO, "Parsed DoVi metadata (first frame): meta [{}] source_meta [{}] has_l254 [{}] l2_count [{}] l8_count [{}]",
                metaVersion, sourceMetaVersion, hasLevel254, level2Count, level8Count);

  doviStreamMetadata.source_meta_version = sourceMetaVersion;
  dataCacheCore.SetVideoDoViStreamMetadata(doviStreamMetadata);
}

inline DOVIELType GetDoviElType(const DoviRpuDataHeader* header)
{
  if (header && ((header->guessed_profile == 4) || (header->guessed_profile == 7)) && header->el_type)
  {
    if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
      return DOVIELType::TYPE_FEL;
    if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
      return DOVIELType::TYPE_MEL;
  }

  return DOVIELType::TYPE_NONE;
}

inline void PopulateDoviStreamInfo(const DoviRpuDataHeader* header,
                                   DOVIELType& doviElType,
                                   const AVDOVIDecoderConfigurationRecord& dovi,
                                   CDataCacheCore& dataCacheCore)
{
  DOVIStreamInfo doviStreamInfo;

  doviElType = GetDoviElType(header);
  doviStreamInfo.dovi_el_type = doviElType;
  doviStreamInfo.dovi = dovi;
  doviStreamInfo.has_config =
      (memcmp(&dovi, &CDVDStreamInfo::empty_dovi, sizeof(AVDOVIDecoderConfigurationRecord)) != 0);
  doviStreamInfo.has_header = (header != nullptr);

  dataCacheCore.SetVideoDoViStreamInfo(doviStreamInfo);
}

inline void PopulateDoviFirstFrameStreamInfo(DoviRpuOpaque* metadataOpaque,
                                             DoviRpuOpaque* sourceOpaque,
                                             DOVIELType& doviElType,
                                             AVDOVIDecoderConfigurationRecord& dovi,
                                             CDataCacheCore& dataCacheCore)
{
  const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(metadataOpaque);

  const bool needsSeparateSourceFetch = sourceOpaque && sourceOpaque != metadataOpaque;
  // When sourceOpaque == metadataOpaque we are reading the original unmodified metadata, so reuse
  // the already-fetched vdrDmData allocation instead of requesting and freeing a duplicate copy.
  const DoviVdrDmData* sourceVdrDmData =
      needsSeparateSourceFetch ? dovi_rpu_get_vdr_dm_data(sourceOpaque) : vdrDmData;

  PopulateDoviStreamMetadata(vdrDmData, sourceVdrDmData, dataCacheCore);

  const DoviRpuDataHeader* header = dovi_rpu_get_header(metadataOpaque);
  PopulateDoviStreamInfo(header, doviElType, dovi, dataCacheCore);
  dovi_rpu_free_header(header);

  // Only free sourceVdrDmData when it came from a separate sourceOpaque request above.
  if (needsSeparateSourceFetch)
    dovi_rpu_free_vdr_dm_data(sourceVdrDmData);

  dovi_rpu_free_vdr_dm_data(vdrDmData);
}

inline void PopulateDoviRpuInfo(DoviRpuOpaque* metadataOpaque,
                                DoviRpuOpaque* sourceOpaque,
                                double pts,
                                CDataCacheCore& dataCacheCore,
                                DOVIFrameMetadata* outDoViFrameMetadata = nullptr)
{
  const DoviVdrDmData* vdrDmData = dovi_rpu_get_vdr_dm_data(metadataOpaque);

  const bool needsSeparateSourceFetch = sourceOpaque && sourceOpaque != metadataOpaque;
  const DoviVdrDmData* sourceVdrDmData = needsSeparateSourceFetch
                                          ? dovi_rpu_get_vdr_dm_data(sourceOpaque)
                                          : vdrDmData;

  PopulateDoviFrameMetadata(vdrDmData, sourceVdrDmData, pts, dataCacheCore,
                            outDoViFrameMetadata);

  if (needsSeparateSourceFetch)
    dovi_rpu_free_vdr_dm_data(sourceVdrDmData);

  dovi_rpu_free_vdr_dm_data(vdrDmData);
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

  DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
  if (opaque)
  {
    PopulateDoviRpuInfo(opaque, opaque, pts, dataCacheCore);
    if (firstFrame)
      PopulateDoviFirstFrameStreamInfo(opaque, opaque, doviElType, dovi, dataCacheCore);
    dovi_rpu_free(opaque);
  }
}

void AppendCMv40ExtensionBlock(BitstreamIoWriter& writer)
{
  // CM v4.0 extension metadata (allowed levels: 3, 8, 9, 10, 11, 254)
  // -----------------------------------------------------------------
  writer.write_ue(4);                         // (00101) num_ext_blocks
  writer.byte_align();                        // dm_alignment_zero_bit

  // Currently the extension block content is fixed, if we need for dynamic values in the future
  // then need to gate and check changes and recreate for each change, see HDR10+ dynamic metadata handling for example.
  static const std::vector<uint8_t> cached_ext_blocks = []() {
    BitstreamIoWriter cacheWriter;

    // L3 ------------ (53 bits)
    cacheWriter.write_ue(5);                         // (00110)          length_bytes (payload only)
    cacheWriter.write_n<uint8_t>(3, 8);              // (00000011)       level
    cacheWriter.write_n<uint16_t>(2048, 12);         // (100000000000)   min_pq_offset
    cacheWriter.write_n<uint16_t>(2048, 12);         // (100000000000)   max_pq_offset
    cacheWriter.write_n<uint16_t>(2048, 12);         // (100000000000)   avg_pq_offset
    cacheWriter.write_n<uint8_t>(0, 4);              // (0000)           alignment of 4 bits. (40)

    // L9 ------------ (19 bits)
    cacheWriter.write_ue(1);                         // (010)            length_bytes (payload only)
    cacheWriter.write_n<uint8_t>(9, 8);              // (00001001)       level
    cacheWriter.write_n<uint8_t>(0, 8);              // (00000000)       source_primary_index

    // L11 ----------- (45 bits)
    cacheWriter.write_ue(4);                         // (00101)          length_bytes (payload only)
    cacheWriter.write_n<uint8_t>(11, 8);             // (00001011)       level
    cacheWriter.write_n<uint8_t>(1, 8);              // (00000001)       content_type
    cacheWriter.write_n<uint8_t>(0, 8);              // (00000000)       whitepoint
    cacheWriter.write_n<uint8_t>(0, 8);              // (00000000)       reserved_byte2
    cacheWriter.write_n<uint8_t>(0, 8);              // (00000000)       reserved_byte3

    // L254 ---------- (27 bits)
    cacheWriter.write_ue(2);                         // (011)            length_bytes (payload only)
    cacheWriter.write_n<uint8_t>(254, 8);            // (11111110)       level
    cacheWriter.write_n<uint8_t>(0, 8);              // (00000000)       dm_mode
    cacheWriter.write_n<uint8_t>(2, 8);              // (00000010)       dm_version_index

    cacheWriter.byte_align();                        // ext_dm_alignment_zero_bit
    return cacheWriter.into_inner();
  }();

  writer.write_bytes(cached_ext_blocks.data(), cached_ext_blocks.size());
}

bool CopyRbspBits(BitstreamIoWriter& writer,
                  const std::vector<uint8_t>& rbsp,
                  size_t startBit,
                  size_t bitCount)
{
  const size_t totalBits = rbsp.size() * 8;
  if ((startBit > totalBits) || (bitCount > (totalBits - startBit))) return false;

  BitstreamIoReader reader(rbsp);
  if (!reader.skip_bits(startBit)) return false;

  for (size_t i = 0; i < bitCount; ++i)
  {
    bool bit = false;
    if (!reader.read(bit)) return false;

    writer.write(bit);
  }

  return true;
}

bool GetCMv29PayloadInfo(const DoviVdrDmData* vdrDmData, size_t& dmPayloadEndBit)
{
  if (!vdrDmData || vdrDmData->cmv29_payload_end_bit == 0) return false;

  dmPayloadEndBit = vdrDmData->cmv29_payload_end_bit;
  return true;
}

// Build a CMv4.0-extended RPU payload with the extension inserted exactly at the
// CMv2.9 payload boundary reported by libdovi. The 2-byte HEVC NAL header is
// handled by the caller.
bool BuildCMv40NaluPayload(const std::vector<uint8_t>& rbsp,
                           size_t dmPayloadEndBit,
                           std::vector<uint8_t>& naluPayloadOut)
{
  if (dmPayloadEndBit == 0) return false;

  BitstreamIoWriter writer(rbsp.size() + 26);
  if (!CopyRbspBits(writer, rbsp, 0, dmPayloadEndBit)) return false;

  AppendCMv40ExtensionBlock(writer);
  writer.byte_align();

  writer.write_n<uint32_t>(Crc32::Compute(writer.as_slice() + 1, writer.as_slice_size() - 1), 32);
  writer.write_n<uint8_t>(0x80, 8);  // FINAL_BYTE

  std::vector<uint8_t> newRbsp = writer.into_inner();

  HevcAddStartCodeEmulationPrevention3Byte(newRbsp);

  naluPayloadOut.swap(newRbsp);

  return true;
}

// Parse a candidate NAL with libdovi and check that L254 is present.
// Returns the opaque RPU handle on success (caller must free), nullptr on failure.
DoviRpuOpaque* ParseAndValidateCmv40Nalu(const std::vector<uint8_t>& nalu)
{
  DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalu.data(), nalu.size());
  if (!opaque) return nullptr;

  const DoviVdrDmData* dm = dovi_rpu_get_vdr_dm_data(opaque);
  const bool valid = (dm && dm->dm_data.level254);
  dovi_rpu_free_vdr_dm_data(dm);

  if (valid) return opaque;

  dovi_rpu_free(opaque);
  return nullptr;
}

// Append CMv4.0 extension to an RPU NAL. On success, populates |out| with the
// new NAL and returns the validated DoviRpuOpaque* (caller must free).
// Returns nullptr on failure.
DoviRpuOpaque* AppendCMv40ToRpuNalu(uint8_t* nalBuf,
                                    int32_t nalSize,
                                    const DoviVdrDmData* sourceVdrDmData,
                                    std::vector<uint8_t>& out)
{
  if (!nalBuf || (nalSize <= 2)) return nullptr;

  std::vector<uint8_t> rbsp;
  HevcClearStartCodeEmulationPrevention3Byte(nalBuf + 2, static_cast<size_t>(nalSize - 2), rbsp);

  if (rbsp.size() < 2) return nullptr;

  size_t dmPayloadEndBit = 0;
  if (!GetCMv29PayloadInfo(sourceVdrDmData, dmPayloadEndBit))
    return nullptr;

  std::vector<uint8_t> naluPayload;
  if (!BuildCMv40NaluPayload(rbsp, dmPayloadEndBit, naluPayload))
    return nullptr;

  std::vector<uint8_t> naluOut;
  naluOut.reserve(2 + naluPayload.size());
  naluOut.push_back(nalBuf[0]);
  naluOut.push_back(nalBuf[1]);
  naluOut.insert(naluOut.end(), naluPayload.begin(), naluPayload.end());

  DoviRpuOpaque* opaque = ParseAndValidateCmv40Nalu(naluOut);
  if (!opaque) return nullptr;

  out.swap(naluOut);
  return opaque;
}

inline DOVIELType GetElTypeFromHeader(const DoviRpuDataHeader* header)
{
  if (header && ((header->guessed_profile == 4) || (header->guessed_profile == 7)) &&
      header->el_type)
  {
    if (StringUtils::EqualsNoCase(header->el_type, "FEL"))
      return DOVIELType::TYPE_FEL;
    if (StringUtils::EqualsNoCase(header->el_type, "MEL"))
      return DOVIELType::TYPE_MEL;
  }

  return DOVIELType::TYPE_NONE;
}

inline void ConvertDoVi(DOVIMode convertMode,
                        bool firstFrame,
                        DoviRpuOpaque* opaque,
                        const DoviRpuDataHeader*& header,
                        const DoviVdrDmData*& vdrDmData,
                        CDVDStreamInfo& hints,
                        CDataCacheCore& dataCacheCore,
                        uint8_t*& nalBuf,
                        int32_t& nalSize,
                        const DoviData*& rpuData)
{
  if (!header || (header->guessed_profile != 7)) return;

  if (firstFrame)
  {
    DOVIStreamInfo doviStreamInfo;
    doviStreamInfo.dovi_el_type = GetElTypeFromHeader(header);
    doviStreamInfo.dovi = hints.dovi;
    dataCacheCore.SetVideoSourceDoViStreamInfo(doviStreamInfo);
  }

  if (dovi_convert_rpu_with_mode(opaque, convertMode) >= 0)
    rpuData = dovi_write_unspec62_nalu(opaque);

  if (!rpuData) return;

  nalBuf = const_cast<uint8_t*>(rpuData->data);
  nalSize = rpuData->len;

  hints.dovi.el_present_flag = 0; // EL removed in both conversion cases - to MEL and to P8.1
  if (convertMode == DOVIMode::MODE_TO81)
  {
    hints.dovi.dv_profile = 8;
    hints.dovi.dv_bl_signal_compatibility_id = 1;
  }

  dovi_rpu_free_header(header);

  header = dovi_rpu_get_header(opaque);
  dovi_rpu_free_vdr_dm_data(vdrDmData);
  vdrDmData = dovi_rpu_get_vdr_dm_data(opaque);
}

inline void AppendCMv40(DOVICMv40Mode cmv40Mode,
                        const DoviRpuDataHeader* header,
                        const DoviVdrDmData* vdrDmData,
                        uint8_t*& nalBuf,
                        int32_t& nalSize,
                        std::vector<uint8_t>& nalu,
                        DoviRpuOpaque*& opaque)
{
  if (!header || !vdrDmData) return;

  const bool hasLevel254 = vdrDmData->dm_data.level254;
  if (hasLevel254) return;

  const bool level2IsEmpty = (vdrDmData->dm_data.level2.len == 0);
  const bool shouldAppend =
      (cmv40Mode == DOVICMv40Mode::CMV40_ALWAYS) || level2IsEmpty;
  if (!shouldAppend) return;

  opaque = AppendCMv40ToRpuNalu(nalBuf, nalSize, vdrDmData, nalu);
  if (opaque)
  {
    nalBuf = nalu.data();
    nalSize = static_cast<int32_t>(nalu.size());
  }
}

inline void InjectPtsForFel(DOVIMode convertMode,
                            DOVIELType doviElType,
                            double pts,
                            uint8_t*& nalBuf,
                            int32_t& nalSize,
                            std::vector<uint8_t>& nalu)
{
  if ((convertMode != DOVIMode::MODE_NONE) ||
      (doviElType != DOVIELType::TYPE_FEL) ||
      !IsValidPtsForInjection(pts) || !nalBuf || (nalSize <= 0)) return;

  if (nalu.empty())
    nalu.assign(nalBuf, nalBuf + nalSize);

  if (AppendPtsToDoviRpuNalu(nalu, static_cast<uint64_t>(pts)))
  {
    nalBuf = nalu.data();
    nalSize = static_cast<int32_t>(nalu.size());
  }
}
} // namespace

void CBitstreamConverter::ProcessDoViRpuWrap(
  uint8_t* nalBuf,
  int32_t nalSize,
  uint8_t** poutbuf,
  uint32_t& poutbufSize,
  double pts)
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
  double pts)
{
  const DoviData* rpuData = nullptr;
  DoviRpuOpaque* appendOpaque = nullptr;
  std::vector<uint8_t> nalu;

  // Optimization: If the input RPU NAL is exactly identical to the previous frame's RPU NAL,
  // AND we are not processing the first frame (which parses stream metadata),
  // we can completely skip `dovi_parse_unspec62_nalu` and avoid all allocations & format processing.
  // This cache deliberately tracks the original input RPU, before any FEL PTS trailer is injected,
  // so a per-frame PTS never invalidates reuse.
  if (!m_first_frame && CachedRpuInputMatches(m_cached_dovi_rpu_in_nal, nalBuf, nalSize))
  {
    m_cached_dovi_frame_metadata.pts = pts;
    m_dataCacheCore.SetVideoDoViFrameMetadata(m_cached_dovi_frame_metadata);

    // Skip all processing and restore the fully configured/converted output NAL
    nalBuf = m_cached_dovi_rpu_out_nal.data();
    nalSize = static_cast<int32_t>(m_cached_dovi_rpu_out_nal.size());
  }
  else
  {
    // Save the original input stream bits before processing modifications
    m_cached_dovi_rpu_in_nal.assign(nalBuf, nalBuf + nalSize);

    DoviRpuOpaque* opaque = dovi_parse_unspec62_nalu(nalBuf, nalSize);
    const DoviRpuDataHeader* header = opaque ? dovi_rpu_get_header(opaque) : nullptr;
    const DoviVdrDmData* vdrDmData = opaque ? dovi_rpu_get_vdr_dm_data(opaque) : nullptr;

    if (opaque && (m_convert_dovi != DOVIMode::MODE_NONE))
      ConvertDoVi(m_convert_dovi,
                  m_first_frame,
                  opaque,
                  header,
                  vdrDmData,
                  m_hints,
                  m_dataCacheCore,
                  nalBuf,
                  nalSize,
                  rpuData);

    if (m_append_cmv40 != DOVICMv40Mode::CMV40_NONE)
      AppendCMv40(m_append_cmv40,
                  header,
                  vdrDmData,
                  nalBuf,
                  nalSize,
                  nalu,
                  appendOpaque);

    // Use the appendOpaque from the append CMv4.0 if available
    DoviRpuOpaque* metadataOpaque = appendOpaque ? appendOpaque : opaque;
    if (metadataOpaque)
    {
      PopulateDoviRpuInfo(metadataOpaque, opaque, pts, m_dataCacheCore,
                          &m_cached_dovi_frame_metadata);
      if (m_first_frame)
        PopulateDoviFirstFrameStreamInfo(
            metadataOpaque, opaque, m_hints.dovi_el_type, m_hints.dovi, m_dataCacheCore);
    }

    if (header) dovi_rpu_free_header(header);
    if (vdrDmData) dovi_rpu_free_vdr_dm_data(vdrDmData);
    if (opaque) dovi_rpu_free(opaque);
    if (appendOpaque)
    {
      dovi_rpu_free(appendOpaque);
      if (m_first_frame)
        logM(LOGINFO, "CMv4.0 extension appended to RPU");
    }

    // Update cache with the newly calculated modified NAL out for the next frame
    m_cached_dovi_rpu_out_nal.assign(nalBuf, nalBuf + nalSize);
  }

  InjectPtsForFel(m_convert_dovi,
                  m_hints.dovi_el_type,
                  pts,
                  nalBuf,
                  nalSize,
                  nalu);

  // HEVC NAL unit type 62 is Dolby Vision RPU (UNSPEC62)
  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalBuf, nalSize, 62);

  if (rpuData) dovi_data_free(rpuData);
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
  auto nalu = create_dovi_rpu_nalu_from_hdr10plus(meta, m_convert_Hdr10Plus_peak_brightness_source,
                                                  m_hdrStaticMetadataInfo);

  if (nalu.empty()) return;

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

  GetDoviRpuInfo(nalu.data(), static_cast<uint32_t>(nalu.size()), m_first_frame, m_hints.dovi_el_type,
                m_hints.dovi, pts, m_dataCacheCore);

  BitstreamAllocAndCopy(poutbuf, poutbufSize, nullptr, 0, nalu.data(),
                        static_cast<uint32_t>(nalu.size()), HEVC_NAL_UNSPEC62);
}
