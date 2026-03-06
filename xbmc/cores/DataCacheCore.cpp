/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DataCacheCore.h"

#include "DVDStreamInfo.h"
#include "ServiceBroker.h"
#include "cores/EdlEdit.h"
#include "cores/AudioEngine/Utils/AEStreamInfo.h"
#include "cores/AudioEngine/Utils/AEChannelInfo.h"
#include "utils/AgedMap.h"
#include "utils/BitstreamConverter.h"
#include "utils/log.h"

#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace
{
constexpr uint64_t SPEED_TEMPO_SEQ_ODD_BIT{1ULL};
constexpr unsigned int SPEED_TEMPO_READ_YIELD_FREQUENCY{64};

class CScopedSequenceWrite
{
public:
  explicit CScopedSequenceWrite(std::atomic<uint64_t>& sequence) noexcept : m_sequence(sequence)
  {
    // seq_cst keeps following stores from being reordered before write-start marker.
    // This marks the sequence as odd before protected writes become externally visible.
    m_sequence.fetch_add(1, std::memory_order_seq_cst);
  }

  ~CScopedSequenceWrite() noexcept
  {
    // release ensures protected writes become visible before write-complete marker is observed.
    m_sequence.fetch_add(1, std::memory_order_release);
  }
  CScopedSequenceWrite(const CScopedSequenceWrite&) = delete;
  CScopedSequenceWrite& operator=(const CScopedSequenceWrite&) = delete;
  CScopedSequenceWrite(CScopedSequenceWrite&&) = delete;
  CScopedSequenceWrite& operator=(CScopedSequenceWrite&&) = delete;

private:
  std::atomic<uint64_t>& m_sequence;
};

template<typename ValueType, typename ReaderFunc>
ValueType ReadSequenceGuardedValue(const std::atomic<uint64_t>& sequence, ReaderFunc&& readValue)
{
  // Read value with seqlock-style validation:
  // - sample sequence before and after callback read
  // - accept value only when sequence is stable and not write-in-progress (odd)
  // - periodically yield under contention
  static_assert(std::is_trivially_copyable_v<ValueType>,
                "ValueType must be trivially copyable for sequence-guarded reads");

  unsigned int retries{0};
  while (true)
  {
    const auto before = sequence.load(std::memory_order_acquire);
    if (!(before & SPEED_TEMPO_SEQ_ODD_BIT))
    {
      const ValueType value = readValue();
      const auto after = sequence.load(std::memory_order_acquire);
      if (before == after)
        return value;
    }

    if (++retries % SPEED_TEMPO_READ_YIELD_FREQUENCY == 0)
      std::this_thread::yield();
  }
}

template<typename ValueType>
void WriteSequenceGuardedAtomic(std::atomic<uint64_t>& sequence,
                                std::atomic<ValueType>& target,
                                ValueType value)
{
  static_assert(std::is_trivially_copyable_v<ValueType>,
                "ValueType must be trivially copyable for sequence-guarded writes");

  CScopedSequenceWrite writeGuard(sequence);
  target.store(value, std::memory_order_relaxed);
}

template<typename ValueType>
ValueType ReadSequenceGuardedAtomic(const std::atomic<uint64_t>& sequence,
                                    const std::atomic<ValueType>& source)
{
  return ReadSequenceGuardedValue<ValueType>(sequence, [&source]() {
    return source.load(std::memory_order_relaxed);
  });
}
}

CDataCacheCore::CDataCacheCore() :
  m_playerVideoInfo {},
  m_playerAudioInfo {},
  m_contentInfo {},
  m_renderInfo {},
  m_stateInfo {}
{
}

CDataCacheCore::~CDataCacheCore() = default;

CDataCacheCore& CDataCacheCore::GetInstance()
{
  return CServiceBroker::GetDataCacheCore();
}

