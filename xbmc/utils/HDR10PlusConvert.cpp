/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/log.h"

#include <algorithm> 
#include <vector>
#include <cmath>
#include <cstdint>

#include "HDR10PlusConvert.h"

#include "HDR10PlusWriter.h"
#include "HDR10Plus.h"

// Nits to PQ
constexpr double ST2084_Y_MAX = 10000.0;
constexpr double ST2084_M1 = 2610.0 / 16384.0;
constexpr double ST2084_M2 = (2523.0 / 4096.0) * 128.0;
constexpr double ST2084_C1 = 3424.0 / 4096.0;
constexpr double ST2084_C2 = (2413.0 / 4096.0) * 32.0;
constexpr double ST2084_C3 = (2392.0 / 4096.0) * 32.0;

// Clamp Values
constexpr std::uint16_t L1_MAX_PQ_MIN_VALUE = 2081;
constexpr std::uint16_t L1_MAX_PQ_MAX_VALUE = 4095;
constexpr std::uint16_t L1_AVG_PQ_MIN_VALUE = 819;

static double nits_to_pq(double nits) {
    double y = nits / ST2084_Y_MAX;
    return std::pow((ST2084_C1 + ST2084_C2 * std::pow(y, ST2084_M1)) / (1.0 + ST2084_C3 * std::pow(y, ST2084_M1)), ST2084_M2);
}

static double peak_brightness_nits(const Hdr10PlusMetadata& meta, const PeakBrightnessSource& source) {

  if (meta.num_windows == 0) return 0;

  switch (source) {
    
    case PeakBrightnessSource::Histogram: {

      const auto& distributions = meta.luminance[0].distribution_maxrgb;
      if (distributions.empty()) return 0;

      auto max_value_it = std::max_element(distributions.begin(), distributions.end(),
                                  [](const auto& a, const auto& b) {
                                      return a.percentile < b.percentile;
                                  });
      const auto& max_value = *max_value_it;
      return static_cast<double>(max_value.percentile) / 10.0; 
    }

    case PeakBrightnessSource::Histogram99: {

      const auto& distributions = meta.luminance[0].distribution_maxrgb;
      if (distributions.empty()) return 0;

      return static_cast<double>(distributions.back().percentile) / 10.0;
    }

    case PeakBrightnessSource::MaxScl: {

      const auto& max_scl = meta.luminance[0].maxscl;
      auto max_value = *std::max_element(max_scl, max_scl + sizeof(max_scl) / sizeof(max_scl[0]));
      return static_cast<double>(max_value) / 10.0;
    }

    case PeakBrightnessSource::MaxSclLuminance: {

      const auto& max_scl = meta.luminance[0].maxscl;
      double r = static_cast<double>(max_scl[0]);
      double g = static_cast<double>(max_scl[1]);
      double b = static_cast<double>(max_scl[2]);
      double luminance = (0.2627 * r) + (0.678 * g) + (0.0593 * b);
      return luminance / 10.0;

    }
  }

  CLog::Log(LOGINFO, "CBitstreamConverter::peak_brightness_nits 6");

  return 0;
}

static uint16_t clamp16(uint16_t d, uint16_t min, uint16_t max) {
  uint16_t t = d < min ? min : d;
  return t > max ? max : t;
}

static std::vector<uint8_t> last_rpu;
static VdrDmData last_vdr_dm_data = {};

std::vector<uint8_t> create_rpu_nalu_for_hdr10plus(
  const Hdr10PlusMetadata& meta,
  const PeakBrightnessSource peak_source,
  const uint16_t max_display_mastering_luminance,
  const uint16_t min_display_mastering_luminance,
  const uint16_t max_content_light_level,
  const uint16_t max_frame_average_light_level) 
{

  double avg_nits = 0;

  if (meta.num_windows > 0) avg_nits = static_cast<double>(meta.luminance[0].average_maxrgb) / 10.0;

  double max_nits = peak_brightness_nits(meta, peak_source);

  uint16_t min_pq = 0;
  if (min_display_mastering_luminance <= 10) {
    min_pq = 7;
  } else if (min_display_mastering_luminance == 50) {
    min_pq = 62;
  } 

  uint16_t max_pq = static_cast<uint16_t>(std::round(nits_to_pq(max_nits) * 4095.0));
  if (max_pq == 0) { 
    switch (max_display_mastering_luminance) {
      case 1000:
        max_pq = 3079;
        break;
      case 2000:
        max_pq = 3388;
        break;
      case 4000:
        max_pq = 3696;
        break;
      case 10000:
        max_pq = 4095;
        break;
      default:
        max_pq = 3079;
    }
  }
  
  uint16_t avg_pq = static_cast<uint16_t>(std::round(nits_to_pq(avg_nits) * 4095.0));

  VdrDmData vdr_dm_data = {}; 
  vdr_dm_data.min_pq = min_pq;
  vdr_dm_data.max_pq = clamp16(max_pq, L1_MAX_PQ_MIN_VALUE, L1_MAX_PQ_MAX_VALUE);
  vdr_dm_data.avg_pq = clamp16(avg_pq, L1_AVG_PQ_MIN_VALUE, (vdr_dm_data.max_pq - 1));
  vdr_dm_data.max_display_mastering_luminance = max_display_mastering_luminance;
  vdr_dm_data.min_display_mastering_luminance = min_display_mastering_luminance;
  vdr_dm_data.max_content_light_level = max_content_light_level;
  vdr_dm_data.max_frame_average_light_level = max_frame_average_light_level;

  if ((last_vdr_dm_data.min_pq != vdr_dm_data.min_pq) || 
      (last_vdr_dm_data.max_pq != vdr_dm_data.max_pq) ||
      (last_vdr_dm_data.avg_pq != vdr_dm_data.avg_pq) || 
      (last_vdr_dm_data.max_display_mastering_luminance != vdr_dm_data.max_display_mastering_luminance) || 
      (last_vdr_dm_data.min_display_mastering_luminance != vdr_dm_data.min_display_mastering_luminance) || 
      (last_vdr_dm_data.max_content_light_level != vdr_dm_data.max_content_light_level) || 
      (last_vdr_dm_data.max_frame_average_light_level != vdr_dm_data.max_frame_average_light_level)) {

    last_rpu = create_rpu_nalu(vdr_dm_data);
    last_vdr_dm_data = vdr_dm_data;

    CLog::Log(LOGINFO, "HDR10PlusConvert::create_rpu_nalu_for_hdr10plus min_pq [{}] max_pq [{}] avg_pq [{}] mdml max [{}] mdml min [{}] cll [{}] fall [{}]", 
      vdr_dm_data.min_pq, 
      vdr_dm_data.max_pq, 
      vdr_dm_data.avg_pq, 
      vdr_dm_data.max_display_mastering_luminance, 
      vdr_dm_data.min_display_mastering_luminance, 
      vdr_dm_data.max_content_light_level, 
      vdr_dm_data.max_frame_average_light_level);  
  }

  return last_rpu;
}