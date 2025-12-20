/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 *
 *  A/V sync improvements based on LAV Filters by Hendrik Leppkes (Nevcairiel)
 *  https://github.com/Nevcairiel/LAVFilters
 */

#pragma once

#include "DVDAudioCodec.h"
#include "FloatingAverage.h"
#include "cores/AudioEngine/Utils/AEAudioFormat.h"
#include "cores/AudioEngine/Utils/AEBitstreamPacker.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"

#include <list>
#include <memory>
#include <vector>

class CProcessInfo;
class CPackerMAT;

class CDVDAudioCodecPassthrough : public CDVDAudioCodec
{
public:
  CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType);
  ~CDVDAudioCodecPassthrough() override;

  bool Open(CDVDStreamInfo &hints, CDVDCodecOptions &options) override;
  void Dispose() override;
  bool AddData(const DemuxPacket &packet) override;
  void GetData(DVDAudioFrame &frame) override;
  void Reset() override;
  AEAudioFormat GetFormat() override { return m_format; }
  bool NeedPassthrough() override { return true; }
  std::string GetName() override { return m_codecName; }
  int GetBufferSize() override;

  // LAV-style sync methods for VideoPlayerAudio integration
  void ResetLavSyncState();
  void SyncToResyncPts(double pts);

private:
  int GetData(uint8_t** dst);
  unsigned int PackTrueHD();
  CAEStreamParser m_parser;
  uint8_t* m_buffer = nullptr;
  unsigned int m_bufferSize = 0;
  unsigned int m_dataSize = 0;
  AEAudioFormat m_format;
  uint8_t *m_backlogBuffer = nullptr;
  unsigned int m_backlogBufferSize = 0;
  unsigned int m_backlogSize = 0;

  // Internal sentinel for "no valid PTS" - use -1.0 instead of DVD_NOPTS_VALUE
  // to avoid confusion with garbage values during seamless branching
  static constexpr double LOCAL_NOPTS = -1.0;

  double m_currentPts{LOCAL_NOPTS};   // Current demuxer PTS
  double m_nextPts{LOCAL_NOPTS};      // Next expected PTS
  double m_lastOutputPts{LOCAL_NOPTS}; // Legacy: last output PTS (kept for compatibility)
  std::string m_codecName;

  // TrueHD specifics
  std::unique_ptr<CPackerMAT> m_packerMAT;
  std::vector<uint8_t> m_trueHDBuffer;
  unsigned int m_trueHDoffset = 0;
  unsigned int m_trueHDframes = 0;
  bool m_deviceIsRAW{false};

  // TrueHD timestamp caching (LAV-style) - cache PTS of first frame in MAT assembly
  double m_truehd_ptsCache{LOCAL_NOPTS};  // Cached PTS for current MAT frame being assembled
  bool m_truehd_ptsCacheValid{false};

  //============================================================================
  // LAV-style Internal Clock A/V Sync (applies to ALL passthrough codecs)
  //============================================================================
  // Based on LAV Filters by Hendrik Leppkes (Nevcairiel)
  // https://github.com/Nevcairiel/LAVFilters
  //
  // KEY ARCHITECTURAL CHANGE:
  // Instead of adjusting demuxer PTS, we maintain our OWN internal clock.
  // - m_internalClock is our running output timestamp (source of truth)
  // - On reset/discontinuity: sync m_internalClock to demuxer PTS once
  // - Then run freely: frame.pts = m_internalClock; m_internalClock += duration
  // - Demuxer PTS is only used to detect drift (jitter tracking)
  // - Correct by adjusting m_internalClock when drift exceeds threshold
  //
  // BENEFITS:
  // - Hardware clock chaos (GetSyncError) becomes irrelevant
  // - DV mode switches don't affect our internal timing
  // - Frame-to-frame output is always smooth (duration-based)
  // - We control when to trust the demuxer (only on explicit resync)
  //============================================================================

  // Internal clock - OUR running output timestamp (like LAV's m_rtStart)
  double m_internalClock{LOCAL_NOPTS};

  // Resync flag - when true, sync m_internalClock to next valid demuxer PTS
  bool m_needsResync{true};

  // Jitter tracking using LAV-style FloatingAverage
  // Tracks drift between our internal clock and demuxer PTS
  static constexpr size_t JITTER_WINDOW_SIZE = 256;
  AudioSync::CFloatingAverage<double, JITTER_WINDOW_SIZE> m_jitterTracker;

  // Jitter correction thresholds (in DVD_TIME_BASE units = microseconds)
  // LAV Filters: TrueHD/DTS use 10x threshold for bitstreaming tolerance
  static constexpr double JITTER_THRESHOLD_TRUEHD_DTS = 100000.0;  // 100ms
  static constexpr double JITTER_THRESHOLD_DEFAULT = 10000.0;      // 10ms

  // Runtime jitter threshold - set based on codec in Open()
  double m_jitterThreshold{JITTER_THRESHOLD_DEFAULT};
};