void CDataCacheCore::Reset()
{
  {
    std::unique_lock lock(m_stateSection);
    m_stateInfo.m_stateSeeking.store(false, std::memory_order_relaxed);
    m_stateInfo.m_renderGuiLayer.store(false, std::memory_order_relaxed);
    m_stateInfo.m_renderVideoLayer.store(false, std::memory_order_relaxed);
    {
      CScopedSequenceWrite speedTempoWriteGuard(m_stateInfo.m_speedTempoWriteSeq);
      m_stateInfo.m_tempo.store(1.0f, std::memory_order_relaxed);
      m_stateInfo.m_speed.store(1.0f, std::memory_order_relaxed);
    }
    m_stateInfo.m_frameAdvance.store(false, std::memory_order_relaxed);
    m_stateInfo.m_lastSeekTime = std::chrono::time_point<std::chrono::system_clock>{};
    m_stateInfo.m_lastSeekOffset = 0;
    m_playerStateChanged = false;
  }
  {
    std::unique_lock lock(m_videoPlayerSection);
    m_playerVideoInfo = {};
    {
      CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);
      m_videoWidth.store(0, std::memory_order_relaxed);
      m_videoHeight.store(0, std::memory_order_relaxed);
      m_videoFps.store(0.0f, std::memory_order_relaxed);
      m_videoDar.store(0.0f, std::memory_order_relaxed);
      m_videoIsInterlaced.store(false, std::memory_order_relaxed);
      m_videoBitDepth.store(0, std::memory_order_relaxed);
      m_videoHdrType.store(StreamHdrType::HDR_TYPE_NONE, std::memory_order_relaxed);
      m_videoSourceHdrType.store(StreamHdrType::HDR_TYPE_NONE, std::memory_order_relaxed);
      m_videoSourceAdditionalHdrType.store(StreamHdrType::HDR_TYPE_NONE, std::memory_order_relaxed);
      m_videoColorSpace.store(AVCOL_SPC_UNSPECIFIED, std::memory_order_relaxed);
      m_videoColorRange.store(AVCOL_RANGE_UNSPECIFIED, std::memory_order_relaxed);
      m_videoColorPrimaries.store(AVCOL_PRI_UNSPECIFIED, std::memory_order_relaxed);
      m_videoColorTransferCharacteristic.store(AVCOL_TRC_UNSPECIFIED, std::memory_order_relaxed);
      m_videoLiveBitRate.store(0.0, std::memory_order_relaxed);
      m_videoQueueLevel.store(0, std::memory_order_relaxed);
      m_videoQueueDataLevel.store(0, std::memory_order_relaxed);
    }
  }
  m_hasAVInfoChanges = false;
  {
    std::unique_lock lock(m_renderSection);
    m_renderInfo = {};
    {
      CScopedSequenceWrite renderWriteGuard(m_renderScalarWriteSeq);
      m_renderClockSync.store(false, std::memory_order_relaxed);
      m_renderPts.store(0.0, std::memory_order_relaxed);
    }
  }
  {
    std::unique_lock lock(m_contentSection);
    m_contentInfo.Reset();
  }
  m_timeInfo = {};
}

void CDataCacheCore::ResetAudioCache()
{
  {
    std::unique_lock lock(m_audioPlayerSection);
    m_playerAudioInfo = {};
    {
      CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);
      m_audioSampleRate.store(0, std::memory_order_relaxed);
      m_audioBitsPerSample.store(0, std::memory_order_relaxed);
      m_audioSpeakerMask.store(0, std::memory_order_relaxed);
      m_audioSpeakerMaskSink.store(0, std::memory_order_relaxed);
      m_audioLiveBitRate.store(0.0, std::memory_order_relaxed);
      m_audioQueueLevel.store(0, std::memory_order_relaxed);
      m_audioQueueDataLevel.store(0, std::memory_order_relaxed);
    }
  }
}

bool CDataCacheCore::HasAVInfoChanges()
{
  bool ret = m_hasAVInfoChanges;
  m_hasAVInfoChanges = false;
  return ret;
}

