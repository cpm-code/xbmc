/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "VideoPlayerAudio.h"

#include "DVDCodecs/Audio/DVDAudioCodec.h"
#include "DVDCodecs/Audio/DVDAudioCodecPassthrough.h"
#include "DVDCodecs/DVDFactoryCodec.h"
#include "ServiceBroker.h"
#include "cores/DataCacheCore.h"
#include "cores/AudioEngine/Interfaces/AE.h"
#include "cores/AudioEngine/Utils/AEUtil.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "settings/Settings.h"
#include "settings/AdvancedSettings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/MathUtils.h"
#include "utils/log.h"

#include <mutex>

#ifdef TARGET_RASPBERRY_PI
#include "platform/linux/RBP.h"
#endif

#include <sstream>
#include <iomanip>
#include <math.h>

using namespace std::chrono_literals;

//==============================================================================
// LAV PTS validation utilities (same as DVDAudioCodecPassthrough.cpp)
//==============================================================================
// Sentinel for invalid PTS (avoids confusion with garbage values like DVD_NOPTS_VALUE cast to double)
constexpr double LOCAL_NOPTS = -1.0;

// Maximum reasonable PTS value (24 hours in DVD_TIME_BASE units)
constexpr double MAX_REASONABLE_PTS = 86400.0 * DVD_TIME_BASE; // 24 hours

// Check if a PTS value is valid (not sentinel and not garbage)
inline bool IsValidPts(double pts)
{
  return (pts >= 0.0) && (pts <= MAX_REASONABLE_PTS);
}

inline bool IsValidClockPts(double pts)
{
  return pts != DVD_NOPTS_VALUE && pts != LOCAL_NOPTS &&
         std::abs(pts) <= MAX_REASONABLE_PTS;
}

class CDVDMsgAudioCodecChange : public CDVDMsg
{
public:
  CDVDMsgAudioCodecChange(const CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
    : CDVDMsg(GENERAL_STREAMCHANGE), m_codec(std::move(codec)), m_hints(hints)
  {}
  ~CDVDMsgAudioCodecChange() override = default;

  std::unique_ptr<CDVDAudioCodec> m_codec;
  CDVDStreamInfo  m_hints;
};

CVideoPlayerAudio::CVideoPlayerAudio(CDVDClock* pClock,
                                     CDVDMessageQueue& parent,
                                     CRenderManager& renderManager,
                                     CProcessInfo& processInfo,
                                     double messageQueueTimeSize)
  : CThread("VideoPlayerAudio"),
    IDVDStreamPlayerAudio(processInfo),
    m_messageQueue("audio"),
    m_messageParent(parent),
    m_renderManager(renderManager),
    m_dataCacheCore(CServiceBroker::GetDataCacheCore()),
    m_audioSink(pClock)
{
  m_pClock = pClock;
  m_audioClock = 0;
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_stalled = true;
  m_paused = false;
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;
  m_synctype = SYNC_DISCON;
  m_prevsynctype = -1;
  m_prevskipped = false;
  m_maxspeedadjust = 0.0;

  // allows max bitrate of 18 Mbit/s (TrueHD max peak) during m_messageQueueTimeSize seconds
  m_messageQueue.SetMaxDataSize(18 * messageQueueTimeSize / 8 * 1024 * 1024);
  m_messageQueue.SetMaxTimeSize(messageQueueTimeSize);

  m_disconAdjustTimeMs = processInfo.GetMaxPassthroughOffSyncDuration();
}

CVideoPlayerAudio::~CVideoPlayerAudio()
{
  StopThread();

  // close the stream, and don't wait for the audio to be finished
  // CloseStream(true);
}

void CVideoPlayerAudio::SetStartupSinkConfig(const StartupSinkConfig& config)
{
  std::unique_lock lock(m_startupSinkConfigSection);
  m_startupSinkConfig = config;
  m_hasStartupSinkConfig = true;
}

void CVideoPlayerAudio::ClearStartupSinkConfig()
{
  std::unique_lock lock(m_startupSinkConfigSection);
  m_startupSinkConfig = {};
  m_hasStartupSinkConfig = false;
}

bool CVideoPlayerAudio::TakeStartupSinkConfig(StartupSinkConfig& config)
{
  std::unique_lock lock(m_startupSinkConfigSection);
  if (!m_hasStartupSinkConfig)
    return false;

  config = m_startupSinkConfig;
  m_startupSinkConfig = {};
  m_hasStartupSinkConfig = false;
  return true;
}

bool CVideoPlayerAudio::OpenStream(CDVDStreamInfo hints)
{
  CLog::Log(LOGINFO, "Finding audio codec for: {}", hints.codec);
  bool allowpassthrough = true;

  CAEStreamInfo::DataType streamType =
      m_audioSink.GetPassthroughStreamType(hints.codec, hints.samplerate, hints.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      hints, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);
  if(!codec)
  {
    CLog::Log(LOGERROR, "Unsupported audio codec");
    return false;
  }

  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgAudioCodecChange>(hints, std::move(codec)), 0);
  else
  {
    OpenStream(hints, std::move(codec));
    m_messageQueue.Init();
    CLog::Log(LOGINFO, "Creating audio thread");
    Create();
  }
  return true;
}

