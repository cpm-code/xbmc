/*
 *  Copyright (C) 2010-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDAudioCodecPassthrough.h"

#include "DVDCodecs/DVDCodecs.h"
#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "cores/AudioEngine/Utils/PackerMAT.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>

extern "C"
{
#include <libavcodec/avcodec.h>
}

namespace
{
constexpr unsigned int TRUEHD_BUF_SIZE = 61440;

// Helper to check if a PTS value is valid
// Valid PTS must be >= 0 and <= 24 hours (way beyond any real content)
// LOCAL_NOPTS is now defined in the class header as static constexpr
constexpr double MAX_REASONABLE_PTS = 86400000000.0; // 24 hours in DVD_TIME_BASE units

inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}

inline double ClampMagnitude(double value, double limit)
{
  return std::clamp(value, -limit, limit);
}
}

void CDVDAudioCodecPassthrough::ResetStartupJitterState(
    CDVDAudioCodecPassthrough::StartupJitterState& state)
{
  state.tracker.Reset();
  state.warmupSamples = 0;
  state.correctionApplied = false;
  state.rejectionLogged = false;
}

void CDVDAudioCodecPassthrough::ResetPassthroughStartupState(double resyncJitterIgnoreUntil)
{
  ResetStartupJitterState(m_startupJitterState);
  m_trueHdCorrectionCooldown.until = LOCAL_NOPTS;
  m_trueHdCorrectionCooldown.logged = false;
  m_resyncJitterHoldoff.until = resyncJitterIgnoreUntil;
  m_resyncJitterHoldoff.logged = false;
}

bool CDVDAudioCodecPassthrough::ShouldIgnoreWindow(
    CDVDAudioCodecPassthrough::IgnoreWindowState& state,
    CDVDAudioCodecPassthrough::IgnoreWindowKind kind)
{
  if (!IsValidPts(state.until) || m_internalClock >= state.until)
  {
    state.until = LOCAL_NOPTS;
    state.logged = false;
    return false;
  }

  if (!state.logged)
  {
    state.logged = true;
    const double remainingMs = (state.until - m_internalClock) / 1000.0;

    switch (kind)
    {
      case IgnoreWindowKind::PassthroughResync:
        logM(LOGDEBUG, "Ignoring passthrough jitter for {:.2f}ms after RESYNC", remainingMs);
        break;

      case IgnoreWindowKind::TrueHdCorrection:
        logM(LOGDEBUG, "Ignoring TrueHD jitter for {:.2f}ms after {} correction",
             remainingMs, GetPassthroughPathLabel());
        break;
    }
  }

  return true;
}

bool CDVDAudioCodecPassthrough::EvaluateStartupJitterState(
    CDVDAudioCodecPassthrough::StartupJitterState& state,
    double jitter,
    double minCorrection,
    double maxCorrection,
    double maxSpread,
    CDVDAudioCodecPassthrough::StartupJitterEvaluation& evaluation)
{
  if (state.correctionApplied)
    return false;

  if (state.warmupSamples < STARTUP_WARMUP_SAMPLES)
  {
    state.warmupSamples++;
    return false;
  }

  state.tracker.Sample(jitter);
  if (!state.tracker.IsFull())
    return false;

  evaluation.minimum = state.tracker.Minimum();
  evaluation.maximum = state.tracker.Maximum();
  evaluation.average = state.tracker.Average();
  evaluation.spread = evaluation.maximum - evaluation.minimum;
  evaluation.consistentSign = (evaluation.minimum > 0.0 && evaluation.maximum > 0.0) ||
                              (evaluation.minimum < 0.0 && evaluation.maximum < 0.0);
  evaluation.minCorrectionOk = std::abs(evaluation.average) >= minCorrection;
  evaluation.maxCorrectionOk = std::abs(evaluation.average) <= maxCorrection;
  evaluation.spreadOk = evaluation.spread <= maxSpread;

  if (evaluation.Accepted())
    state.correctionApplied = true;

  return true;
}

double CDVDAudioCodecPassthrough::EvaluateDtsHdMaStartupCorrection(double jitter)
{
  StartupJitterEvaluation evaluation;
  if (!EvaluateStartupJitterState(m_startupJitterState,
                                  jitter,
                                  DTSHD_MA_STARTUP_MIN_CORRECTION,
                                  DTSHD_MA_STARTUP_MAX_CORRECTION,
                                  DTSHD_MA_STARTUP_MAX_SPREAD,
                                  evaluation))
    return 0.0;

  if (!evaluation.Accepted())
  {
    if (!m_startupJitterState.rejectionLogged)
    {
      m_startupJitterState.rejectionLogged = true;

      logM(LOGDEBUG, "DTS-HD MA startup baseline rejected "
                     "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms, sign:{:d}, minOk:{:d}, maxOk:{:d}, spreadOk:{:d})",
                     (evaluation.average / 1000.0), (evaluation.minimum / 1000.0),
                     (evaluation.maximum / 1000.0), (evaluation.spread / 1000.0),
                     evaluation.consistentSign ? 1 : 0, evaluation.minCorrectionOk ? 1 : 0,
                     evaluation.maxCorrectionOk ? 1 : 0, evaluation.spreadOk ? 1 : 0);
    }

    return 0.0;
  }

  logM(LOGDEBUG, "DTS-HD MA startup baseline correction {:.2f}ms "
                 "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms)",
                 (evaluation.average / 1000.0), (evaluation.average / 1000.0),
                 (evaluation.minimum / 1000.0), (evaluation.maximum / 1000.0),
                 (evaluation.spread / 1000.0));

  return evaluation.average;
}

bool CDVDAudioCodecPassthrough::ApplyDtsHdMaStartupCorrection(double jitter, DVDAudioFrame& frame)
{
  const double startupCorrection = EvaluateDtsHdMaStartupCorrection(jitter);
  if (startupCorrection == 0.0) return false;

  ApplyPassthroughJitterCorrection(startupCorrection, true, true, frame);
  return true;
}

double CDVDAudioCodecPassthrough::EvaluateEac3StartupCorrection(double jitter)
{
  StartupJitterEvaluation evaluation;
  if (!EvaluateStartupJitterState(m_startupJitterState,
                                  jitter,
                                  EAC3_STARTUP_MIN_CORRECTION,
                                  EAC3_STARTUP_MAX_CORRECTION,
                                  EAC3_STARTUP_MAX_SPREAD,
                                  evaluation))
    return 0.0;

  if (!evaluation.Accepted())
  {
    if (!m_startupJitterState.rejectionLogged)
    {
      m_startupJitterState.rejectionLogged = true;

      logM(LOGDEBUG, "DD+ startup baseline rejected "
                     "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms, sign:{:d}, minOk:{:d}, maxOk:{:d}, spreadOk:{:d})",
                     (evaluation.average / 1000.0), (evaluation.minimum / 1000.0),
                     (evaluation.maximum / 1000.0), (evaluation.spread / 1000.0),
                     evaluation.consistentSign ? 1 : 0, evaluation.minCorrectionOk ? 1 : 0,
                     evaluation.maxCorrectionOk ? 1 : 0, evaluation.spreadOk ? 1 : 0);
    }

    return 0.0;
  }

  logM(LOGDEBUG, "DD+ startup baseline correction {:.2f}ms "
                 "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms)",
                 (evaluation.average / 1000.0), (evaluation.average / 1000.0),
                 (evaluation.minimum / 1000.0), (evaluation.maximum / 1000.0),
                 (evaluation.spread / 1000.0));

  return evaluation.average;
}

bool CDVDAudioCodecPassthrough::ApplyEac3StartupCorrection(double jitter,
                                                           DVDAudioFrame& frame)
{
  const double startupCorrection = EvaluateEac3StartupCorrection(jitter);
  if (startupCorrection == 0.0)
    return false;

  ApplyPassthroughJitterCorrection(startupCorrection, true, true, frame);
  return true;
}

double CDVDAudioCodecPassthrough::EvaluateTrueHdStartupCorrection(double jitter,
                                                                  double samplesOffsetTime)
{
  StartupJitterEvaluation evaluation;
  if (!EvaluateStartupJitterState(m_startupJitterState,
                                  jitter,
                                  TRUEHD_STARTUP_MIN_CORRECTION,
                                  TRUEHD_STARTUP_MAX_CORRECTION,
                                  TRUEHD_STARTUP_MAX_SPREAD,
                                  evaluation))
    return 0.0;

  if (!evaluation.Accepted())
  {
    if (!m_startupJitterState.rejectionLogged)
    {
      m_startupJitterState.rejectionLogged = true;

      logM(LOGDEBUG, "TrueHD startup baseline rejected "
                     "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms, "
                     "clock {:.2f}ms, MAT {:.2f}ms, path {}, sign:{:d}, minOk:{:d}, "
                     "maxOk:{:d}, spreadOk:{:d})",
                     (evaluation.average / 1000.0), (evaluation.minimum / 1000.0),
                     (evaluation.maximum / 1000.0), (evaluation.spread / 1000.0),
                     ((evaluation.average - samplesOffsetTime) / 1000.0),
                     (samplesOffsetTime / 1000.0), GetPassthroughPathLabel(),
                     evaluation.consistentSign ? 1 : 0, evaluation.minCorrectionOk ? 1 : 0,
                     evaluation.maxCorrectionOk ? 1 : 0, evaluation.spreadOk ? 1 : 0);
    }

    return 0.0;
  }

  logM(LOGDEBUG, "TrueHD startup baseline correction {:.2f}ms "
                 "(min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms, "
                 "clock {:.2f}ms, MAT {:.2f}ms, path {})",
                 (evaluation.average / 1000.0), (evaluation.minimum / 1000.0),
                 (evaluation.maximum / 1000.0), (evaluation.spread / 1000.0),
                 ((evaluation.average - samplesOffsetTime) / 1000.0),
               (samplesOffsetTime / 1000.0), GetPassthroughPathLabel());

  return evaluation.average;
}

bool CDVDAudioCodecPassthrough::ApplyTrueHdStartupCorrection(double jitter,
                                                             double samplesOffsetTime,
                                                             DVDAudioFrame& frame)
{
  const double startupCorrection = EvaluateTrueHdStartupCorrection(jitter, samplesOffsetTime);
  if (startupCorrection == 0.0) return false;

  ApplyPassthroughJitterCorrection(startupCorrection, true, false, frame);
  m_trueHdCorrectionCooldown.until = m_internalClock + GetTrueHdCorrectionCooldown();
  m_trueHdCorrectionCooldown.logged = false;
  return true;
}

void CDVDAudioCodecPassthrough::ApplyPassthroughJitterCorrection(double correction,
                                                                 bool resetTracker,
                                                                 bool signalDiscontinuity,
                                                                 DVDAudioFrame& frame)
{
  m_internalClock -= correction;

  if (resetTracker)
    m_jitterTracker.Reset();
  else
    m_jitterTracker.OffsetValues(-correction);

  if (signalDiscontinuity)
  {
    frame.hasDiscontinuity = true;
    frame.discontinuityCorrection = correction;
  }
}

CDVDAudioCodecPassthrough::CDVDAudioCodecPassthrough(CProcessInfo &processInfo, CAEStreamInfo::DataType streamType) :
  CDVDAudioCodec(processInfo)
{
  m_format.m_streamInfo.m_type = streamType;
  m_deviceIsRAW = processInfo.WantsRawPassthrough();

  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
    {
      settings->RegisterCallback(this, {CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM,
                                        CSettings::SETTING_COREELEC_AUDIO_TRUEHD_ATMOS_DIALNORM});
    }
  }

  UpdateDialNormSettings();

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    m_trueHDBuffer.resize(TRUEHD_BUF_SIZE);

    if (!m_deviceIsRAW)
      m_packerMAT = std::make_unique<CPackerMAT>();
  }
}

CDVDAudioCodecPassthrough::~CDVDAudioCodecPassthrough(void)
{
  if (const auto settingsComponent = CServiceBroker::GetSettingsComponent())
  {
    if (const auto settings = settingsComponent->GetSettings())
      settings->UnregisterCallback(this);
  }

  Dispose();
}

void CDVDAudioCodecPassthrough::UpdateDialNormSettings()
{
  const auto settingsComponent = CServiceBroker::GetSettingsComponent();
  const auto settings = settingsComponent ? settingsComponent->GetSettings() : nullptr;
  if (!settings) return;

  m_defeatAC3DialNorm.store(settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM));
  m_defeatTrueHDDialNorm.store(settings->GetBool(CSettings::SETTING_COREELEC_AUDIO_TRUEHD_ATMOS_DIALNORM));
}

void CDVDAudioCodecPassthrough::OnSettingChanged(const std::shared_ptr<const CSetting>& setting)
{
  if (!setting) return;

  const std::string& settingId = setting->GetId();
  if (settingId == CSettings::SETTING_COREELEC_AUDIO_AC3_DIALNORM ||
      settingId == CSettings::SETTING_COREELEC_AUDIO_TRUEHD_ATMOS_DIALNORM)
  {
    UpdateDialNormSettings();
  }
}

bool CDVDAudioCodecPassthrough::Open(CDVDStreamInfo &hints, CDVDCodecOptions &options)
{
  UpdateDialNormSettings();

  m_parser.SetCoreOnly(false);
  switch (m_format.m_streamInfo.m_type)
  {
    case CAEStreamInfo::STREAM_TYPE_AC3:
      m_codecName = "pt-ac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      m_parser.SetDefeatAC3DialNorm(m_defeatAC3DialNorm.load());
      break;

    case CAEStreamInfo::STREAM_TYPE_EAC3:
      m_codecName = "pt-eac3";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      m_parser.SetDefeatAC3DialNorm(m_defeatAC3DialNorm.load());
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_MA:
      m_codecName = "pt-dtshd_ma";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      m_codecName = "pt-dtshd_hra";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      m_codecName = "pt-dts";
      m_parser.SetCoreOnly(true);
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_codecName = "pt-truehd";
      m_jitterThreshold = JITTER_THRESHOLD_DEFAULT;
      m_parser.SetDefeatTrueHDDialNorm(m_defeatTrueHDDialNorm.load());

      CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::{} - passthrough output device is {}",
                __func__, m_deviceIsRAW ? "RAW" : "IEC");
      break;

    default:
      return false;
  }

  CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::{} - jitter threshold {:.0f}ms for {}",
            __func__, m_jitterThreshold / 1000.0, m_codecName);

  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;
  m_currentPts = LOCAL_NOPTS;
  m_nextPts = LOCAL_NOPTS;
  m_lastOutputPts = LOCAL_NOPTS;
  ResetPassthroughStartupState(LOCAL_NOPTS);
  return true;
}

void CDVDAudioCodecPassthrough::Dispose()
{
  if (m_buffer)
  {
    delete[] m_buffer;
    m_buffer = NULL;
  }

  free(m_backlogBuffer);
  m_backlogBuffer = nullptr;
  m_backlogBufferSize = 0;

  m_bufferSize = 0;
}

bool CDVDAudioCodecPassthrough::AddData(const DemuxPacket &packet)
{
  // Apply cached values (updated by settings callbacks) without per-packet settings lookups.
  m_parser.SetDefeatAC3DialNorm(m_defeatAC3DialNorm.load());
  m_parser.SetDefeatTrueHDDialNorm(m_defeatTrueHDDialNorm.load());

  if (m_backlogSize)
  {
    m_dataSize = m_bufferSize;
    unsigned int consumed = m_parser.AddData(m_backlogBuffer, m_backlogSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);
    if (consumed != m_backlogSize)
    {
      memmove(m_backlogBuffer, m_backlogBuffer+consumed, m_backlogSize-consumed);
    }
    m_backlogSize -= consumed;
  }

  unsigned char *pData(const_cast<uint8_t*>(packet.pData));
  int iSize(packet.iSize);

  // For TrueHD seamless branching: detect invalid PTS values using robust check
  double incomingPts = packet.pts;
  bool ptsIsValid = IsValidPts(incomingPts);

  if (pData)
  {
    // Sanitize PTS members if they contain garbage values (can happen during seamless branching)
    if (!IsValidPts(m_currentPts))
      m_currentPts = LOCAL_NOPTS;
    if (!IsValidPts(m_nextPts))
      m_nextPts = LOCAL_NOPTS;

    if (m_currentPts == LOCAL_NOPTS)
    {
      if (m_nextPts != LOCAL_NOPTS)
      {
        m_currentPts = m_nextPts;
        m_nextPts = ptsIsValid ? incomingPts : LOCAL_NOPTS;
      }
      else if (ptsIsValid)
      {
        m_currentPts = incomingPts;
      }
    }
    else if (ptsIsValid)
    {
      m_nextPts = incomingPts;
    }
  }

  if (pData && !m_backlogSize)
  {
    if (iSize <= 0)
      return true;

    m_dataSize = m_bufferSize;
    int used = m_parser.AddData(pData, iSize, &m_buffer, &m_dataSize);
    m_bufferSize = std::max(m_bufferSize, m_dataSize);

    if (used != iSize)
    {
      const unsigned int remaining = static_cast<unsigned int>(iSize - used);
      if (m_backlogBufferSize < remaining)
      {
        m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, remaining);
        m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
      }
      m_backlogSize = remaining;
      memcpy(m_backlogBuffer, pData + used, m_backlogSize);
    }
  }
  else if (pData)
  {
    const unsigned int newSize = m_backlogSize + static_cast<unsigned int>(iSize);
    if (m_backlogBufferSize < newSize)
    {
      m_backlogBufferSize = std::max(TRUEHD_BUF_SIZE, newSize);
      m_backlogBuffer = static_cast<uint8_t*>(realloc(m_backlogBuffer, m_backlogBufferSize));
    }
    memcpy(m_backlogBuffer + m_backlogSize, pData, iSize);
    m_backlogSize += static_cast<unsigned int>(iSize);
  }

  if (!m_dataSize)
    return true;

  m_format.m_dataFormat = AE_FMT_RAW;
  m_format.m_streamInfo = m_parser.GetStreamInfo();
  m_format.m_sampleRate = m_parser.GetSampleRate();
  m_format.m_frameSize = 1;
  CAEChannelInfo layout;
  for (unsigned int i = 0; i < m_parser.GetChannels(); i++)
  {
    layout += AE_CH_RAW;
  }
  m_format.m_channelLayout = layout;

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
  {
    if (m_deviceIsRAW) // RAW
    {
      m_dataSize = PackTrueHD();
    }
    else // IEC
    {
      // LAV-style timestamp caching: cache the PTS of the first frame being assembled into MAT
      // Since a MAT frame contains 24 TrueHD frames, we want the timestamp of the first one
      if (!m_truehd_ptsCacheValid && IsValidPts(m_currentPts))
      {
        m_truehd_ptsCache = m_currentPts;
        m_truehd_ptsCacheValid = true;
      }

      if (m_packerMAT->PackTrueHD(m_buffer, m_dataSize))
      {
        m_trueHDBuffer = m_packerMAT->GetOutputFrame();
        m_dataSize = TRUEHD_BUF_SIZE;

        // Check if MAT packer detected discontinuity (seamless branch point)
        // For TrueHD, we handle discontinuity silently to avoid receiver issues:
        // - Don't invalidate jitter baseline (keep smooth timestamps)
        // - Don't propagate discontinuity flag (avoid downstream reactions)
        // The MAT packer already handled the padding correctly (LAV-style)
        if (m_packerMAT->HadDiscontinuity())
        {
          CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough: seamless branch detected, continuing with smooth timestamps");
          // Note: NOT invalidating m_lastOutputPts to maintain smooth timing
        }

        // Use cached timestamp for this MAT frame, then reset cache for next MAT
        if (m_truehd_ptsCacheValid)
        {
          m_currentPts = m_truehd_ptsCache;
          m_truehd_ptsCacheValid = false;
          m_truehd_ptsCache = LOCAL_NOPTS;
        }
      }
      else
      {
        m_dataSize = 0;
      }
    }
  }

  return true;
}

unsigned int CDVDAudioCodecPassthrough::PackTrueHD()
{
  unsigned int dataSize{0};

  if (m_trueHDoffset == 0)
    m_trueHDframes = 0;

  memcpy(m_trueHDBuffer.data() + m_trueHDoffset, m_buffer, m_dataSize);

  m_trueHDoffset += m_dataSize;
  m_trueHDframes++;

  if (m_trueHDframes == 24)
  {
    dataSize = m_trueHDoffset;
    m_trueHDoffset = 0;
    m_trueHDframes = 0;
    return dataSize;
  }

  return 0;
}

void CDVDAudioCodecPassthrough::GetData(DVDAudioFrame &frame)
{
  frame.nb_frames = GetData(frame.data);
  frame.framesOut = 0;
  frame.hasDiscontinuity = false;
  frame.discontinuityCorrection = 0.0;

  if (frame.nb_frames == 0)
    return;

  frame.passthrough = true;
  frame.format = m_format;
  frame.planes = 1;
  frame.bits_per_sample = 8;
  frame.duration = DVD_MSEC_TO_TIME(frame.format.m_streamInfo.GetDuration());

  //============================================================================
  // LAV-style Internal Clock A/V Sync for ALL passthrough codecs
  //============================================================================
  // Based on LAV Filters by Hendrik Leppkes (Nevcairiel)
  //
  // KEY DIFFERENCE FROM PREVIOUS APPROACH:
  // - We maintain our OWN internal clock (m_internalClock)
  // - Output PTS comes from OUR clock, not demuxer
  // - Demuxer PTS is only used to detect drift
  // - This isolates us from hardware clock chaos during DV switches
  //============================================================================

  const CAEStreamInfo::DataType streamType = m_format.m_streamInfo.m_type;
  const bool isTrueHD = (streamType == CAEStreamInfo::STREAM_TYPE_TRUEHD);
  const bool isDtsHdMa = (streamType == CAEStreamInfo::STREAM_TYPE_DTSHD_MA);
  const bool isEac3 = (streamType == CAEStreamInfo::STREAM_TYPE_EAC3);

  // TrueHD-specific: Get samples offset for drift calculation (LAV-style)
  double samplesOffsetTime = 0.0;
  if (isTrueHD && m_packerMAT)
  {
    int samplesOffset = m_packerMAT->GetSamplesOffset();
    if (samplesOffset != 0)
    {
      samplesOffsetTime = static_cast<double>(samplesOffset) / m_format.m_sampleRate * DVD_TIME_BASE;
    }

    // Consume discontinuity flag from MAT packer (seamless branch detection)
    // We don't need to react to it - LAV-style packer already handled padding
    // and our internal clock continues smoothly regardless
    (void)m_packerMAT->HadDiscontinuity();
  }

  // Demuxer PTS for this frame (may be invalid during branching)
  const double demuxerPts = m_currentPts;
  const bool haveDemuxerPts = IsValidPts(demuxerPts);

  //============================================================================
  // STEP 1: Resync internal clock if needed
  //============================================================================
  // On reset/discontinuity, sync our clock to the demuxer once
  if (m_needsResync && haveDemuxerPts)
  {
    m_internalClock = demuxerPts;
    m_needsResync = false;
    m_jitterTracker.Reset();  // Clear jitter history on resync
    ResetPassthroughStartupState(LOCAL_NOPTS);

    logM(LOGDEBUG, "Internal clock synced to demuxer PTS {:.3f}s", (demuxerPts / DVD_TIME_BASE));
  }

  //============================================================================
  // STEP 2: Track jitter and correct internal clock when threshold exceeded
  //============================================================================
  // LAV Filters approach: track drift between our internal clock and demuxer PTS.
  // When drift exceeds threshold, CORRECT the internal clock to realign.
  // This handles both:
  // - Seamless branch points (large sudden jumps in demuxer PTS)
  // - Long-term drift accumulation
  //============================================================================

  if (IsValidPts(m_internalClock) && haveDemuxerPts)
  {
    if (!ShouldIgnoreWindow(m_resyncJitterHoldoff, IgnoreWindowKind::PassthroughResync))
    {
      // Jitter = clock delta + TrueHD MAT sample offset compensation.
      // Positive jitter = we're ahead of demuxer, negative = we're behind.
      const double clockDelta = m_internalClock - demuxerPts;
      const double jitter = clockDelta + samplesOffsetTime;
      const double absJitter = std::abs(jitter);
      bool startupCorrectionApplied = false;
      bool startupCorrectionPending = false;

      if (absJitter >= PASSTHROUGH_ABNORMAL_JITTER)
      {
        m_internalClock = demuxerPts;
        m_jitterTracker.Reset();
        ResetPassthroughStartupState(LOCAL_NOPTS);

        if (!isTrueHD)
        {
          frame.hasDiscontinuity = true;
          frame.discontinuityCorrection = jitter;
        }

        logM(LOGDEBUG,
             "Abnormal passthrough jitter {:.2f}ms, resyncing internal clock "
             "(samplesOffset={:.2f}ms, threshold {:.0f}ms)",
             (jitter / 1000.0), (samplesOffsetTime / 1000.0),
             (PASSTHROUGH_ABNORMAL_JITTER / 1000.0));
      }
      else
      {
        if (isDtsHdMa)
          startupCorrectionApplied = ApplyDtsHdMaStartupCorrection(jitter, frame);
        else if (isEac3)
        {
          startupCorrectionApplied = ApplyEac3StartupCorrection(jitter, frame);
          startupCorrectionPending =
              !m_startupJitterState.correctionApplied &&
              ((m_startupJitterState.warmupSamples < STARTUP_WARMUP_SAMPLES) ||
               !m_startupJitterState.tracker.IsFull());
        }
        else if (isTrueHD)
        {
          startupCorrectionApplied = ApplyTrueHdStartupCorrection(jitter, samplesOffsetTime, frame);
          startupCorrectionPending =
              !m_startupJitterState.correctionApplied &&
              ((m_startupJitterState.warmupSamples < STARTUP_WARMUP_SAMPLES) ||
               !m_startupJitterState.tracker.IsFull());
        }

        if (!startupCorrectionApplied && !startupCorrectionPending &&
            !(isTrueHD && ShouldIgnoreWindow(m_trueHdCorrectionCooldown,
                                             IgnoreWindowKind::TrueHdCorrection)))
        {
          m_jitterTracker.Sample(jitter);

          // Use AbsMinimum for correction (most stable value in the window)
          const double absMinJitter = m_jitterTracker.AbsMinimum();

          if (std::abs(absMinJitter) > m_jitterThreshold)
          {
            const double correction =
                isTrueHD ? ClampMagnitude(absMinJitter, GetTrueHdCorrectionClamp()) : absMinJitter;
            const double trackerMin = m_jitterTracker.Minimum();
            const double trackerMax = m_jitterTracker.Maximum();
            const double trackerAvg = m_jitterTracker.Average();
            const double trackerSpread = trackerMax - trackerMin;

            ApplyPassthroughJitterCorrection(correction, false, !isTrueHD, frame);

            if (isDtsHdMa)
              ResetPassthroughStartupState(LOCAL_NOPTS);
            else if (isTrueHD)
            {
              m_trueHdCorrectionCooldown.until = m_internalClock + GetTrueHdCorrectionCooldown();
              m_trueHdCorrectionCooldown.logged = false;

              logM(LOGDEBUG,
                   "TrueHD staged jitter correction {:.2f}ms of {:.2f}ms "
                   "(clock {:.2f}ms, MAT {:.2f}ms, avg {:.2f}ms, min {:.2f}ms, "
                   "max {:.2f}ms, spread {:.2f}ms, internal {:.3f}s, demux {:.3f}s, path {})",
                   (correction / 1000.0), (absMinJitter / 1000.0), (clockDelta / 1000.0),
                   (samplesOffsetTime / 1000.0), (trackerAvg / 1000.0),
                   (trackerMin / 1000.0), (trackerMax / 1000.0), (trackerSpread / 1000.0),
                   (m_internalClock / DVD_TIME_BASE), (demuxerPts / DVD_TIME_BASE),
                     GetPassthroughPathLabel());
            }
            else
            {
              logM(LOGDEBUG, "Jitter correction {:.2f}ms (threshold {:.0f}ms)",
                             (correction / 1000.0), (m_jitterThreshold / 1000.0));
            }
          }
        }
      }
    }
  }

  //============================================================================
  // STEP 3: Output from OUR internal clock (not demuxer)
  //============================================================================
  if (IsValidPts(m_internalClock))
  {
    // Output PTS is from OUR clock - this is the key LAV-style difference
    frame.pts = m_internalClock;

    // Advance our clock by frame duration for next frame
    m_internalClock += frame.duration;

    // Also track for legacy lastOutputPts (may be used elsewhere)
    m_lastOutputPts = frame.pts;
  }
  else if (haveDemuxerPts)
  {
    // Internal clock not initialized but demuxer has PTS - shouldn't happen
    // but handle gracefully by initializing now
    m_internalClock = demuxerPts;
    frame.pts = m_internalClock;
    m_internalClock += frame.duration;
    m_lastOutputPts = frame.pts;
    m_needsResync = false;
    ResetPassthroughStartupState(LOCAL_NOPTS);

    logM(LOGDEBUG, "Late init of internal clock to {:.3f}s", (frame.pts / DVD_TIME_BASE));
  }
  else
  {
    // No valid PTS anywhere - output invalid
    frame.pts = DVD_NOPTS_VALUE;
  }

  // Clear current PTS after use
  m_currentPts = LOCAL_NOPTS;
}

int CDVDAudioCodecPassthrough::GetData(uint8_t** dst)
{
  if (!m_dataSize)
    AddData(DemuxPacket());

  if (m_format.m_streamInfo.m_type == CAEStreamInfo::STREAM_TYPE_TRUEHD)
    *dst = m_trueHDBuffer.data();
  else
    *dst = m_buffer;

  int bytes = m_dataSize;
  m_dataSize = 0;
  return bytes;
}

void CDVDAudioCodecPassthrough::Reset()
{
  m_trueHDoffset = 0;
  m_trueHDframes = 0;
  m_dataSize = 0;
  m_bufferSize = 0;
  m_backlogSize = 0;
  m_currentPts = LOCAL_NOPTS;
  m_nextPts = LOCAL_NOPTS;
  m_lastOutputPts = LOCAL_NOPTS;
  m_parser.Reset();

  // Reset TrueHD-specific state
  m_truehd_ptsCache = LOCAL_NOPTS;
  m_truehd_ptsCacheValid = false;

  // Reset LAV-style internal clock - will resync on next valid PTS or RESYNC
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();
  ResetPassthroughStartupState(LOCAL_NOPTS);

  CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::Reset - Internal clock reset, will resync");

  // Reset PackerMAT state for TrueHD
  if (m_packerMAT)
    m_packerMAT->Reset();
}

void CDVDAudioCodecPassthrough::ResetLavSyncState()
{
  // Reset PTS tracking to force resync on next valid timestamp
  m_lastOutputPts = LOCAL_NOPTS;

  // Reset TrueHD timestamp cache
  m_truehd_ptsCache = LOCAL_NOPTS;
  m_truehd_ptsCacheValid = false;

  // Reset internal clock - will resync to RESYNC pts
  m_internalClock = LOCAL_NOPTS;
  m_needsResync = true;
  m_jitterTracker.Reset();
  ResetPassthroughStartupState(LOCAL_NOPTS);

  CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::ResetLavSyncState - Internal clock reset, will resync");
}

void CDVDAudioCodecPassthrough::SyncToResyncPts(double pts)
{
  // VideoPlayer::Sync() sends RESYNC with a coordinated A/V clock value.
  // We trust this value and use it directly for our internal clock.
  // This is the KEY fix for video switch A/V desync.

  constexpr double MAX_REASONABLE_PTS = 86400000000.0; // 24 hours

  if (pts != DVD_NOPTS_VALUE && pts >= 0.0 && pts <= MAX_REASONABLE_PTS)
  {
    m_internalClock = pts;
    m_needsResync = false;
    m_jitterTracker.Reset();
    ResetPassthroughStartupState(pts + PASSTHROUGH_RESYNC_JITTER_HOLDOFF);

    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::SyncToResyncPts - Internal clock set to RESYNC pts {:.3f}s",
              pts / DVD_TIME_BASE);
  }
  else
  {
    CLog::Log(LOGDEBUG, "CDVDAudioCodecPassthrough::SyncToResyncPts - Invalid pts, ignoring");
  }
}

int CDVDAudioCodecPassthrough::GetBufferSize()
{
  return (int)m_parser.GetBufferSize();
}