void CDataCacheCore::SignalVideoInfoChange()
{
  m_hasAVInfoChanges = true;
}

void CDataCacheCore::SignalAudioInfoChange()
{
  m_hasAVInfoChanges = true;
}

void CDataCacheCore::SignalSubtitleInfoChange()
{
  m_hasAVInfoChanges = true;
}

void CDataCacheCore::SetAVChange(bool value)
{
  m_AVChange = value;
}

bool CDataCacheCore::GetAVChange()
{
  return m_AVChange;
}

void CDataCacheCore::SetAVChangeExtended(bool value)
{
  m_AVChangeExtended = value;
}

bool CDataCacheCore::GetAVChangeExtended()
{
  return m_AVChangeExtended;
}

void CDataCacheCore::SetVideoDecoderName(std::string name, bool isHw)
{
  std::unique_lock lock(m_videoPlayerSection);

  m_playerVideoInfo.decoderName = std::move(name);
  m_playerVideoInfo.isHwDecoder = isHw;
}

std::string CDataCacheCore::GetVideoDecoderName()
{
  std::unique_lock lock(m_videoPlayerSection);

  return m_playerVideoInfo.decoderName;
}

bool CDataCacheCore::IsVideoHwDecoder()
{
  std::unique_lock lock(m_videoPlayerSection);

  return m_playerVideoInfo.isHwDecoder;
}


void CDataCacheCore::SetVideoDeintMethod(std::string method)
{
  std::unique_lock lock(m_videoPlayerSection);

  m_playerVideoInfo.deintMethod = std::move(method);
}

std::string CDataCacheCore::GetVideoDeintMethod()
{
  std::unique_lock lock(m_videoPlayerSection);

  return m_playerVideoInfo.deintMethod;
}

void CDataCacheCore::SetVideoPixelFormat(std::string pixFormat)
{
  std::unique_lock lock(m_videoPlayerSection);

  m_playerVideoInfo.pixFormat = std::move(pixFormat);
}

std::string CDataCacheCore::GetVideoPixelFormat()
{
  std::unique_lock lock(m_videoPlayerSection);

  return m_playerVideoInfo.pixFormat;
}

void CDataCacheCore::SetVideoStereoMode(std::string mode)
{
  std::unique_lock lock(m_videoPlayerSection);

  m_playerVideoInfo.stereoMode = std::move(mode);
}

std::string CDataCacheCore::GetVideoStereoMode()
{
  std::unique_lock lock(m_videoPlayerSection);

  return m_playerVideoInfo.stereoMode;
}

void CDataCacheCore::SetVideoDimensions(int width, int height)
{
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_videoWidth.store(width, std::memory_order_relaxed);
  m_videoHeight.store(height, std::memory_order_relaxed);
}

int CDataCacheCore::GetVideoWidth()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoWidth);
}

int CDataCacheCore::GetVideoHeight()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoHeight);
}

void CDataCacheCore::SetVideoBitDepth(int bitDepth)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoBitDepth, bitDepth);
}

int CDataCacheCore::GetVideoBitDepth()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoBitDepth);
}

void CDataCacheCore::SetVideoHdrType(StreamHdrType hdrType)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoHdrType, hdrType);
}

StreamHdrType CDataCacheCore::GetVideoHdrType()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoHdrType);
}

void CDataCacheCore::SetVideoSourceHdrType(StreamHdrType hdrType)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoSourceHdrType, hdrType);
}

StreamHdrType CDataCacheCore::GetVideoSourceHdrType()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoSourceHdrType);
}

void CDataCacheCore::SetVideoSourceAdditionalHdrType(StreamHdrType hdrType)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoSourceAdditionalHdrType, hdrType);
}

StreamHdrType CDataCacheCore::GetVideoSourceAdditionalHdrType()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoSourceAdditionalHdrType);
}