void CVideoPlayerAudio::OpenStream(CDVDStreamInfo& hints, std::unique_ptr<CDVDAudioCodec> codec)
{
  m_pAudioCodec = std::move(codec);


  /* store our stream hints */
  m_streaminfo = hints;

  /* update codec information from what codec gave out, if any */
  int channelsFromCodec   = m_pAudioCodec->GetFormat().m_channelLayout.Count();
  int samplerateFromCodec = m_pAudioCodec->GetFormat().m_sampleRate;

  if (channelsFromCodec > 0)
    m_streaminfo.channels = channelsFromCodec;
  if (samplerateFromCodec > 0)
    m_streaminfo.samplerate = samplerateFromCodec;

  /* check if we only just got sample rate, in which case the previous call
   * to CreateAudioCodec() couldn't have started passthrough */
  if (hints.samplerate != m_streaminfo.samplerate)
    SwitchCodecIfNeeded();

  m_audioClock = 0;
  m_stalled = m_messageQueue.GetPacketCount(CDVDMsg::DEMUXER_PACKET) == 0;

  m_prevsynctype = -1;
  m_synctype = m_processInfo.IsRealtimeStream() ? SYNC_RESAMPLE : SYNC_DISCON;

  if (m_synctype == SYNC_DISCON)
    CLog::LogF(LOGDEBUG, "Allowing max Out-Of-Sync Value of {} ms", m_disconAdjustTimeMs);

  m_prevskipped = false;

  m_maxspeedadjust = 5.0;

  StartupSinkConfig startupSinkConfig;
  if (TakeStartupSinkConfig(startupSinkConfig))
  {
    DVDAudioFrame startupFrame{};
    startupFrame.format = startupSinkConfig.format;
    startupFrame.bits_per_sample = startupSinkConfig.bitsPerSample;
    startupFrame.passthrough = startupSinkConfig.passthrough;

    if (!m_audioSink.IsValidFormat(startupFrame))
    {
      if (m_speed)
        m_audioSink.Drain();

      m_audioSink.Destroy(false);

      if (!m_audioSink.Create(startupFrame, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
        CLog::Log(LOGDEBUG,
                  "CVideoPlayerAudio::OpenStream - failed to pre-create audio renderer from probe");
    }
  }

  m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
  m_syncState = IDVDStreamPlayer::SYNC_STARTING;

  m_pcmJitterTracker.Reset();
  m_pcmOutputClock = LOCAL_NOPTS;
  m_pcmResyncTimestamp = true;
}

void CVideoPlayerAudio::CloseStream(bool bWaitForBuffers)
{
  ClearStartupSinkConfig();

  bool bWait = bWaitForBuffers && m_speed > 0 && !CServiceBroker::GetActiveAE()->IsSuspended();

  // wait until buffers are empty
  if (bWait)
    m_messageQueue.WaitUntilEmpty();

  // send abort message to the audio queue
  m_messageQueue.Abort();

  CLog::Log(LOGINFO, "Waiting for audio thread to exit");

  // shut down the adio_decode thread and wait for it
  StopThread(); // will set this->m_bStop to true

  // destroy audio device
  CLog::Log(LOGINFO, "Closing audio device");
  if (bWait)
  {
    m_bStop = false;
    m_audioSink.Drain();
    m_bStop = true;
  }
  else
  {
    m_audioSink.Flush();
  }

  m_audioSink.Destroy(true);

  // uninit queue
  m_messageQueue.End();

  CLog::Log(LOGINFO, "Deleting audio codec");
  if (m_pAudioCodec)
  {
    m_pAudioCodec->Dispose();
    m_pAudioCodec.reset();
  }

  std::ostringstream s;
  SInfo info;
  info.info        = s.str();
  info.pts         = DVD_NOPTS_VALUE;
  info.packetDelay = 0.0;
  info.passthrough = false;

  { std::unique_lock<CCriticalSection> lock(m_info_section);
    m_info = info;
  }
}

void CVideoPlayerAudio::OnStartup()
{
}

void CVideoPlayerAudio::UpdatePlayerInfo()
{
  int level, dataLevel;
  m_messageQueue.GetLevels(level, dataLevel);
  std::ostringstream s;
  s << "aq:"     << std::setw(2) << std::min(99, level) << "% (" << std::setw(2)
    << std::min(99, dataLevel) << "%)";
  s << ", Kb/s:" << std::fixed << std::setprecision(2) << m_audioStats.GetBitrate() / 1024.0;
  s << ", ac:"   << m_processInfo.GetAudioDecoderName().c_str();
  if (!m_info.passthrough)
    s << ", chan:" << m_processInfo.GetAudioChannels().c_str();
  s << ", " << m_streaminfo.samplerate/1000 << " kHz";

  // print a/v discontinuity adjustments counter when audio is not resampled (passthrough mode)
  if (m_synctype == SYNC_DISCON)
    s << ", a/v corrections (" << m_disconAdjustTimeMs << "ms): " << m_disconAdjustCounter;

  // print the inverse of the resample ratio, since that makes more sense
  // if the resample ratio is 0.5, then we're playing twice as fast
  if (m_synctype == SYNC_RESAMPLE)
    s << ", rr:" << std::fixed << std::setprecision(5) << 1.0 / m_audioSink.GetResampleRatio();

  SInfo info;
  info.info        = s.str();
  info.pts         = m_audioSink.GetPlayingPts();
  info.packetDelay = m_audioSink.GetDelay();
  info.passthrough = m_pAudioCodec && m_pAudioCodec->NeedPassthrough();

  {
    std::unique_lock lock(m_info_section);
    m_info = info;
  }

  m_dataCacheCore.SetAudioLiveBitRate(m_audioStats.GetBitrate());
  m_dataCacheCore.SetAudioQueueLevel(std::min(99, level));
  m_dataCacheCore.SetAudioQueueDataLevel(std::min(99, dataLevel));
}

void CVideoPlayerAudio::Process()
{
  CLog::Log(LOGINFO, "running thread: CVideoPlayerAudio::Process()");

  DVDAudioFrame audioframe;
  audioframe.nb_frames = 0;
  audioframe.framesOut = 0;
  audioframe.hasDiscontinuity = false;
  audioframe.discontinuityCorrection = 0.0;
  m_audioStats.Start();
  m_disconAdjustCounter = 0;

  bool onlyPrioMsgs = false;

  while (!m_bStop)
  {
    std::shared_ptr<CDVDMsg> pMsg;
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double, std::ratio<1>>(m_audioSink.GetCacheTime()));

    // read next packet and return -1 on error
    int priority = 1;
    //Do we want a new audio frame?
    if (m_syncState == IDVDStreamPlayer::SYNC_STARTING ||              /* when not started */
        m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) ||
        m_speed <  DVD_PLAYSPEED_PAUSE  || /* when rewinding */
        (m_speed >  DVD_PLAYSPEED_NORMAL && m_audioClock < m_pClock->GetClock())) /* when behind clock in ff */
      priority = 0;

    if (m_syncState == IDVDStreamPlayer::SYNC_WAITSYNC)
      priority = 1;

    if (m_paused)
      priority = 1;

    if (onlyPrioMsgs)
    {
      priority = 1;
      timeout = 0ms;
    }

    MsgQueueReturnCode ret = m_messageQueue.Get(pMsg, timeout, priority);

    onlyPrioMsgs = false;

    if (MSGQ_IS_ERROR(ret))
    {
      if (!m_messageQueue.ReceivedAbortRequest())
        CLog::Log(LOGERROR, "MSGQ_IS_ERROR returned true ({})", ret);

      break;
    }
    else if (ret == MSGQ_TIMEOUT)
    {
      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
        continue;
      }

      // if we only wanted priority messages, this isn't a stall
      if (priority)
        continue;

      if (m_processInfo.IsTempoAllowed(static_cast<float>(m_speed)/DVD_PLAYSPEED_NORMAL) &&
          !m_stalled && m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        // while AE sync is active, we still have time to fill buffers
        if (m_syncTimer.IsTimePast())
        {
          CLog::Log(LOGINFO, "CVideoPlayerAudio::Process - stream stalled");
          m_stalled = true;
        }
      }
      if (timeout == 0ms)
        CThread::Sleep(10ms);

      continue;
    }

    // handle messages
    if (pMsg->IsType(CDVDMsg::GENERAL_SYNCHRONIZE))
    {
      if (std::static_pointer_cast<CDVDMsgGeneralSynchronize>(pMsg)->Wait(100ms, SYNCSOURCE_AUDIO))
        CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_SYNCHRONIZE");
      else
        m_messageQueue.Put(pMsg, 1); // push back as prio message, to process other prio messages
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESYNC))
    { //player asked us to set internal clock
      double pts = std::static_pointer_cast<CDVDMsgDouble>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::GENERAL_RESYNC({:.3f} level: {:d} cache:{:.3f}",
                pts / DVD_TIME_BASE, m_messageQueue.GetLevel(), m_audioSink.GetDelay() / DVD_TIME_BASE);

      double delay = m_audioSink.GetDelay();
      if (!(m_pAudioCodec && m_pAudioCodec->NeedPassthrough()) &&
          (pts > m_audioClock - delay + 0.5 * DVD_TIME_BASE))
      {
        m_audioSink.Flush();
      }
      m_audioClock = pts + delay;
      if (m_speed != DVD_PLAYSPEED_PAUSE)
        m_audioSink.Resume();
      m_syncState = IDVDStreamPlayer::SYNC_INSYNC;
      m_syncTimer.Set(3000ms);

      // LAV-style: Sync passthrough codec's internal clock to RESYNC pts
      // This is the KEY fix for video switch A/V desync
      if (m_pAudioCodec && m_pAudioCodec->NeedPassthrough())
      {
        CDVDAudioCodecPassthrough* passthroughCodec =
            dynamic_cast<CDVDAudioCodecPassthrough*>(m_pAudioCodec.get());
        if (passthroughCodec)
        {
          passthroughCodec->ResetLavSyncState();
          passthroughCodec->SyncToResyncPts(pts + delay);
        }
      }
      else
      {
        // PCM: Reset output clock for RESYNC
        m_pcmOutputClock = LOCAL_NOPTS;
        m_pcmResyncTimestamp = true;
        m_pcmJitterTracker.Reset();
      }
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_RESET))
    {
      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;
      m_syncState = IDVDStreamPlayer::SYNC_STARTING;
      // Reset PCM jitter tracking on reset - will resync on next valid PTS
      m_pcmJitterTracker.Reset();
      m_pcmResyncTimestamp = true;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_FLUSH))
    {
      bool sync = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      m_audioSink.Flush();
      m_stalled = true;
      m_audioClock = 0;
      audioframe.nb_frames = 0;

      // Reset PCM jitter tracking on flush (seek, stream change) - will resync on next valid PTS
      m_pcmJitterTracker.Reset();
      m_pcmResyncTimestamp = true;

      if (sync)
      {
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        m_audioSink.Pause();
      }

      if (m_pAudioCodec)
        m_pAudioCodec->Reset();
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_EOF))
    {
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_EOF");
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_SETSPEED))
    {
      double speed = std::static_pointer_cast<CDVDMsgInt>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio - CDVDMsg::PLAYER_SETSPEED: {:f} last: {:d}", speed, m_speed);

      if (m_processInfo.IsTempoAllowed(static_cast<float>(speed)/DVD_PLAYSPEED_NORMAL))
      {
        if (speed != m_speed)
        {
          if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
          {
            m_audioSink.Resume();
            m_stalled = false;
          }
        }
      }
      else
      {
        m_audioSink.Pause();
      }
      m_speed = (int)speed;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_STREAMCHANGE))
    {
      auto msg = std::static_pointer_cast<CDVDMsgAudioCodecChange>(pMsg);
      OpenStream(msg->m_hints, std::move(msg->m_codec));
      msg->m_codec = NULL;
    }
    else if (pMsg->IsType(CDVDMsg::GENERAL_PAUSE))
    {
      m_paused = std::static_pointer_cast<CDVDMsgBool>(pMsg)->m_value;
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio - CDVDMsg::GENERAL_PAUSE: {}", m_paused);
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_REQUEST_STATE))
    {
      SStateMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.syncState = m_syncState;
      m_messageParent.Put(
          std::make_shared<CDVDMsgType<SStateMsg>>(CDVDMsg::PLAYER_REPORT_STATE, msg));
    }
    else if (pMsg->IsType(CDVDMsg::DEMUXER_PACKET))
    {
      DemuxPacket* pPacket = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacket();
      bool bPacketDrop = std::static_pointer_cast<CDVDMsgDemuxerPacket>(pMsg)->GetPacketDrop();

      if (bPacketDrop)
      {
        if (m_syncState != IDVDStreamPlayer::SYNC_STARTING)
        {
          m_audioSink.Drain();
          m_audioSink.Flush();
          audioframe.nb_frames = 0;
        }
        m_syncState = IDVDStreamPlayer::SYNC_STARTING;
        continue;
      }

      if (!m_processInfo.IsTempoAllowed(static_cast<float>(m_speed) / DVD_PLAYSPEED_NORMAL) &&
          m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
      {
        continue;
      }

      if (!m_pAudioCodec->AddData(*pPacket))
      {
        m_messageQueue.PutBack(pMsg);
        onlyPrioMsgs = true;
        continue;
      }

      m_audioStats.AddSampleBytes(pPacket->iSize);
      UpdatePlayerInfo();

      if (ProcessDecoderOutput(audioframe))
      {
        onlyPrioMsgs = true;
      }
    }
    else if (pMsg->IsType(CDVDMsg::PLAYER_DISPLAY_RESET))
    {
      m_displayReset = true;
    }
  }
}

