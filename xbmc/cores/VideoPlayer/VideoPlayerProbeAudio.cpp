/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerProbeAudio.h"

#include "AudioSinkAE.h"
#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "DVDDemuxers/DVDDemux.h"
#include "DVDStreamInfo.h"
#include "Interface/DemuxPacket.h"
#include "Process/ProcessInfo.h"

#include <deque>

namespace
{
constexpr unsigned int PROBE_MAX_PACKETS = 96;

bool Supports(const CDVDStreamInfo& hint)
{
  return (STREAM_SOURCE_MASK(hint.source) == STREAM_SOURCE_DEMUX) &&
         (hint.type == StreamType::AUDIO);
}

bool IsRelevantAudioProbePacket(const CDVDStreamInfo& hint,
                                const CDemuxStream& stream,
                                const DemuxPacket& packet)
{
  if ((stream.type != StreamType::AUDIO) || (stream.source != STREAM_SOURCE_DEMUX))
    return false;

  return (packet.demuxerId == hint.demuxerId) && (packet.iStreamId == hint.uniqueId);
}

bool ResolveAudioProbeResult(CDVDStreamInfo& hint,
                             CDVDAudioCodec& codec,
                             const DVDAudioFrame& audioframe,
                             VideoPlayerProbeAudio::AudioProbeResult& result)
{
  if ((audioframe.nb_frames == 0) || (audioframe.format.m_sampleRate <= 0))
    return false;

  if (!audioframe.passthrough && (audioframe.format.m_channelLayout.Count() <= 0))
    return false;

  result.format = audioframe.format;
  result.bitsPerSample = audioframe.bits_per_sample;
  result.passthrough = audioframe.passthrough;

  hint.samplerate = audioframe.format.m_sampleRate;
  if (audioframe.format.m_channelLayout.Count() > 0)
    hint.channels = audioframe.format.m_channelLayout.Count();
  if (audioframe.bits_per_sample > 0)
    hint.bitspersample = audioframe.bits_per_sample;

  const int profile = codec.GetProfile();
  if (profile > 0)
    hint.profile = profile;

  return true;
}

bool DrainProbeFrame(CDVDStreamInfo& hint,
                     CDVDAudioCodec& codec,
                     VideoPlayerProbeAudio::AudioProbeResult& result)
{
  DVDAudioFrame audioframe{};
  codec.GetData(audioframe);
  return ResolveAudioProbeResult(hint, codec, audioframe, result);
}
}

namespace VideoPlayerProbeAudio
{
bool Run(CDVDDemux& demuxer,
         CDVDStreamInfo& hint,
         CProcessInfo& processInfo,
         const std::atomic<bool>& abortRequested,
         AudioProbeResult& result)
{
  result = {};

  if (!Supports(hint))
    return false;

  bool allowpassthrough = true;
  const auto streamType =
      CAudioSinkAE::ResolvePassthroughType(hint.codec, hint.samplerate, hint.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      hint, processInfo, allowpassthrough, processInfo.AllowDTSHDDecode(), streamType);
  if (!codec)
    return false;

  bool probed = false;
  unsigned int packetCount = 0;
  std::deque<DemuxPacket*> rdPkts;

  while (!abortRequested.load(std::memory_order_relaxed) && packetCount < PROBE_MAX_PACKETS)
  {
    DemuxPacket* packet = demuxer.Read();
    if (!packet)
      break;

    rdPkts.emplace_back(packet);
    packetCount++;

    if (packet->iStreamId == DMX_SPECIALID_STREAMCHANGE)
      break;

    if ((packet->iStreamId < 0) || (packet->pData == nullptr) || (packet->iSize <= 0))
      continue;

    CDemuxStream* stream = demuxer.GetStream(packet->demuxerId, packet->iStreamId);
    if (!stream || !IsRelevantAudioProbePacket(hint, *stream, *packet))
      continue;

    probed = true;

    if (!codec->AddData(*packet))
    {
      if (DrainProbeFrame(hint, *codec, result))
        break;
      continue;
    }

    if (DrainProbeFrame(hint, *codec, result))
      break;
  }

  demuxer.ReplayPackets(rdPkts);
  codec->Dispose();
  return probed && (result.format.m_sampleRate > 0);
}
}