void CDataCacheCore::SetVideoColorSpace(AVColorSpace colorSpace)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorSpace, colorSpace);
}

AVColorSpace CDataCacheCore::GetVideoColorSpace()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorSpace);
}

void CDataCacheCore::SetVideoColorRange(AVColorRange colorRange)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorRange, colorRange);
}

AVColorRange CDataCacheCore::GetVideoColorRange()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorRange);
}

void CDataCacheCore::SetVideoColorPrimaries(AVColorPrimaries colorPrimaries)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorPrimaries, colorPrimaries);
}

AVColorPrimaries CDataCacheCore::GetVideoColorPrimaries()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorPrimaries);
}

void CDataCacheCore::SetVideoColorTransferCharacteristic(AVColorTransferCharacteristic colorTransferCharacteristic)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorTransferCharacteristic,
                             colorTransferCharacteristic);
}

AVColorTransferCharacteristic CDataCacheCore::GetVideoColorTransferCharacteristic()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoColorTransferCharacteristic);
}

void CDataCacheCore::SetVideoDoViFrameMetadata(DOVIFrameMetadata value)
{
  std::lock_guard lock(m_videoPlayerSection);

  uint64_t pts = value.pts;
  logM(LOGDEBUG, "CDataCacheCore", "Set meta for pts [{}] [{}]", pts, value.level1_max_pq);
  m_playerVideoInfo.doviFrameMetadataMap.insert(pts, std::move(value));
}

DOVIFrameMetadata CDataCacheCore::GetVideoDoViFrameMetadata()
{
  std::lock_guard lock(m_videoPlayerSection);

  uint64_t pts = GetRenderPts();
  auto doviFrameMetadata = m_playerVideoInfo.doviFrameMetadataMap.findOrLatest(pts);
  if (doviFrameMetadata != m_playerVideoInfo.doviFrameMetadataMap.end())
  {
    logM(LOGDEBUG, "CDataCacheCore", "Get meta for pts [{}] [{}] (matched pts [{}])",
                                     pts, doviFrameMetadata->second.level1_max_pq, doviFrameMetadata->first);
    return doviFrameMetadata->second;
  }
  return {};
}

void CDataCacheCore::SetVideoDoViStreamMetadata(DOVIStreamMetadata value)
{
  std::lock_guard lock(m_videoPlayerSection);

  m_playerVideoInfo.doviStreamMetadata = std::move(value);
}

DOVIStreamMetadata CDataCacheCore::GetVideoDoViStreamMetadata()
{
  std::lock_guard lock(m_videoPlayerSection);

  return m_playerVideoInfo.doviStreamMetadata;
}

void CDataCacheCore::SetVideoDoViStreamInfo(DOVIStreamInfo value)
{
  std::lock_guard lock(m_videoPlayerSection);

  m_playerVideoInfo.doviStreamInfo = std::move(value);
}

DOVIStreamInfo CDataCacheCore::GetVideoDoViStreamInfo()
{
  std::lock_guard lock(m_videoPlayerSection);

  return m_playerVideoInfo.doviStreamInfo;
}

void CDataCacheCore::SetVideoSourceDoViStreamInfo(DOVIStreamInfo value)
{
  std::lock_guard lock(m_videoPlayerSection);

  m_playerVideoInfo.sourceDoViStreamInfo = std::move(value);
}

DOVIStreamInfo CDataCacheCore::GetVideoSourceDoViStreamInfo()
{
  std::lock_guard lock(m_videoPlayerSection);

  return m_playerVideoInfo.sourceDoViStreamInfo;
}

void CDataCacheCore::SetVideoDoViCodecFourCC(std::string codecFourCC)
{
  std::lock_guard lock(m_videoPlayerSection);

  m_playerVideoInfo.doviCodecFourCC = std::move(codecFourCC);
}

std::string CDataCacheCore::GetVideoDoViCodecFourCC()
{
  std::lock_guard lock(m_videoPlayerSection);

  return m_playerVideoInfo.doviCodecFourCC;
}