bool CVideoPlayerAudio::ProcessDecoderOutput(DVDAudioFrame &audioframe)
{
  if (audioframe.nb_frames <= audioframe.framesOut)
  {
    audioframe.hasDownmix = false;

    m_pAudioCodec->GetData(audioframe);

    if (audioframe.nb_frames == 0)
    {
      return false;
    }

    // Initialize discontinuity fields for non-passthrough audio
    // (passthrough codec initializes these in its GetData)
    if (!audioframe.passthrough)
    {
      audioframe.hasDiscontinuity = false;
      audioframe.discontinuityCorrection = 0.0;
    }

    audioframe.hasTimestamp = true;
    const bool hasValidTimestamp =
      audioframe.passthrough ? IsValidClockPts(audioframe.pts) : IsValidPts(audioframe.pts);
    if (!hasValidTimestamp)
    {
      audioframe.pts = m_audioClock;
      audioframe.hasTimestamp = false;
    }
    else
    {
      //========================================================================
      // LAV-style Jitter Tracking for PCM/Decoded Audio
      //========================================================================
      // LAV's approach:
      // 1. Maintain a running output timestamp (m_rtStart / m_pcmOutputClock)
      // 2. On discontinuity (flush/seek), set resync flag
      // 3. When resync flag is set and we get valid input PTS, sync output clock to input
      // 4. After that, output clock runs freely via duration accumulation
      // 5. Track jitter = output_clock - input_pts, correct when threshold exceeded
      //
      // This is different from original Kodi which syncs m_audioClock to demuxer
      // PTS every frame (which defeats jitter tracking).
      //========================================================================

      if (!audioframe.passthrough)
      {
        // LAV-style resync: on first valid PTS after discontinuity, sync output clock to input
        if (m_pcmResyncTimestamp)
        {
          m_pcmOutputClock = audioframe.pts;
          m_pcmResyncTimestamp = false;
          m_pcmJitterTracker.Reset();
          CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio: PCM resync, output clock set to {:.3f}",
                    audioframe.pts / DVD_TIME_BASE);
        }
        else
        {
          // Calculate jitter: output_clock (calculated) - input_pts (from demuxer)
          double jitter = m_pcmOutputClock - audioframe.pts;

          double absJitter = std::abs(jitter);

          // Handle different jitter ranges:
          // 1. Normal jitter (<1s): Jitter tracking and correction
          // 2. Huge jitter (>1s): Likely a seek or stream change - resync to input

          if (absJitter < 1000000.0)  // Less than 1 second - normal jitter tracking
          {
            m_pcmJitterTracker.Sample(jitter);

            double absMinJitter = m_pcmJitterTracker.AbsMinimum();
            if (std::abs(absMinJitter) > PCM_JITTER_THRESHOLD)
            {
              CLog::Log(LOGDEBUG, LOGAUDIO,
                        "CVideoPlayerAudio: LAV-style PCM jitter correction, "
                        "adjusting by {:.2f}ms (absMin={:.2f}ms, avg={:.2f}ms)",
                        absMinJitter / 1000.0,
                        absMinJitter / 1000.0,
                        m_pcmJitterTracker.Average() / 1000.0);

              // Adjust output clock (like LAV's m_rtStart -= rtJitterMin)
              m_pcmOutputClock -= absMinJitter;

              // Reset jitter tracking baseline
              m_pcmJitterTracker.OffsetValues(-absMinJitter);

              // Signal discontinuity for hardware clock sync
              audioframe.hasDiscontinuity = true;
              audioframe.discontinuityCorrection = absMinJitter;
            }
          }
          else
          {
            // Huge jump (>1s) - likely seek or stream change, resync to input
            CLog::Log(LOGDEBUG, LOGAUDIO,
                      "CVideoPlayerAudio: PCM huge jitter {:.2f}s, resyncing to input",
                      jitter / DVD_TIME_BASE);
            m_pcmOutputClock = audioframe.pts;
            m_pcmJitterTracker.Reset();
          }
        }

        // Use our output clock for frame PTS (this is what gets sent downstream)
        // Note: We still update m_audioClock for other Kodi systems that use it
        audioframe.pts = m_pcmOutputClock;
      }

      // Update m_audioClock (used by other Kodi systems)
      m_audioClock = audioframe.pts;
    }

    if (audioframe.format.m_sampleRate && m_streaminfo.samplerate != (int) audioframe.format.m_sampleRate)
    {
      // The sample rate has changed or we just got it for the first time
      // for this stream. See if we should enable/disable passthrough due
      // to it.
      m_streaminfo.samplerate = audioframe.format.m_sampleRate;
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // Display reset event has occurred
    // See if we should enable passthrough
    if (m_displayReset)
    {
      if (SwitchCodecIfNeeded())
      {
        audioframe.nb_frames = 0;
        return false;
      }
    }

    // demuxer reads metatags that influence channel layout
    if (m_streaminfo.codec == AV_CODEC_ID_FLAC && m_streaminfo.channellayout)
      audioframe.format.m_channelLayout = CAEUtil::GetAEChannelLayout(m_streaminfo.channellayout);

    // we have successfully decoded an audio frame, setup renderer to match
    if (!m_audioSink.IsValidFormat(audioframe))
    {
      if (m_speed)
        m_audioSink.Drain();

      m_audioSink.Destroy(false);

      if (!m_audioSink.Create(audioframe, m_streaminfo.codec, m_synctype == SYNC_RESAMPLE))
        CLog::Log(LOGERROR, "{} - failed to create audio renderer", __FUNCTION__);

      m_prevsynctype = -1;

      if (m_syncState == IDVDStreamPlayer::SYNC_INSYNC)
        m_audioSink.Resume();
    }

    const auto videoSettings = m_processInfo.GetVideoSettings();
    m_audioSink.SetDynamicRangeCompression(
      static_cast<long>(videoSettings.m_VolumeAmplification * 100));

    SetSyncType(audioframe.passthrough);

    // downmix
    double clev = audioframe.hasDownmix ? audioframe.centerMixLevel : M_SQRT1_2;
    double curDB = 20 * log10(clev);
    audioframe.centerMixLevel = pow(10, (curDB + videoSettings.m_CenterMixLevel) / 20);
    audioframe.hasDownmix = true;
  }

  // A/V sync correction
  if (m_synctype == SYNC_DISCON)
  {
    if (audioframe.hasDiscontinuity && audioframe.discontinuityCorrection != 0)
    {
      // LAV-style jitter correction already adjusted the PTS (either in codec
      // for passthrough, or above for PCM). Now we update the master clock.
      // This ensures: 1) Jitter correction adjusts PTS first, 2) Clock syncs to match.
      const char* source = audioframe.passthrough ? "passthrough-jitter" : "pcm-jitter";
      double correction = m_pClock->ErrorAdjust(audioframe.discontinuityCorrection,
                                                 source);
      if (correction != 0)
      {
        m_audioSink.SetSyncErrorCorrection(-correction);
        m_disconAdjustCounter++;
        CLog::Log(LOGDEBUG, LOGAUDIO,
                  "CVideoPlayerAudio:: {} clock sync correction:{:.3f}ms",
                  source, correction / DVD_TIME_BASE * 1000.0);
      }
    }
    else
    {
      // No jitter correction triggered - use hardware-level sync error for ALL audio
      // This handles cases where hardware timing drifts independently of PTS
      double syncerror = m_audioSink.GetSyncError();

      if (std::abs(syncerror) > DVD_MSEC_TO_TIME(m_disconAdjustTimeMs))
      {
        double correction = m_pClock->ErrorAdjust(syncerror, "CVideoPlayerAudio::OutputPacket");
        if (correction != 0)
        {
          m_audioSink.SetSyncErrorCorrection(-correction);
          m_disconAdjustCounter++;
          CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio:: sync error correction:{:.3f}", correction / DVD_TIME_BASE);
        }
      }
    }
  }
  CLog::Log(LOGDEBUG, LOGAUDIO, "CVideoPlayerAudio::OutputPacket: pts:{:.3f} curr_pts:{:.3f} clock:{:.3f} level:{:d}",
    audioframe.pts / DVD_TIME_BASE, m_info.pts / DVD_TIME_BASE, m_pClock->GetClock() / DVD_TIME_BASE, GetLevel());

  int framesOutput = m_audioSink.AddPackets(audioframe);

  // Calculate duration actually output
  double durationOutput = audioframe.duration * ((double)framesOutput / audioframe.nb_frames);

  // Update clocks by duration output
  m_audioClock += durationOutput;

  // Also update PCM output clock for non-passthrough jitter tracking
  if (!audioframe.passthrough)
    m_pcmOutputClock += durationOutput;

  audioframe.framesOut += framesOutput;

  // signal to our parent that we have initialized
  if (m_syncState == IDVDStreamPlayer::SYNC_STARTING)
  {
    const double startupCacheTotal = m_audioSink.GetCacheTotal();
    const double startupCacheTime = m_audioSink.GetCacheTime();
    if (startupCacheTime >= startupCacheTotal * 0.75)
    {
      m_syncState = IDVDStreamPlayer::SYNC_WAITSYNC;
      m_stalled = false;
      SStartMsg msg;
      msg.player = VideoPlayer_AUDIO;
      msg.cachetotal = startupCacheTotal * DVD_TIME_BASE;
      msg.cachetime = startupCacheTime * DVD_TIME_BASE;
      msg.timestamp = audioframe.hasTimestamp ? audioframe.pts : DVD_NOPTS_VALUE;
      m_messageParent.Put(std::make_shared<CDVDMsgType<SStartMsg>>(CDVDMsg::PLAYER_STARTED, msg));

      m_streaminfo.channels = audioframe.format.m_channelLayout.Count();
      CLog::Log(LOGDEBUG, "CVideoPlayerAudio::ProcessDecoderOutput: GetAudioChannelsSink: {}",
        m_processInfo.GetAudioChannelsSink());
      m_processInfo.SetAudioChannels(audioframe.format.m_channelLayout);
      m_processInfo.SetAudioSampleRate(audioframe.format.m_sampleRate);
      int bitsPerSample = audioframe.bits_per_sample;
      if (audioframe.passthrough && audioframe.format.m_streamInfo.m_bitDepth > 0)
        bitsPerSample = audioframe.format.m_streamInfo.m_bitDepth;
      m_processInfo.SetAudioBitsPerSample(bitsPerSample);
      m_processInfo.SetAudioDecoderName(m_pAudioCodec->GetName());
      m_messageParent.Put(std::make_shared<CDVDMsg>(CDVDMsg::PLAYER_AVCHANGE));
    }
  }

  return true;
}

