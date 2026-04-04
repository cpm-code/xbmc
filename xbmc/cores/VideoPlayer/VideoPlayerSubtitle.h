/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DVDDemuxSPU.h"
#include "DVDMessageQueue.h"
#include "DVDOverlayContainer.h"
#include "DVDStreamInfo.h"
#include "DVDSubtitles/DVDFactorySubtitle.h"
#include "IVideoPlayer.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "threads/Event.h"
#include "threads/Thread.h"

#include <atomic>

class CDVDInputStream;
class CDVDSubtitleStream;
class CDVDSubtitleParser;
class CDVDInputStreamNavigator;
class CDVDOverlayCodec;

class CVideoPlayerSubtitle : public CThread, public IDVDStreamPlayer
{
public:
  CVideoPlayerSubtitle(CDVDOverlayContainer* pOverlayContainer, CProcessInfo &processInfo);
  ~CVideoPlayerSubtitle() override;

  void UpdatePlaybackPosition(double pts, double offset);
  void Flush();
  void FindSubtitles(const char* strFilename);
  int GetSubtitleCount();

  void UpdateOverlayInfo(const std::shared_ptr<CDVDInputStreamNavigator>& pStream, int iAction)
  {
    m_pOverlayContainer->UpdateOverlayInfo(pStream, &m_dvdspus, iAction);
  }

  bool AcceptsData() const override;
  void SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority = 0) override;
  void FlushMessages() override { m_messageQueue.Flush(); }
  bool OpenStream(CDVDStreamInfo hints) override { return OpenStream(hints, hints.filename); }
  bool OpenStream(CDVDStreamInfo &hints, std::string& filename);
  void CloseStream(bool bWaitForBuffers) override;

  bool IsInited() const override { return m_messageQueue.IsInited(); }
  bool IsStalled() const override { return m_pOverlayContainer->GetSize() == 0; }
protected:
  void Process() override;

private:
  void HandleMessage(const std::shared_ptr<CDVDMsg>& pMsg);
  void ProcessParser(double pts, double offset);

  CDVDOverlayContainer* m_pOverlayContainer;
  CDVDMessageQueue m_messageQueue;
  CEvent m_wakeEvent{true};

  std::unique_ptr<CDVDSubtitleParser> m_pSubtitleFileParser;
  std::unique_ptr<CDVDOverlayCodec> m_pOverlayCodec;
  CDVDDemuxSPU        m_dvdspus;

  CDVDStreamInfo      m_streaminfo;
  double              m_lastPts;
  std::atomic_bool    m_usesTimedParser{false};
  std::atomic_bool    m_hasPendingTiming{false};
  std::atomic<double> m_latestPts{DVD_NOPTS_VALUE};
  std::atomic<double> m_latestOffset{0.0};


  CCriticalSection    m_section;
};


//typedef struct SubtitleInfo
//{

//
//} SubtitleInfo;

