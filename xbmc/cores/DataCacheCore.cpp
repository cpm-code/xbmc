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
  std::unique_lock lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.width = width;
  m_playerVideoInfo.height = height;
  m_videoWidth.store(width, std::memory_order_relaxed);
  m_videoHeight.store(height, std::memory_order_relaxed);
}

int CDataCacheCore::GetVideoWidth()
{
  return ReadSequenceGuardedValue<int>(m_videoScalarWriteSeq, [this]() {
    return m_videoWidth.load(std::memory_order_relaxed);
  });
}

int CDataCacheCore::GetVideoHeight()
{
  return ReadSequenceGuardedValue<int>(m_videoScalarWriteSeq, [this]() {
    return m_videoHeight.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoBitDepth(int bitDepth)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.bitDepth = bitDepth;
  m_videoBitDepth.store(bitDepth, std::memory_order_relaxed);
}

int CDataCacheCore::GetVideoBitDepth()
{
  return ReadSequenceGuardedValue<int>(m_videoScalarWriteSeq, [this]() {
    return m_videoBitDepth.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoHdrType(StreamHdrType hdrType)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.hdrType = hdrType;
  m_videoHdrType.store(hdrType, std::memory_order_relaxed);
}

StreamHdrType CDataCacheCore::GetVideoHdrType()
{
  return ReadSequenceGuardedValue<StreamHdrType>(m_videoScalarWriteSeq, [this]() {
    return m_videoHdrType.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoSourceHdrType(StreamHdrType hdrType)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.sourceHdrType = hdrType;
  m_videoSourceHdrType.store(hdrType, std::memory_order_relaxed);
}

StreamHdrType CDataCacheCore::GetVideoSourceHdrType()
{
  return ReadSequenceGuardedValue<StreamHdrType>(m_videoScalarWriteSeq, [this]() {
    return m_videoSourceHdrType.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoSourceAdditionalHdrType(StreamHdrType hdrType)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.sourceAdditionalHdrType = hdrType;
  m_videoSourceAdditionalHdrType.store(hdrType, std::memory_order_relaxed);
}

StreamHdrType CDataCacheCore::GetVideoSourceAdditionalHdrType()
{
  return ReadSequenceGuardedValue<StreamHdrType>(m_videoScalarWriteSeq, [this]() {
    return m_videoSourceAdditionalHdrType.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoColorSpace(AVColorSpace colorSpace)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.colorSpace = colorSpace;
  m_videoColorSpace.store(colorSpace, std::memory_order_relaxed);
}

AVColorSpace CDataCacheCore::GetVideoColorSpace()
{
  return ReadSequenceGuardedValue<AVColorSpace>(m_videoScalarWriteSeq, [this]() {
    return m_videoColorSpace.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoColorRange(AVColorRange colorRange)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.colorRange = colorRange;
  m_videoColorRange.store(colorRange, std::memory_order_relaxed);
}

AVColorRange CDataCacheCore::GetVideoColorRange()
{
  return ReadSequenceGuardedValue<AVColorRange>(m_videoScalarWriteSeq, [this]() {
    return m_videoColorRange.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoColorPrimaries(AVColorPrimaries colorPrimaries)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.colorPrimaries = colorPrimaries;
  m_videoColorPrimaries.store(colorPrimaries, std::memory_order_relaxed);
}

AVColorPrimaries CDataCacheCore::GetVideoColorPrimaries()
{
  return ReadSequenceGuardedValue<AVColorPrimaries>(m_videoScalarWriteSeq, [this]() {
    return m_videoColorPrimaries.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoColorTransferCharacteristic(AVColorTransferCharacteristic colorTransferCharacteristic)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.colorTransferCharacteristic = colorTransferCharacteristic;
  m_videoColorTransferCharacteristic.store(colorTransferCharacteristic, std::memory_order_relaxed);
}

AVColorTransferCharacteristic CDataCacheCore::GetVideoColorTransferCharacteristic()
{
  return ReadSequenceGuardedValue<AVColorTransferCharacteristic>(m_videoScalarWriteSeq, [this]() {
    return m_videoColorTransferCharacteristic.load(std::memory_order_relaxed);
  });
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
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.liveBitRate = bitRate;
  m_videoLiveBitRate.store(bitRate, std::memory_order_relaxed);
}

double CDataCacheCore::GetVideoLiveBitRate()
{
  return ReadSequenceGuardedValue<double>(m_videoScalarWriteSeq, [this]() {
    return m_videoLiveBitRate.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoQueueLevel(int level)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.queueLevel = level;
  m_videoQueueLevel.store(level, std::memory_order_relaxed);
}

int CDataCacheCore::GetVideoQueueLevel()
{
  return ReadSequenceGuardedValue<int>(m_videoScalarWriteSeq, [this]() {
    return m_videoQueueLevel.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoQueueDataLevel(int level)
{
  std::lock_guard lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.queueDataLevel = level;
  m_videoQueueDataLevel.store(level, std::memory_order_relaxed);
}

int CDataCacheCore::GetVideoQueueDataLevel()
{
  return ReadSequenceGuardedValue<int>(m_videoScalarWriteSeq, [this]() {
    return m_videoQueueDataLevel.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoFps(float fps)
{
  std::unique_lock lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.fps = fps;
  m_videoFps.store(fps, std::memory_order_relaxed);
}

float CDataCacheCore::GetVideoFps()
{
  return ReadSequenceGuardedValue<float>(m_videoScalarWriteSeq, [this]() {
    return m_videoFps.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoDAR(float dar)
{
  std::unique_lock lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);

  m_playerVideoInfo.dar = dar;
  m_videoDar.store(dar, std::memory_order_relaxed);
}

float CDataCacheCore::GetVideoDAR()
{
  return ReadSequenceGuardedValue<float>(m_videoScalarWriteSeq, [this]() {
    return m_videoDar.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetVideoInterlaced(bool isInterlaced)
{
  std::unique_lock lock(m_videoPlayerSection);
  CScopedSequenceWrite videoWriteGuard(m_videoScalarWriteSeq);
  m_playerVideoInfo.m_isInterlaced = isInterlaced;
  m_videoIsInterlaced.store(isInterlaced, std::memory_order_relaxed);
}

bool CDataCacheCore::IsVideoInterlaced()
{
  return ReadSequenceGuardedValue<bool>(m_videoScalarWriteSeq, [this]() {
    return m_videoIsInterlaced.load(std::memory_order_relaxed);
  });
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
  std::unique_lock lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);

  m_playerAudioInfo.sampleRate = sampleRate;
  m_audioSampleRate.store(sampleRate, std::memory_order_relaxed);
}

int CDataCacheCore::GetAudioSampleRate()
{
  return ReadSequenceGuardedValue<int>(m_audioScalarWriteSeq, [this]() {
    return m_audioSampleRate.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetAudioBitsPerSample(int bitsPerSample)
{
  std::unique_lock lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);

  m_playerAudioInfo.bitsPerSample = bitsPerSample;
  m_audioBitsPerSample.store(bitsPerSample, std::memory_order_relaxed);
}

int CDataCacheCore::GetAudioBitsPerSample()
{
  return ReadSequenceGuardedValue<int>(m_audioScalarWriteSeq, [this]() {
    return m_audioBitsPerSample.load(std::memory_order_relaxed);
  });
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
  std::unique_lock lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);
  m_playerAudioInfo.speakerMask = mask;
  m_audioSpeakerMask.store(mask, std::memory_order_relaxed);
}

uint64_t CDataCacheCore::GetAudioSpeakerMask()
{
  return ReadSequenceGuardedValue<uint64_t>(m_audioScalarWriteSeq, [this]() {
    return m_audioSpeakerMask.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetAudioSpeakerMaskSink(uint64_t mask)
{
  std::unique_lock lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);
  m_playerAudioInfo.speakerMaskSink = mask;
  m_audioSpeakerMaskSink.store(mask, std::memory_order_relaxed);
}

uint64_t CDataCacheCore::GetAudioSpeakerMaskSink()
{
  return ReadSequenceGuardedValue<uint64_t>(m_audioScalarWriteSeq, [this]() {
    return m_audioSpeakerMaskSink.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetAudioLiveBitRate(double bitRate)
{
  std::lock_guard lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);

  m_playerAudioInfo.liveBitRate = bitRate;
  m_audioLiveBitRate.store(bitRate, std::memory_order_relaxed);
}

double CDataCacheCore::GetAudioLiveBitRate()
{
  return ReadSequenceGuardedValue<double>(m_audioScalarWriteSeq, [this]() {
    return m_audioLiveBitRate.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetAudioQueueLevel(int level)
{
  std::lock_guard lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);

  m_playerAudioInfo.queueLevel = level;
  m_audioQueueLevel.store(level, std::memory_order_relaxed);
}

int CDataCacheCore::GetAudioQueueLevel()
{
  return ReadSequenceGuardedValue<int>(m_audioScalarWriteSeq, [this]() {
    return m_audioQueueLevel.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetAudioQueueDataLevel(int level)
{
  std::lock_guard lock(m_audioPlayerSection);
  CScopedSequenceWrite audioWriteGuard(m_audioScalarWriteSeq);

  m_playerAudioInfo.queueDataLevel = level;
  m_audioQueueDataLevel.store(level, std::memory_order_relaxed);
}

int CDataCacheCore::GetAudioQueueDataLevel()
{
  return ReadSequenceGuardedValue<int>(m_audioScalarWriteSeq, [this]() {
    return m_audioQueueDataLevel.load(std::memory_order_relaxed);
  });
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
  std::unique_lock lock(m_renderSection);
  CScopedSequenceWrite renderWriteGuard(m_renderScalarWriteSeq);

  m_renderInfo.m_isClockSync = enable;
  m_renderClockSync.store(enable, std::memory_order_relaxed);
}

bool CDataCacheCore::IsRenderClockSync()
{
  return ReadSequenceGuardedValue<bool>(m_renderScalarWriteSeq, [this]() {
    return m_renderClockSync.load(std::memory_order_relaxed);
  });
}

void CDataCacheCore::SetRenderPts(double pts)
{
  std::lock_guard lock(m_renderSection);
  CScopedSequenceWrite renderWriteGuard(m_renderScalarWriteSeq);

  m_renderInfo.pts = pts;
  m_renderPts.store(pts, std::memory_order_relaxed);
}

double CDataCacheCore::GetRenderPts()
{
  return ReadSequenceGuardedValue<double>(m_renderScalarWriteSeq, [this]() {
    return m_renderPts.load(std::memory_order_relaxed);
  });
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
  return ReadSequenceGuardedValue<float>(m_stateInfo.m_speedTempoWriteSeq, [this]() {
    return m_stateInfo.m_speed.load(std::memory_order_relaxed);
  });
}

bool CDataCacheCore::IsNormalPlayback()
{
  // Exact comparison is intentional: playback speed uses canonical constants (0.0f / 1.0f).
  return ReadSequenceGuardedValue<float>(m_stateInfo.m_speedTempoWriteSeq,
                                         [this]() {
                                           return m_stateInfo.m_speed.load(std::memory_order_relaxed);
                                         }) == 1.0f;
}

bool CDataCacheCore::IsPausedPlayback()
{
  return ReadSequenceGuardedValue<float>(m_stateInfo.m_speedTempoWriteSeq,
                                         [this]() {
                                           return m_stateInfo.m_speed.load(std::memory_order_relaxed);
                                         }) == 0.0f;
}

float CDataCacheCore::GetTempo()
{
  return ReadSequenceGuardedValue<float>(m_stateInfo.m_speedTempoWriteSeq, [this]() {
    return m_stateInfo.m_tempo.load(std::memory_order_relaxed);
  });
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
