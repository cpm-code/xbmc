/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "HevcSei.h"
#include "HDR10Plus.h"

#include <algorithm>

namespace
{
constexpr uint8_t SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35 = 4;
constexpr uint8_t SEI_TYPE_MASTERING_DISPLAY_COLOUR_VOLUME = 137;
constexpr uint8_t SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO = 144;
// ITU-T H.265 Annex D alternative_transfer_characteristics payload type.
constexpr uint8_t SEI_TYPE_ALTERNATIVE_TRANSFER_CHARACTERISTICS = 147;

bool HasContainedPayload(
  const CHevcSei& sei,
  const std::vector<uint8_t>& buf,
  size_t minSize = 0)
{
  return (sei.m_payloadSize >= minSize) &&
         ((sei.m_payloadOffset + sei.m_payloadSize) <= buf.size());
}

bool HasHdr10PlusPayload(
  const CHevcSei& sei,
  const std::vector<uint8_t>& buf)
{
  return (sei.m_payloadType == SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35) &&
         HasContainedPayload(sei, buf, 7);
}

bool IsHdr10PlusSeiMessage(
  const CHevcSei& sei,
  const std::vector<uint8_t>& buf)
{
  if (!HasHdr10PlusPayload(sei, buf)) return false;

  CBitstreamReader br(buf.data() + sei.m_payloadOffset, sei.m_payloadSize);
  const auto itu_t_t35_country_code = br.ReadBits(8);
  const auto itu_t_t35_terminal_provider_code = br.ReadBits(16);
  const auto itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);

  // United States, Samsung Electronics America, ST 2094-40
  if ((itu_t_t35_country_code == 0xB5) &&
      (itu_t_t35_terminal_provider_code == 0x003C) &&
      (itu_t_t35_terminal_provider_oriented_code == 0x0001))
  {
    const auto application_identifier = br.ReadBits(8);
    const auto application_version = br.ReadBits(8);

    return (application_identifier == 4) &&
           (application_version <= 1);
  }

  return false;
}

std::optional<Hdr10PlusMetadata> ExtractHdr10Plus(const CHevcSei& message,
                                                  const std::vector<uint8_t>& buf)
{
  if (!IsHdr10PlusSeiMessage(message, buf)) return std::nullopt;

  CBitstreamReader br(buf.data() + message.m_payloadOffset, message.m_payloadSize);
  return hdr10plus_sei_to_metadata(br);
}

std::optional<MasteringDisplayColourVolume> ExtractMasteringDisplayColourVolume(
  const CHevcSei& message,
  const std::vector<uint8_t>& buf)
{
  if (!HasContainedPayload(message, buf, 24)) return std::nullopt;

  CBitstreamReader br(buf.data() + message.m_payloadOffset, message.m_payloadSize);

  MasteringDisplayColourVolume metadata;

  for (int i = 0; i < 3; ++i)
  {
    metadata.displayPrimaries[i].x = br.ReadBits(16);
    metadata.displayPrimaries[i].y = br.ReadBits(16);
  }

  metadata.whitePoint.x = br.ReadBits(16);
  metadata.whitePoint.y = br.ReadBits(16);

  const uint32_t maxLuminanceRaw = br.ReadBits(32);
  const uint32_t minLuminanceRaw = br.ReadBits(32);
  metadata.maxLuminance = static_cast<uint32_t>(maxLuminanceRaw) / 10000.0f;
  metadata.minLuminance = static_cast<uint32_t>(minLuminanceRaw);

  return metadata;
}

std::optional<ContentLightLevel> ExtractContentLightLevel(const CHevcSei& message,
                                                          const std::vector<uint8_t>& buf)
{
  if (!HasContainedPayload(message, buf, 4)) return std::nullopt;

  CBitstreamReader br(buf.data() + message.m_payloadOffset, message.m_payloadSize);
  uint16_t maxCLL = br.ReadBits(16);
  uint16_t maxFALL = br.ReadBits(16);

  return ContentLightLevel{maxCLL, maxFALL};
}

std::optional<uint8_t> ExtractAlternativeTransferCharacteristics(const CHevcSei& message,
                                                                 const std::vector<uint8_t>& buf)
{
  if (!HasContainedPayload(message, buf, 1)) return std::nullopt;

  CBitstreamReader br(buf.data() + message.m_payloadOffset, message.m_payloadSize);
  return static_cast<uint8_t>(br.ReadBits(8));
}

CHevcSei::Metadata ExtractMetadata(const std::vector<CHevcSei>& messages,
                                   const std::vector<uint8_t>& buf)
{
  CHevcSei::Metadata metadata;

  for (const CHevcSei& sei : messages)
  {
    switch (sei.m_payloadType)
    {
      case SEI_TYPE_USER_DATA_REGISTERED_ITU_T_T35:
        if (!metadata.hdr10Plus)
          metadata.hdr10Plus = ExtractHdr10Plus(sei, buf);
        break;
      case SEI_TYPE_MASTERING_DISPLAY_COLOUR_VOLUME:
        if (!metadata.masteringDisplayColourVolume)
          metadata.masteringDisplayColourVolume = ExtractMasteringDisplayColourVolume(sei, buf);
        break;
      case SEI_TYPE_CONTENT_LIGHT_LEVEL_INFO:
        if (!metadata.contentLightLevel)
          metadata.contentLightLevel = ExtractContentLightLevel(sei, buf);
        break;
      case SEI_TYPE_ALTERNATIVE_TRANSFER_CHARACTERISTICS:
        if (!metadata.alternativeTransferCharacteristics)
          metadata.alternativeTransferCharacteristics =
            ExtractAlternativeTransferCharacteristics(sei, buf);
        break;
      default:
        break;
    }

    if (metadata.hdr10Plus && metadata.masteringDisplayColourVolume &&
        metadata.contentLightLevel && metadata.alternativeTransferCharacteristics)
      break;
  }

  return metadata;
}
} // namespace