void CDataCacheCore::SetVideoHDRStaticMetadataInfo(HDRStaticMetadataInfo value)
{
  std::lock_guard lock(m_videoPlayerSection);

  m_playerVideoInfo.hdrStaticMetadataInfo = std::move(value);
}

HDRStaticMetadataInfo CDataCacheCore::GetVideoHDRStaticMetadataInfo()
{
  std::lock_guard lock(m_videoPlayerSection);

  return m_playerVideoInfo.hdrStaticMetadataInfo;
}

void CDataCacheCore::SetVideoLiveBitRate(double bitRate)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoLiveBitRate, bitRate);
}

double CDataCacheCore::GetVideoLiveBitRate()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoLiveBitRate);
}

void CDataCacheCore::SetVideoQueueLevel(int level)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoQueueLevel, level);
}

int CDataCacheCore::GetVideoQueueLevel()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoQueueLevel);
}

void CDataCacheCore::SetVideoQueueDataLevel(int level)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoQueueDataLevel, level);
}

int CDataCacheCore::GetVideoQueueDataLevel()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoQueueDataLevel);
}

void CDataCacheCore::SetVideoFps(float fps)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoFps, fps);
}

float CDataCacheCore::GetVideoFps()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoFps);
}

void CDataCacheCore::SetVideoDAR(float dar)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoDar, dar);
}

float CDataCacheCore::GetVideoDAR()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoDar);
}

void CDataCacheCore::SetVideoInterlaced(bool isInterlaced)
{
  WriteSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoIsInterlaced, isInterlaced);
}

bool CDataCacheCore::IsVideoInterlaced()
{
  return ReadSequenceGuardedAtomic(m_videoScalarWriteSeq, m_videoIsInterlaced);
}

// player audio info
void CDataCacheCore::SetAudioDecoderName(std::string name)
{
  std::unique_lock lock(m_audioPlayerSection);

  m_playerAudioInfo.decoderName = std::move(name);
}

std::string CDataCacheCore::GetAudioDecoderName()
{
  std::unique_lock lock(m_audioPlayerSection);

  return m_playerAudioInfo.decoderName;
}

void CDataCacheCore::SetAudioChannels(std::string channels)
{
  std::unique_lock lock(m_audioPlayerSection);

  m_playerAudioInfo.channels = std::move(channels);
}

void CDataCacheCore::SetAudioChannelsSink(std::string channels)
{
  std::unique_lock<CCriticalSection> lock(m_audioPlayerSection);

  m_playerAudioInfo.channels_sink = std::move(channels);
}

std::string CDataCacheCore::GetAudioChannels()
{
  std::unique_lock lock(m_audioPlayerSection);

  return m_playerAudioInfo.channels;
}

std::string CDataCacheCore::GetAudioChannelsSink()
{
  std::unique_lock<CCriticalSection> lock(m_audioPlayerSection);

  return m_playerAudioInfo.channels_sink;
}

void CDataCacheCore::SetAudioSampleRate(int sampleRate)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioSampleRate, sampleRate);
}

int CDataCacheCore::GetAudioSampleRate()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioSampleRate);
}

void CDataCacheCore::SetAudioBitsPerSample(int bitsPerSample)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioBitsPerSample, bitsPerSample);
}

int CDataCacheCore::GetAudioBitsPerSample()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioBitsPerSample);
}

