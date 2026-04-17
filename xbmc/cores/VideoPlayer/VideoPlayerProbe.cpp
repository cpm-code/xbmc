/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerProbe.h"
#include "DualLayerPairing.h"

#include "DVDDemuxers/DVDDemux.h"
#include "Interface/DemuxPacket.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "utils/BitstreamConverter.h"

#include <cstring>

namespace
{
constexpr unsigned int PROBE_MAX_PACKETS = 96;
constexpr unsigned int PROBE_MAX_VIDEO_PACKETS = 16;
constexpr size_t PROBE_MAX_DUAL_PACKETS = 4;

bool HasDoviStreamInfo(const DOVIStreamInfo& doviStreamInfo)
{
  return doviStreamInfo.has_config ||
         doviStreamInfo.has_header ||
         (doviStreamInfo.dovi_el_type != DOVIELType::TYPE_NONE) ||
         (memcmp(&doviStreamInfo.dovi, &CDVDStreamInfo::empty_dovi,
                 sizeof(AVDOVIDecoderConfigurationRecord)) != 0);
}

void InitializeVideoSourceProbeState(CDVDStreamInfo& hint)
{
  hint.amlVideoOpen.sourceHdrType = hint.hdrType;
  hint.amlVideoOpen.sourceAdditionalHdrType = StreamHdrType::HDR_TYPE_NONE;
  hint.amlVideoOpen.sourceDoViStreamInfo = {};

  auto& dataCache = CServiceBroker::GetDataCacheCore();
  dataCache.SetVideoSourceHdrType(hint.amlVideoOpen.sourceHdrType);
  dataCache.SetVideoSourceAdditionalHdrType(hint.amlVideoOpen.sourceAdditionalHdrType);
  dataCache.SetVideoDoViStreamInfo({});
  dataCache.SetVideoSourceDoViStreamInfo(hint.amlVideoOpen.sourceDoViStreamInfo);
}

void SyncProbedSourceInfo(CDVDStreamInfo& hint)
{
  auto& dataCache = CServiceBroker::GetDataCacheCore();
  hint.amlVideoOpen.sourceHdrType = dataCache.GetVideoSourceHdrType();
  hint.amlVideoOpen.sourceAdditionalHdrType = dataCache.GetVideoSourceAdditionalHdrType();

  auto doviStreamInfo = dataCache.GetVideoSourceDoViStreamInfo();
  const auto currentDoViStreamInfo = dataCache.GetVideoDoViStreamInfo();

  if (HasDoviStreamInfo(currentDoViStreamInfo))
  {
    doviStreamInfo = currentDoViStreamInfo;
    dataCache.SetVideoSourceDoViStreamInfo(doviStreamInfo);
  }

  hint.amlVideoOpen.sourceDoViStreamInfo = doviStreamInfo;
}

bool IsRelevantHdrProbePacket(const CDVDStreamInfo& hint,
                              const CDemuxStream& stream,
                              const DemuxPacket& packet)
{
  if ((stream.type != StreamType::VIDEO) || (stream.source != STREAM_SOURCE_DEMUX))
    return false;

  if (packet.demuxerId != hint.demuxerId)
    return false;

  if (packet.isDualStream)
    return true;

  return packet.iStreamId == hint.uniqueId;
}

bool IsHdrProbeResolved(const CDVDStreamInfo& hint, StreamHdrType initialHdrType)
{
  const auto sourceHdrType = hint.amlVideoOpen.sourceHdrType;
  const auto sourceAdditionalHdrType = hint.amlVideoOpen.sourceAdditionalHdrType;

  if ((hint.hdrType != initialHdrType) ||
      (sourceAdditionalHdrType != StreamHdrType::HDR_TYPE_NONE))
    return true;

  if ((sourceHdrType == StreamHdrType::HDR_TYPE_HLG) ||
      (sourceHdrType == StreamHdrType::HDR_TYPE_HDR10PLUS))
    return true;

  if (sourceHdrType == StreamHdrType::HDR_TYPE_DOLBYVISION)
  {
    const auto& doviStreamInfo = hint.amlVideoOpen.sourceDoViStreamInfo;

    if (hint.dovi.el_present_flag &&
        !doviStreamInfo.has_header &&
        (doviStreamInfo.dovi_el_type == DOVIELType::TYPE_NONE))
      return false;

    return HasDoviStreamInfo(doviStreamInfo);
  }

  return false;
}

bool Supports(const CDVDStreamInfo& hint)
{
  return (hint.source == STREAM_SOURCE_DEMUX) && (hint.codec == AV_CODEC_ID_HEVC);
}
}

namespace VideoPlayerProbe
{
bool Run(CDVDDemux& demuxer,
         CDVDStreamInfo& hint,
         const std::atomic<bool>& abortRequested)
{
  if (!Supports(hint))
    return false;

  InitializeVideoSourceProbeState(hint);
  if (IsHdrProbeResolved(hint, hint.hdrType))
    return false;

  CBitstreamConverter bitstream(hint);
  bitstream.Open(true);

  const StreamHdrType initialHdrType = hint.hdrType;
  bool probed = false;
  unsigned int packetCount = 0;
  unsigned int videoPacketCount = 0;
  std::deque<DemuxPacket*> rdPkts;
  std::deque<DemuxPacket*> dualLayerPackets;

  while (!abortRequested.load(std::memory_order_relaxed) &&
         packetCount < PROBE_MAX_PACKETS &&
         videoPacketCount < PROBE_MAX_VIDEO_PACKETS)
  {
    DemuxPacket* packet = demuxer.Read();
    if (!packet) break;

    rdPkts.emplace_back(packet);
    packetCount++;

    if (packet->iStreamId == DMX_SPECIALID_STREAMCHANGE) break;

    if (packet->iStreamId < 0 || packet->pData == nullptr || packet->iSize <= 0)
      continue;

    CDemuxStream* stream = demuxer.GetStream(packet->demuxerId, packet->iStreamId);
    if (!stream || !IsRelevantHdrProbePacket(hint, *stream, *packet))
      continue;

    probed = true;

    if (packet->isDualStream)
    {
      if (VideoPlayerDualLayer::CanPairWithFront(dualLayerPackets, packet->isELPackage,
                                                 packet->dts))
      {
        DemuxPacket* peer = dualLayerPackets.front();
        if (packet->isELPackage)
          bitstream.Convert(peer->pData, peer->iSize, packet->pData, packet->iSize, packet->pts);
        else
          bitstream.Convert(packet->pData, packet->iSize, peer->pData, peer->iSize, packet->pts);

        dualLayerPackets.pop_front();
        videoPacketCount++;
        SyncProbedSourceInfo(hint);
      }
      else
      {
        VideoPlayerDualLayer::QueuePendingPacket(dualLayerPackets, packet,
                                                 PROBE_MAX_DUAL_PACKETS);
      }
    }
    else
    {
      bitstream.Convert(packet->pData, packet->iSize, packet->pts);
      videoPacketCount++;
      SyncProbedSourceInfo(hint);
    }

    if (IsHdrProbeResolved(hint, initialHdrType))
      break;
  }

  demuxer.ReplayPackets(rdPkts);
  return probed;
}
}
