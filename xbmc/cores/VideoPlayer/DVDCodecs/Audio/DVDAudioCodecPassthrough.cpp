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
}

void CDVDAudioCodecPassthrough::ResetDtsHdMaStartupJitter()
{
  m_dtsHdMaStartupJitter.Reset();
  m_dtsHdMaStartupWarmupSamples = 0;
  m_dtsHdMaStartupCorrectionApplied = false;
  m_dtsHdMaStartupRejectionLogged = false;
}

double CDVDAudioCodecPassthrough::EvaluateDtsHdMaStartupCorrection(double jitter)
{
  if (m_dtsHdMaStartupCorrectionApplied) return 0.0;

  if (m_dtsHdMaStartupWarmupSamples < DTSHD_MA_STARTUP_WARMUP_SAMPLES)
  {
    m_dtsHdMaStartupWarmupSamples++;
    return 0.0;
  }

  m_dtsHdMaStartupJitter.Sample(jitter);
  if (!m_dtsHdMaStartupJitter.IsFull()) return 0.0;

  const double minJitter = m_dtsHdMaStartupJitter.Minimum();
  const double maxJitter = m_dtsHdMaStartupJitter.Maximum();
  const double averageJitter = m_dtsHdMaStartupJitter.Average();
  const double spread = maxJitter - minJitter;
  const bool consistentSign = (minJitter > 0.0 && maxJitter > 0.0) ||
                              (minJitter < 0.0 && maxJitter < 0.0);
  const bool minCorrectionOk = std::abs(averageJitter) >= DTSHD_MA_STARTUP_MIN_CORRECTION;
  const bool maxCorrectionOk = std::abs(averageJitter) <= DTSHD_MA_STARTUP_MAX_CORRECTION;
  const bool spreadOk = spread <= DTSHD_MA_STARTUP_MAX_SPREAD;

  if (!consistentSign || !spreadOk || !minCorrectionOk || !maxCorrectionOk)
  {
    if (!m_dtsHdMaStartupRejectionLogged)
    {
      m_dtsHdMaStartupRejectionLogged = true;

      logM(LOGDEBUG, "DTS-HD MA startup baseline rejected "
                     "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms, sign:{:d}, minOk:{:d}, maxOk:{:d}, spreadOk:{:d})",
                     (averageJitter / 1000.0), (minJitter / 1000.0), (maxJitter / 1000.0),
                     (spread / 1000.0), consistentSign ? 1 : 0, minCorrectionOk ? 1 : 0,
                     maxCorrectionOk ? 1 : 0, spreadOk ? 1 : 0);
    }

    return 0.0;
  }

  m_dtsHdMaStartupCorrectionApplied = true;

  logM(LOGDEBUG, "DTS-HD MA startup baseline correction {:.2f}ms "
                 "(avg {:.2f}ms, min {:.2f}ms, max {:.2f}ms, spread {:.2f}ms)",
                 (averageJitter / 1000.0), (averageJitter / 1000.0), (minJitter / 1000.0),
                 (maxJitter / 1000.0), (spread / 1000.0));

  return averageJitter;
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
      // LAV Filters: TrueHD/DTS use 10x threshold (1 second) for bitstreaming tolerance
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD:
      m_codecName = "pt-dtshd_hra";
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_DTSHD_CORE:
      m_codecName = "pt-dts";
      m_parser.SetCoreOnly(true);
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
      break;

    case CAEStreamInfo::STREAM_TYPE_TRUEHD:
      m_codecName = "pt-truehd";
      // LAV Filters: TrueHD/DTS use 10x threshold (1 second) for bitstreaming tolerance
      m_jitterThreshold = JITTER_THRESHOLD_TRUEHD_DTS;
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
  ResetDtsHdMaStartupJitter();
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
    ResetDtsHdMaStartupJitter();

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
    // Jitter = our_clock - demuxer_pts (+ samplesOffset for TrueHD MAT compensation)
    // Positive jitter = we're ahead of demuxer, negative = we're behind
    double jitter = m_internalClock - demuxerPts + samplesOffsetTime;
    bool startupCorrectionApplied = false;

    if (isDtsHdMa)
    {
      const double startupCorrection = EvaluateDtsHdMaStartupCorrection(jitter);
      if (startupCorrection != 0.0)
      {
        frame.hasDiscontinuity = true;
        frame.discontinuityCorrection = startupCorrection;
        m_internalClock -= startupCorrection;
        m_jitterTracker.Reset();
        startupCorrectionApplied = true;
      }
    }

    if (!startupCorrectionApplied)
    {
      m_jitterTracker.Sample(jitter);

      // Use AbsMinimum for correction (most stable value in the window)
      double absMinJitter = m_jitterTracker.AbsMinimum();

      if (std::abs(absMinJitter) > m_jitterThreshold)
      {
        // Correct internal clock by the jitter amount (like LAV Filters)
        m_internalClock -= absMinJitter;
        m_jitterTracker.OffsetValues(-absMinJitter);

        if (!isTrueHD)
        {
          frame.hasDiscontinuity = true;
          frame.discontinuityCorrection = absMinJitter;
        }

        if (isDtsHdMa)
          ResetDtsHdMaStartupJitter();

        logM(LOGDEBUG, "Jitter correction {:.2f}ms (threshold {:.0f}ms)",
                       (absMinJitter / 1000.0), (m_jitterThreshold / 1000.0));

        if (isTrueHD)
        {
          logM(LOGDEBUG, "TrueHD jitter details corr={:.2f}ms "
                         "jitter={:.2f}ms samplesOffset={:.2f}ms internal={:.3f}s demux={:.3f}s",
                         (absMinJitter / 1000.0), (jitter / 1000.0), (samplesOffsetTime / 1000.0),
                         (m_internalClock / DVD_TIME_BASE), (demuxerPts / DVD_TIME_BASE));
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
    ResetDtsHdMaStartupJitter();

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
  ResetDtsHdMaStartupJitter();

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
  ResetDtsHdMaStartupJitter();

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
    ResetDtsHdMaStartupJitter();

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