uint64_t CDataCacheCore::MakeSpeakerMask(const CAEChannelInfo& channels)
{
  uint64_t mask = 0;

  // Passthrough/RAW layouts don't carry speaker positions. Provide a sane default
  // "bed" mapping based on channel count so skins can still light speakers.
  if (channels.HasChannel(AE_CH_RAW))
  {
    switch (channels.Count())
    {
      case 1:
        mask |= (1ULL << 2); // FC
        break;
      case 2:
        mask |= (1ULL << 0) | (1ULL << 1); // FL/FR
        break;
      case 3:
        mask |= (1ULL << 0) | (1ULL << 1) | (1ULL << 2); // FL/FR/FC
        break;
      case 4:
        mask |= (1ULL << 0) | (1ULL << 1) | (1ULL << 4) | (1ULL << 5); // FL/FR/SL/SR
        break;
      case 5:
        mask |= (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 4) | (1ULL << 5); // 5.0
        break;
      case 6:
        mask |= (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) | (1ULL << 5); // 5.1
        break;
      default:
        if (channels.Count() >= 8)
        {
          mask |= (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3) | (1ULL << 4) |
                  (1ULL << 5) | (1ULL << 6) | (1ULL << 7); // 7.1
        }
        break;
    }

    return mask;
  }

  for (unsigned int i = 0; i < channels.Count(); ++i)
  {
    switch (channels[i])
    {
      case AE_CH_FL: mask |= (1ULL << 0); break;
      case AE_CH_FR: mask |= (1ULL << 1); break;
      case AE_CH_FC: mask |= (1ULL << 2); break;
      case AE_CH_LFE: mask |= (1ULL << 3); break;
      case AE_CH_SL: mask |= (1ULL << 4); break;
      case AE_CH_SR: mask |= (1ULL << 5); break;
      case AE_CH_BL: mask |= (1ULL << 6); break;
      case AE_CH_BR: mask |= (1ULL << 7); break;
      case AE_CH_BC: mask |= (1ULL << 8); break;
      case AE_CH_FLOC: mask |= (1ULL << 9); break;
      case AE_CH_FROC: mask |= (1ULL << 10); break;
      case AE_CH_TFL: mask |= (1ULL << 11); break;
      case AE_CH_TFR: mask |= (1ULL << 12); break;
      case AE_CH_TFC: mask |= (1ULL << 13); break;
      case AE_CH_TC: mask |= (1ULL << 14); break;
      case AE_CH_TBL: mask |= (1ULL << 15); break;
      case AE_CH_TBR: mask |= (1ULL << 16); break;
      case AE_CH_TBC: mask |= (1ULL << 17); break;
      case AE_CH_BLOC: mask |= (1ULL << 18); break;
      case AE_CH_BROC: mask |= (1ULL << 19); break;
      default:
        break;
    }
  }

  return mask;
}

void CDataCacheCore::SetAudioSpeakerMask(uint64_t mask)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioSpeakerMask, mask);
}

uint64_t CDataCacheCore::GetAudioSpeakerMask()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioSpeakerMask);
}

void CDataCacheCore::SetAudioSpeakerMaskSink(uint64_t mask)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioSpeakerMaskSink, mask);
}

uint64_t CDataCacheCore::GetAudioSpeakerMaskSink()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioSpeakerMaskSink);
}

void CDataCacheCore::SetAudioLiveBitRate(double bitRate)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioLiveBitRate, bitRate);
}

double CDataCacheCore::GetAudioLiveBitRate()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioLiveBitRate);
}

void CDataCacheCore::SetAudioQueueLevel(int level)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioQueueLevel, level);
}

int CDataCacheCore::GetAudioQueueLevel()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioQueueLevel);
}

void CDataCacheCore::SetAudioQueueDataLevel(int level)
{
  WriteSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioQueueDataLevel, level);
}

int CDataCacheCore::GetAudioQueueDataLevel()
{
  return ReadSequenceGuardedAtomic(m_audioScalarWriteSeq, m_audioQueueDataLevel);
}

void CDataCacheCore::SetEditList(const std::vector<EDL::Edit>& editList)
{
  std::unique_lock lock(m_contentSection);
  m_contentInfo.SetEditList(editList);
}

const std::vector<EDL::Edit>& CDataCacheCore::GetEditList() const
{
  std::unique_lock lock(m_contentSection);
  return m_contentInfo.GetEditList();
}

