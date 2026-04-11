#pragma once

#include <atomic>

#include "DVDStreamInfo.h"

class CDVDDemux;

namespace VideoPlayerProbe
{
bool Run(CDVDDemux& demuxer,
         CDVDStreamInfo& hint,
         const std::atomic<bool>& abortRequested);
}