void HevcAddStartCodeEmulationPrevention3Byte(
  std::vector<uint8_t>& buf)
{
  size_t i = 0;

  while (i < buf.size())
  {
    if (i > 2 && buf[i - 2] == 0 && buf[i - 1] == 0 && buf[i] <= 3)
      buf.insert(buf.begin() + i, 3);

    i += 1;
  }
}

void HevcClearStartCodeEmulationPrevention3Byte(
  const uint8_t* buf,
  const size_t len,
  std::vector<uint8_t>& out)
{
  size_t i = 0;

  if (len > 2)
  {
    out.reserve(len);

    out.emplace_back(buf[0]);
    out.emplace_back(buf[1]);

    for (i = 2; i < len; i++)
    {
      if (!(buf[i - 2] == 0 && buf[i - 1] == 0 && buf[i] == 3))
        out.emplace_back(buf[i]);
    }
  }
  else
  {
    out.assign(buf, buf + len);
  }
}

int CHevcSei::ParseSeiMessage(
  CBitstreamReader& br,
  std::vector<CHevcSei>& messages)
{
  CHevcSei sei;
  uint8_t lastPayloadTypeByte{0};
  uint8_t lastPayloadSizeByte{0};

  sei.m_msgOffset = br.Position() / 8;

  lastPayloadTypeByte = br.ReadBits(8);
  while (lastPayloadTypeByte == 0xFF)
  {
    lastPayloadTypeByte = br.ReadBits(8);
    sei.m_payloadType += 255;
  }

  sei.m_payloadType += lastPayloadTypeByte;

  lastPayloadSizeByte = br.ReadBits(8);
  while (lastPayloadSizeByte == 0xFF)
  {
    lastPayloadSizeByte = br.ReadBits(8);
    sei.m_payloadSize += 255;
  }

  sei.m_payloadSize += lastPayloadSizeByte;
  sei.m_payloadOffset = br.Position() / 8;

  // Invalid size
  if (sei.m_payloadSize > br.AvailableBits())
    return 1;

  br.SkipBits(sei.m_payloadSize * 8);
  messages.emplace_back(sei);

  return 0;
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbspInternal(
  const uint8_t* buf,
  const size_t len)
{
  std::vector<CHevcSei> messages;

  if (len > 4)
  {
    CBitstreamReader br(buf, len);

    // forbidden_zero_bit, nal_type, nuh_layer_id, temporal_id
    // nal_type == SEI_PREFIX should already be verified by caller
    br.SkipBits(16);

    while (true)
    {
      if (ParseSeiMessage(br, messages))
        break;

      if (br.AvailableBits() <= 8)
        break;
    }
  }

  return messages;
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbsp(
  const uint8_t* buf,
  const size_t len)
{
  return ParseSeiRbspInternal(buf, len);
}

std::vector<CHevcSei> CHevcSei::ParseSeiRbspUnclearedEmulation(
  const uint8_t* inData,
  const size_t inDataLen,
  std::vector<uint8_t>& buf)
{
  HevcClearStartCodeEmulationPrevention3Byte(inData, inDataLen, buf);
  return ParseSeiRbsp(buf.data(), buf.size());
}

CHevcSei::Metadata CHevcSei::ExtractMetadata(const uint8_t* inData, const size_t inDataLen)
{
  std::vector<uint8_t> buf;
  const auto messages = ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);
  return ::ExtractMetadata(messages, buf);
}

const std::vector<uint8_t> CHevcSei::RemoveHdr10PlusFromSeiNalu(
  const uint8_t* inData,
  const size_t inDataLen)
{
  std::vector<uint8_t> buf;
  auto messages = CHevcSei::ParseSeiRbspUnclearedEmulation(inData, inDataLen, buf);
  const auto hdr10Plus = std::find_if(messages.cbegin(), messages.cend(),
                         [&buf](const CHevcSei& sei) { return IsHdr10PlusSeiMessage(sei, buf); });

  if (hdr10Plus != messages.cend())
  {
    if (messages.size() > 1)
    {
      // Multiple SEI messages in NALU, remove only the HDR10+ one
      buf.erase(std::next(buf.begin(), hdr10Plus->m_msgOffset),
                std::next(buf.begin(), hdr10Plus->m_payloadOffset + hdr10Plus->m_payloadSize));
      HevcAddStartCodeEmulationPrevention3Byte(buf);
    }
    else
    {
      // Single SEI message in NALU
      buf.clear();
    }
  }
  else
  {
    // No HDR10+
    buf.clear();
  }

  return buf;
}