void CDataCacheCore::SetCuts(const std::vector<std::chrono::milliseconds>& cuts)
{
  std::unique_lock lock(m_contentSection);
  m_contentInfo.SetCuts(cuts);
}

const std::vector<std::chrono::milliseconds>& CDataCacheCore::GetCuts() const
{
  std::unique_lock lock(m_contentSection);
  return m_contentInfo.GetCuts();
}

void CDataCacheCore::SetSceneMarkers(const std::vector<std::chrono::milliseconds>& sceneMarkers)
{
  std::unique_lock lock(m_contentSection);
  m_contentInfo.SetSceneMarkers(sceneMarkers);
}

const std::vector<std::chrono::milliseconds>& CDataCacheCore::GetSceneMarkers() const
{
  std::unique_lock lock(m_contentSection);
  return m_contentInfo.GetSceneMarkers();
}

void CDataCacheCore::SetChapters(const std::vector<std::pair<std::string, int64_t>>& chapters)
{
  std::unique_lock lock(m_contentSection);
  m_contentInfo.SetChapters(chapters);
}

const std::vector<std::pair<std::string, int64_t>>& CDataCacheCore::GetChapters() const
{
  std::unique_lock lock(m_contentSection);
  return m_contentInfo.GetChapters();
}

void CDataCacheCore::SetRenderClockSync(bool enable)
{
  WriteSequenceGuardedAtomic(m_renderScalarWriteSeq, m_renderClockSync, enable);
}

bool CDataCacheCore::IsRenderClockSync()
{
  return ReadSequenceGuardedAtomic(m_renderScalarWriteSeq, m_renderClockSync);
}

void CDataCacheCore::SetRenderPts(double pts)
{
  WriteSequenceGuardedAtomic(m_renderScalarWriteSeq, m_renderPts, pts);
}

double CDataCacheCore::GetRenderPts()
{
  return ReadSequenceGuardedAtomic(m_renderScalarWriteSeq, m_renderPts);
}

// player states
void CDataCacheCore::SeekFinished(int64_t offset)
{
  std::unique_lock lock(m_stateSection);
  m_stateInfo.m_lastSeekTime = std::chrono::system_clock::now();
  m_stateInfo.m_lastSeekOffset = offset;
}

int64_t CDataCacheCore::GetSeekOffSet() const
{
  std::unique_lock lock(m_stateSection);
  return m_stateInfo.m_lastSeekOffset;
}

bool CDataCacheCore::HasPerformedSeek(int64_t lastSecondInterval) const
{
  std::unique_lock lock(m_stateSection);
  if (m_stateInfo.m_lastSeekTime == std::chrono::time_point<std::chrono::system_clock>{})
  {
    return false;
  }
  return (std::chrono::system_clock::now() - m_stateInfo.m_lastSeekTime) <
         std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::duration<int64_t>(lastSecondInterval));
}

void CDataCacheCore::SetStateSeeking(bool active)
{
  std::unique_lock lock(m_stateSection);

  m_stateInfo.m_stateSeeking.store(active, std::memory_order_relaxed);
  m_playerStateChanged = true;
}

bool CDataCacheCore::IsSeeking()
{
  return m_stateInfo.m_stateSeeking.load(std::memory_order_relaxed);
}

void CDataCacheCore::SetSpeed(float tempo, float speed)
{
  std::unique_lock lock(m_stateSection);

  CScopedSequenceWrite speedTempoWriteGuard(m_stateInfo.m_speedTempoWriteSeq);
  m_stateInfo.m_tempo.store(tempo, std::memory_order_relaxed);
  m_stateInfo.m_speed.store(speed, std::memory_order_relaxed);
}

float CDataCacheCore::GetSpeed()
{
  return ReadSequenceGuardedAtomic(m_stateInfo.m_speedTempoWriteSeq, m_stateInfo.m_speed);
}