void CVideoPlayerAudio::SetSyncType(bool passthrough)
{
  if (passthrough && m_synctype == SYNC_RESAMPLE)
    m_synctype = SYNC_DISCON;

  //if SetMaxSpeedAdjust returns false, it means no video is played and we need to use clock feedback
  double maxspeedadjust = 0.0;
  if (m_synctype == SYNC_RESAMPLE)
    maxspeedadjust = m_maxspeedadjust;

  m_pClock->SetMaxSpeedAdjust(maxspeedadjust);

  if (m_synctype != m_prevsynctype)
  {
    const char *synctypes[] = {"clock feedback", "resample", "invalid"};
    int synctype = (m_synctype >= 0 && m_synctype <= 1) ? m_synctype : 2;
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio:: synctype set to {}: {}", m_synctype,
              synctypes[synctype]);
    m_prevsynctype = m_synctype;
    if (m_synctype == SYNC_RESAMPLE)
      m_audioSink.SetResampleMode(1);
    else
      m_audioSink.SetResampleMode(0);
  }
}

void CVideoPlayerAudio::OnExit()
{
#ifdef TARGET_WINDOWS
  CoUninitialize();
#endif

  CLog::Log(LOGINFO, "thread end: CVideoPlayerAudio::OnExit()");
}

void CVideoPlayerAudio::SetSpeed(int speed)
{
  if(m_messageQueue.IsInited())
    m_messageQueue.Put(std::make_shared<CDVDMsgInt>(CDVDMsg::PLAYER_SETSPEED, speed), 1);
  else
    m_speed = speed;
}

