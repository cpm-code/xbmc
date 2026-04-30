/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/AudioEngine/Utils/AEAudioFormat.h"

#include <atomic>

class CDVDDemux;
class CDVDStreamInfo;
class CProcessInfo;

namespace VideoPlayerProbeAudio
{
struct AudioProbeResult
{
  AEAudioFormat format;
  int bitsPerSample{0};
  bool passthrough{false};
};

bool Run(CDVDDemux& demuxer,
         CDVDStreamInfo& hint,
         CProcessInfo& processInfo,
         const std::atomic<bool>& abortRequested,
         AudioProbeResult& result);
}