bool CDataCacheCore::IsNormalPlayback()
{
  // Exact comparison is intentional: playback speed uses canonical constants (0.0f / 1.0f).
  return ReadSequenceGuardedAtomic(m_stateInfo.m_speedTempoWriteSeq, m_stateInfo.m_speed) == 1.0f;
}

bool CDataCacheCore::IsPausedPlayback()
{
  return ReadSequenceGuardedAtomic(m_stateInfo.m_speedTempoWriteSeq, m_stateInfo.m_speed) == 0.0f;
}

float CDataCacheCore::GetTempo()
{
  return ReadSequenceGuardedAtomic(m_stateInfo.m_speedTempoWriteSeq, m_stateInfo.m_tempo);
}

void CDataCacheCore::SetFrameAdvance(bool fa)
{
  m_stateInfo.m_frameAdvance.store(fa, std::memory_order_relaxed);
}

bool CDataCacheCore::IsFrameAdvance()
{
  return m_stateInfo.m_frameAdvance.load(std::memory_order_relaxed);
}

bool CDataCacheCore::IsPlayerStateChanged()
{
  std::unique_lock lock(m_stateSection);

  bool ret(m_playerStateChanged);
  m_playerStateChanged = false;

  return ret;
}

void CDataCacheCore::SetGuiRender(bool gui)
{
  std::unique_lock lock(m_stateSection);

  m_stateInfo.m_renderGuiLayer.store(gui, std::memory_order_relaxed);
  m_playerStateChanged = true;
}

bool CDataCacheCore::GetGuiRender()
{
  return m_stateInfo.m_renderGuiLayer.load(std::memory_order_relaxed);
}

void CDataCacheCore::SetVideoRender(bool video)
{
  std::unique_lock lock(m_stateSection);

  m_stateInfo.m_renderVideoLayer.store(video, std::memory_order_relaxed);
  m_playerStateChanged = true;
}

bool CDataCacheCore::GetVideoRender()
{
  return m_stateInfo.m_renderVideoLayer.load(std::memory_order_relaxed);
}

void CDataCacheCore::SetPlayTimes(time_t start, int64_t current, int64_t min, int64_t max)
{
  std::unique_lock lock(m_stateSection);
  m_timeInfo.m_startTime = start;
  m_timeInfo.m_time = current;
  m_timeInfo.m_timeMin = min;
  m_timeInfo.m_timeMax = max;
}

void CDataCacheCore::GetPlayTimes(time_t &start, int64_t &current, int64_t &min, int64_t &max)
{
  std::unique_lock lock(m_stateSection);
  start = m_timeInfo.m_startTime;
  current = m_timeInfo.m_time;
  min = m_timeInfo.m_timeMin;
  max = m_timeInfo.m_timeMax;
}

time_t CDataCacheCore::GetStartTime()
{
  std::unique_lock lock(m_stateSection);
  return m_timeInfo.m_startTime;
}

int64_t CDataCacheCore::GetPlayTime()
{
  std::unique_lock lock(m_stateSection);
  return m_timeInfo.m_time;
}

int64_t CDataCacheCore::GetMinTime()
{
  std::unique_lock lock(m_stateSection);
  return m_timeInfo.m_timeMin;
}

int64_t CDataCacheCore::GetMaxTime()
{
  std::unique_lock lock(m_stateSection);
  return m_timeInfo.m_timeMax;
}

float CDataCacheCore::GetPlayPercentage()
{
  std::unique_lock lock(m_stateSection);

  // Note: To calculate accurate percentage, all time data must be consistent,
  //       which is the case for data cache core. Calculation can not be done
  //       outside of data cache core or a possibility to lock the data cache
  //       core from outside would be needed.
  int64_t iTotalTime = m_timeInfo.m_timeMax - m_timeInfo.m_timeMin;
  if (iTotalTime <= 0)
    return 0;

  return m_timeInfo.m_time * 100 / static_cast<float>(iTotalTime);
}