void CVideoPlayerAudio::Flush(bool sync)
{
  m_messageQueue.Flush();
  m_messageQueue.Put(std::make_shared<CDVDMsgBool>(CDVDMsg::GENERAL_FLUSH, sync), 1);

  m_audioSink.AbortAddPackets();
}

bool CVideoPlayerAudio::AcceptsData() const
{
  bool full = m_messageQueue.IsFull();
  return !full;
}

bool CVideoPlayerAudio::SwitchCodecIfNeeded()
{
  if (m_displayReset)
    CLog::Log(LOGINFO, "CVideoPlayerAudio: display reset occurred, checking for passthrough");
  else
    CLog::Log(LOGDEBUG, "CVideoPlayerAudio: stream props changed, checking for passthrough");

  m_displayReset = false;

  bool allowpassthrough = !CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (m_synctype == SYNC_RESAMPLE)
    allowpassthrough = false;

  CAEStreamInfo::DataType streamType = m_audioSink.GetPassthroughStreamType(
      m_streaminfo.codec, m_streaminfo.samplerate, m_streaminfo.profile);
  std::unique_ptr<CDVDAudioCodec> codec = CDVDFactoryCodec::CreateAudioCodec(
      m_streaminfo, m_processInfo, allowpassthrough, m_processInfo.AllowDTSHDDecode(), streamType);

  if (!codec || codec->NeedPassthrough() == m_pAudioCodec->NeedPassthrough())
  {
    // passthrough state has not changed
    return false;
  }

  m_pAudioCodec = std::move(codec);

  return true;
}

std::string CVideoPlayerAudio::GetPlayerInfo()
{
  std::unique_lock lock(m_info_section);
  return m_info.info;
}

int CVideoPlayerAudio::GetAudioChannels()
{
  return m_streaminfo.channels;
}

bool CVideoPlayerAudio::IsPassthrough() const
{
  std::unique_lock lock(m_info_section);
  return m_info.passthrough;
}
