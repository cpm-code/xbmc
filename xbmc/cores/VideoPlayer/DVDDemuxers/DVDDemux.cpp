/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDDemux.h"

#include "DVDDemuxUtils.h"

#include "ServiceBroker.h"
#include "resources/LocalizeStrings.h"
#include "resources/ResourcesComponent.h"
#include "utils/StreamUtils.h"
#include "utils/StringUtils.h"

namespace
{
class CDVDDemuxReplay final : public CDVDDemux
{
public:
  explicit CDVDDemuxReplay(std::unique_ptr<CDVDDemux> demux)
    : m_demux(std::move(demux))
  {
    SetDemuxerId(m_demux->GetDemuxerId());
  }

  bool Reset() override
  {
    ClearReplay();
    return m_demux->Reset();
  }

  void Abort() override
  {
    m_demux->Abort();
  }

  void Flush() override
  {
    ClearReplay();
    m_demux->Flush();
  }

  DemuxPacket* Read() override
  {
    if (DemuxPacket* pkt = ReadReplay())
      return pkt;

    return m_demux->Read();
  }

  bool SeekTime(double time, bool backwards = false, double* startpts = nullptr) override
  {
    return m_demux->SeekTime(time, backwards, startpts);
  }

  bool SeekChapter(int chapter, double* startpts = nullptr) override
  {
    return m_demux->SeekChapter(chapter, startpts);
  }

  int GetChapterCount() override
  {
    return m_demux->GetChapterCount();
  }

  int GetChapter() override
  {
    return m_demux->GetChapter();
  }

  void GetChapterName(std::string& strChapterName, int chapterIdx = -1) override
  {
    m_demux->GetChapterName(strChapterName, chapterIdx);
  }

  std::chrono::milliseconds GetChapterPos(int chapterIdx = -1) override
  {
    return m_demux->GetChapterPos(chapterIdx);
  }

  void SetSpeed(int iSpeed) override
  {
    m_demux->SetSpeed(iSpeed);
  }

  void FillBuffer(bool mode) override
  {
    m_demux->FillBuffer(mode);
  }

  int GetStreamLength() override
  {
    return m_demux->GetStreamLength();
  }

  CDemuxStream* GetStream(int64_t demuxerId, int iStreamId) const override
  {
    return m_demux->GetStream(demuxerId, iStreamId);
  }

  std::vector<CDemuxStream*> GetStreams() const override
  {
    return m_demux->GetStreams();
  }

  int GetNrOfStreams() const override
  {
    return m_demux->GetNrOfStreams();
  }

  int GetPrograms(std::vector<ProgramInfo>& programs) override
  {
    return m_demux->GetPrograms(programs);
  }

  void SetProgram(int progId) override
  {
    m_demux->SetProgram(progId);
  }

  std::string GetFileName() override
  {
    return m_demux->GetFileName();
  }

  std::string GetStreamCodecName(int64_t demuxerId, int iStreamId) override
  {
    return m_demux->GetStreamCodecName(demuxerId, iStreamId);
  }

  void EnableStream(int64_t demuxerId, int id, bool enable) override
  {
    m_demux->EnableStream(demuxerId, id, enable);
  }

  void OpenStream(int64_t demuxerId, int id) override
  {
    m_demux->OpenStream(demuxerId, id);
  }

  void SetVideoResolution(unsigned int width, unsigned int height) override
  {
    m_demux->SetVideoResolution(width, height);
  }

protected:
  void EnableStream(int id, bool enable) override
  {
    m_demux->EnableStream(m_demux->GetDemuxerId(), id, enable);
  }

  void OpenStream(int id) override
  {
    m_demux->OpenStream(m_demux->GetDemuxerId(), id);
  }

  CDemuxStream* GetStream(int iStreamId) const override
  {
    return m_demux->GetStream(m_demux->GetDemuxerId(), iStreamId);
  }

  std::string GetStreamCodecName(int iStreamId) override
  {
    return m_demux->GetStreamCodecName(m_demux->GetDemuxerId(), iStreamId);
  }

private:
  std::unique_ptr<CDVDDemux> m_demux;
};
}

