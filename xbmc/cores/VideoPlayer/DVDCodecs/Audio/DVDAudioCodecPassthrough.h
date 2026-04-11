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
#include "settings/lib/ISettingCallback.h"

#include <atomic>
#include <list>
#include <memory>
#include <vector>

class CProcessInfo;
class CPackerMAT;

class CSetting;

class CDVDAudioCodecPassthrough : public CDVDAudioCodec, public ISettingCallback
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

  void OnSettingChanged(const std::shared_ptr<const CSetting>& setting) override;

private:
  // Internal sentinel for "no valid PTS" - use -1.0 instead of DVD_NOPTS_VALUE
  // to avoid confusion with garbage values during seamless branching.
  static constexpr double LOCAL_NOPTS = -1.0;

  static constexpr size_t STARTUP_JITTER_WINDOW_SIZE = 8;
  static constexpr size_t STARTUP_WARMUP_SAMPLES = 2;

  struct StartupJitterState
  {
    AudioSync::CFloatingAverage<double, STARTUP_JITTER_WINDOW_SIZE> tracker;
    size_t warmupSamples{0};
    bool correctionApplied{false};
    bool rejectionLogged{false};
  };

  struct StartupJitterEvaluation
  {
    double minimum{0.0};
    double maximum{0.0};
    double average{0.0};
    double spread{0.0};
    bool consistentSign{false};
    bool minCorrectionOk{false};
    bool maxCorrectionOk{false};
    bool spreadOk{false};

    bool Accepted() const
    {
      return consistentSign && minCorrectionOk && maxCorrectionOk && spreadOk;
    }
  };

  struct IgnoreWindowState
  {
    double until{LOCAL_NOPTS};
    bool logged{false};
  };

  enum class IgnoreWindowKind
  {
    PassthroughResync,
    TrueHdCorrection,
  };

  void UpdateDialNormSettings();
  void ResetStartupJitterState(StartupJitterState& state);
  void ResetPassthroughStartupState(double resyncJitterIgnoreUntil);
  bool ShouldIgnoreWindow(IgnoreWindowState& state, IgnoreWindowKind kind);
  bool EvaluateStartupJitterState(StartupJitterState& state,
                                  double jitter,
                                  double minCorrection,
                                  double maxCorrection,
                                  double maxSpread,
                                  StartupJitterEvaluation& evaluation);
  double EvaluateDtsHdMaStartupCorrection(double jitter);
  bool ApplyDtsHdMaStartupCorrection(double jitter, DVDAudioFrame& frame);
  double EvaluateTrueHdStartupCorrection(double jitter, double samplesOffsetTime);
  bool ApplyTrueHdStartupCorrection(double jitter,
                                    double samplesOffsetTime,
                                    DVDAudioFrame& frame);
  void ApplyPassthroughJitterCorrection(double correction,
                                        bool resetTracker,
                                        bool signalDiscontinuity,
                                        DVDAudioFrame& frame);
  double GetTrueHdCorrectionClamp() const
  {
    return m_deviceIsRAW ? TRUEHD_CORRECTION_CHUNK_RAW : TRUEHD_CORRECTION_CHUNK_IEC;
  }
  double GetTrueHdCorrectionCooldown() const
  {
    return m_deviceIsRAW ? TRUEHD_CORRECTION_COOLDOWN_RAW : TRUEHD_CORRECTION_COOLDOWN_IEC;
  }
  const char* GetPassthroughPathLabel() const
  {
    return m_deviceIsRAW ? "RAW" : "IEC";
  }

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
  // Bitstreaming codecs use a coarser 100ms correction gate than PCM to avoid
  // reacting to minor receiver or packetization timing noise.
  static constexpr double JITTER_THRESHOLD_TRUEHD_DTS = 100000.0;   // 100ms
  static constexpr double JITTER_THRESHOLD_DEFAULT = 10000.0;       // 10ms
  static constexpr double PASSTHROUGH_ABNORMAL_JITTER = 1000000.0;  // 1s

  // DTS-HD MA can carry track-specific startup offsets that are smaller than the
  // coarse passthrough jitter threshold above. Learn a stable baseline from the
  // first few post-resync jitter samples and apply it once.
  static constexpr double DTSHD_MA_STARTUP_MIN_CORRECTION = 10000.0;  // 10ms

  // Keep this slightly above the coarse 100ms passthrough jitter gate so
  // stable startup offsets around that boundary do not fall into a dead zone.
  static constexpr double DTSHD_MA_STARTUP_MAX_CORRECTION = 120000.0;  // 120ms
  static constexpr double DTSHD_MA_STARTUP_MAX_SPREAD = 10000.0;      // 10ms

  // TrueHD via MAT can expose a stable startup offset that should be learned
  // once before enabling coarse residual-drift correction.
  static constexpr double TRUEHD_STARTUP_MIN_CORRECTION = 10000.0;   // 10ms
  static constexpr double TRUEHD_STARTUP_MAX_CORRECTION = 150000.0;  // 150ms
  static constexpr double TRUEHD_STARTUP_MAX_SPREAD = 5000.0;        // 5ms
  static constexpr double TRUEHD_CORRECTION_CHUNK_RAW = 25000.0;     // 25ms
  static constexpr double TRUEHD_CORRECTION_CHUNK_IEC = 15000.0;     // 15ms
  static constexpr double TRUEHD_CORRECTION_COOLDOWN_RAW = 100000.0; // 100ms
  static constexpr double TRUEHD_CORRECTION_COOLDOWN_IEC = 150000.0; // 150ms

  // After an explicit coordinated RESYNC from VideoPlayerAudio, trust that
  // clock for a short settling window so stale demuxer PTS values from a
  // display-reset reopen do not immediately trigger a large passthrough jitter
  // correction that undoes the resync.
  static constexpr double PASSTHROUGH_RESYNC_JITTER_HOLDOFF = 250000.0;  // 250ms

  // Runtime jitter threshold - set based on codec in Open()
  double m_jitterThreshold{JITTER_THRESHOLD_DEFAULT};

  StartupJitterState m_startupJitterState;
  IgnoreWindowState m_trueHdCorrectionCooldown;
  IgnoreWindowState m_resyncJitterHoldoff;

  // Cached settings (updated via callback, read in hot path)
  std::atomic<bool> m_defeatAC3DialNorm{false};
  std::atomic<bool> m_defeatTrueHDDialNorm{false};
};
