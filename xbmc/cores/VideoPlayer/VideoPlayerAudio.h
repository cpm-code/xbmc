/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "AudioSinkAE.h"
#include "DVDClock.h"
#include "DVDCodecs/Audio/FloatingAverage.h"
#include "DVDMessageQueue.h"
#include "DVDStreamInfo.h"
#include "IVideoPlayer.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "cores/VideoPlayer/VideoRenderers/RenderManager.h"
#include "threads/SystemClock.h"
#include "threads/Thread.h"
#include "utils/BitstreamStats.h"

#include <chrono>
#include <list>
#include <mutex>
#include <utility>

class CDataCacheCore;
class CDVDAudioCodec;
class CVideoPlayer;

class CVideoPlayerAudio : public CThread, public IDVDStreamPlayerAudio
{
public:
  struct StartupSinkConfig
  {
    AEAudioFormat format;
    int bitsPerSample{0};
    bool passthrough{false};
  };

  CVideoPlayerAudio(CDVDClock* pClock,
                    CDVDMessageQueue& parent,
                    CRenderManager& renderManager,
                    CProcessInfo& processInfo,
                    double messageQueueTimeSize);
  ~CVideoPlayerAudio() override;

  bool OpenStream(CDVDStreamInfo hints) override;
  void CloseStream(bool bWaitForBuffers) override;
  void SetStartupSinkConfig(const StartupSinkConfig& config);
  void ClearStartupSinkConfig();
  bool HasConfiguredSink() { return m_audioSink.HasStream(); }

  void SetSpeed(int speed) override;
  void Flush(bool sync) override;

  // waits until all available data has been rendered
  bool AcceptsData() const override;
  bool HasData() const override { return m_messageQueue.GetDataSize() > 0; }
  int GetLevel() const override { return m_messageQueue.GetLevel(); }
  bool IsInited() const override { return m_messageQueue.IsInited(); }

  void SendMessage(std::shared_ptr<CDVDMsg> pMsg, int priority = 0) override
  {
    m_messageQueue.Put(pMsg, priority);
  }

  void FlushMessages() override { m_messageQueue.Flush(); }

  void SetDynamicRangeCompression(long drc) override
  {
    m_audioSink.SetDynamicRangeCompression(drc);
  }

  float GetDynamicRangeAmplification() const override { return 0.0f; }

  std::string GetPlayerInfo1() override;
  std::string GetPlayerInfo2() override;
  int GetAudioChannels() override;

  double GetCurrentPts() override
  {
    std::unique_lock lock(m_info_section);
    return m_info.pts;
  }

  double GetCurrentPacketDelay() override
  {
    std::lock_guard lock(m_info_section);
    return m_info.packetDelay;
  }

  bool IsStalled() const override { return m_stalled; }
  bool IsPassthrough() const override;

protected:
  void OnStartup() override;
  void OnExit() override;
  void Process() override;

  bool ProcessDecoderOutput(DVDAudioFrame& audioframe);
  void UpdatePlayerInfo();
  void ResetPassthroughClockSyncDebugState()
  {
    m_lastPassthroughClockSyncCorrection = 0.0;
    m_hasLastPassthroughClockSyncCorrection = false;
  }
  void OpenStream(CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec);
  bool TakeStartupSinkConfig(StartupSinkConfig& config);
  //! Switch codec if needed. Called when the sample rate gotten from the
  //! codec changes, in which case we may want to switch passthrough on/off.
  bool SwitchCodecIfNeeded();
  void SetSyncType(bool passthrough);

  CDVDMessageQueue m_messageQueue;
  CDVDMessageQueue& m_messageParent;

  // Access to adjust the tweak the latency because of audio
  CRenderManager& m_renderManager;
  CDataCacheCore& m_dataCacheCore;

  // holds stream information for current playing stream
  CDVDStreamInfo m_streaminfo;

  double m_audioClock;

  CAudioSinkAE m_audioSink; // audio output device
  CDVDClock* m_pClock; // dvd master clock
  std::unique_ptr<CDVDAudioCodec> m_pAudioCodec; // audio codec
  BitstreamStats m_audioStats;

  int m_speed;
  bool m_stalled;
  bool m_paused;
  IDVDStreamPlayer::ESyncState m_syncState;
  XbmcThreads::EndTime<> m_syncTimer;

  int m_synctype;
  int m_prevsynctype;

  bool m_prevskipped;
  double m_maxspeedadjust;

  struct SInfo
  {
    std::string info1;
    std::string info2;
    double pts = DVD_NOPTS_VALUE;
    double packetDelay = 0.0;
    bool passthrough = false;
  };

  mutable CCriticalSection m_info_section;
  SInfo m_info;

  bool m_displayReset = false;
  unsigned int m_disconAdjustTimeMs = 10; // maximum sync-off before adjusting
  int m_disconAdjustCounter = 0;
  double m_lastPassthroughClockSyncCorrection{0.0};
  bool m_hasLastPassthroughClockSyncCorrection{false};

  //============================================================================
  // LAV-style Jitter Tracking for PCM/Decoded Audio
  //============================================================================
  // LAV Filters approach: maintain a running output timestamp (m_rtStart) and
  // compare against input (demuxer) timestamps. On discontinuity (flush/seek),
  // resync the output timestamp to input, then let it run freely.
  //
  // m_pcmOutputClock: Our running output timestamp (equivalent to LAV's m_rtStart)
  // m_pcmResyncTimestamp: Flag to resync on next valid input timestamp (like LAV's m_bResyncTimestamp)
  //
  // Jitter = m_pcmOutputClock (calculated) - audioframe.pts (input)
  // When jitter exceeds threshold, adjust m_pcmOutputClock.
  //============================================================================
  static constexpr size_t PCM_JITTER_WINDOW_SIZE = 64; // LAV uses 64 for non-bitstream
  static constexpr double PCM_JITTER_THRESHOLD =
      10000.0; // 10ms in DVD_TIME_BASE (LAV: MAX_JITTER_DESYNC)
  AudioSync::CFloatingAverage<double, PCM_JITTER_WINDOW_SIZE> m_pcmJitterTracker;
  double m_pcmOutputClock{0.0}; // Running output timestamp (like LAV's m_rtStart)
  bool m_pcmResyncTimestamp{true}; // Resync output clock to input on next valid PTS

  CCriticalSection m_startupSinkConfigSection;
  StartupSinkConfig m_startupSinkConfig;
  bool m_hasStartupSinkConfig{false};
};