std::string CDemuxStreamAudio::GetStreamType() const
{
  std::string strInfo;
  switch (codec)
  {
    case AV_CODEC_ID_AC3:
      strInfo = "AC3";
      break;
    case AV_CODEC_ID_AC4:
      strInfo = "AC4";
      break;
    case AV_CODEC_ID_EAC3:
    {
      if (profile == AV_PROFILE_EAC3_DDP_ATMOS)
        strInfo = "DD+ ATMOS";
      else
        strInfo = "DD+";
      break;
    }
    case AV_CODEC_ID_DTS:
    {
      switch (profile)
      {
        case AV_PROFILE_DTS_96_24:
          strInfo = "DTS 96/24";
          break;
        case AV_PROFILE_DTS_ES:
          strInfo = "DTS ES";
          break;
        case AV_PROFILE_DTS_EXPRESS:
          strInfo = "DTS EXPRESS";
          break;
        case AV_PROFILE_DTS_HD_MA:
          strInfo = "DTS-HD MA";
          break;
        case AV_PROFILE_DTS_HD_HRA:
          strInfo = "DTS-HD HRA";
          break;
        case AV_PROFILE_DTS_HD_MA_X:
          strInfo = "DTS-HD MA X";
          break;
        case AV_PROFILE_DTS_HD_MA_X_IMAX:
          strInfo = "DTS-HD MA X (IMAX)";
          break;
        default:
          strInfo = "DTS";
          break;
      }
      break;
    }
    case AV_CODEC_ID_MP2:
      strInfo = "MP2";
      break;
    case AV_CODEC_ID_MP3:
      strInfo = "MP3";
      break;
    case AV_CODEC_ID_TRUEHD:
      if (profile == AV_PROFILE_TRUEHD_ATMOS)
        strInfo = "TrueHD ATMOS";
      else
        strInfo = "TrueHD";
      break;
    case AV_CODEC_ID_AAC:
    {
      switch (profile)
      {
        case AV_PROFILE_AAC_LOW:
        case AV_PROFILE_MPEG2_AAC_LOW:
          strInfo = "AAC-LC";
          break;
        case AV_PROFILE_AAC_HE:
        case AV_PROFILE_MPEG2_AAC_HE:
          strInfo = "HE-AAC";
          break;
        case AV_PROFILE_AAC_HE_V2:
          strInfo = "HE-AACv2";
          break;
        case AV_PROFILE_AAC_SSR:
          strInfo = "AAC-SSR";
          break;
        case AV_PROFILE_AAC_LTP:
          strInfo = "AAC-LTP";
          break;
        default:
        {
          // Try check by codec full string according to RFC 6381
          if (codecName == "mp4a.40.2" || codecName == "mp4a.40.17")
            strInfo = "AAC-LC";
          else if (codecName == "mp4a.40.3")
            strInfo = "AAC-SSR";
          else if (codecName == "mp4a.40.4" || codecName == "mp4a.40.19")
            strInfo = "AAC-LTP";
          else if (codecName == "mp4a.40.5")
            strInfo = "HE-AAC";
          else if (codecName == "mp4a.40.29")
            strInfo = "HE-AACv2";
          else
            strInfo = "AAC";
          break;
        }
      }
      break;
    }
    case AV_CODEC_ID_ALAC:
      strInfo = "ALAC";
      break;
    case AV_CODEC_ID_FLAC:
      strInfo = "FLAC";
      break;
    case AV_CODEC_ID_OPUS:
      strInfo = "Opus";
      break;
    case AV_CODEC_ID_VORBIS:
      strInfo = "Vorbis";
      break;
    default:
      break;
  }

  if (codec >= AV_CODEC_ID_PCM_S16LE && codec <= AV_CODEC_ID_PCM_SGA)
    strInfo = "PCM";

  if (strInfo.empty())
    strInfo = CServiceBroker::GetResourcesComponent().GetLocalizeStrings().Get(13205); // "Unknown"

  strInfo.append(" ");
  strInfo.append(StreamUtils::GetLayout(iChannels));

  return strInfo;
}

int CDVDDemux::GetNrOfStreams(StreamType streamType) const
{
  int iCounter = 0;

  for (auto pStream : GetStreams())
  {
    if (pStream && pStream->type == streamType)
      iCounter++;
  }

  return iCounter;
}

CDVDDemux::~CDVDDemux()
{
  ClearReplay();
}

CDVDDemux* CDVDDemux::Wrap(std::unique_ptr<CDVDDemux> demux)
{
  if (!demux)
    return nullptr;

  return new CDVDDemuxReplay(std::move(demux));
}

void CDVDDemux::ReplayPacket(DemuxPacket* pkt)
{
  if (pkt) m_replay.emplace_back(pkt);
}

void CDVDDemux::ReplayPackets(std::deque<DemuxPacket*>& pkts)
{
  while (!pkts.empty())
  {
    ReplayPacket(pkts.front());
    pkts.pop_front();
  }
}

void CDVDDemux::ClearReplay()
{
  while (!m_replay.empty())
  {
    CDVDDemuxUtils::FreeDemuxPacket(m_replay.front());
    m_replay.pop_front();
  }
}

DemuxPacket* CDVDDemux::ReadReplay()
{
  if (m_replay.empty()) return nullptr;

  DemuxPacket* pkt = m_replay.front();
  m_replay.pop_front();
  return pkt;
}

int CDVDDemux::GetNrOfSubtitleStreams() const
{
  return GetNrOfStreams(StreamType::SUBTITLE);
}

std::string CDemuxStream::GetStreamName()
{
  return name;
}
