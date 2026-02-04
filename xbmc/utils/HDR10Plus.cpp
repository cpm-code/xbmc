/*
 *  Copyright (C) 2024 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "utils/log.h"

#include <vector>
#include <cstdint>

#include "HDR10Plus.h"

#include "BitstreamReader.h"

Hdr10PlusMetadata hdr10plus_sei_to_metadata(CBitstreamReader& br) 
{
  Hdr10PlusMetadata metadata = {};

  metadata.itu_t_t35_country_code = br.ReadBits(8);
  metadata.itu_t_t35_terminal_provider_code = br.ReadBits(16);
  metadata.itu_t_t35_terminal_provider_oriented_code = br.ReadBits(16);
  metadata.application_identifier = br.ReadBits(8);
  metadata.application_version = br.ReadBits(8);
  metadata.num_windows = br.ReadBits(2);

  if (metadata.num_windows > 1) {

    metadata.processing_windows = std::vector<ProcessingWindow>(metadata.num_windows);

    for (uint8_t i = 1; i < metadata.num_windows; i++) {
        ProcessingWindow& window = metadata.processing_windows[i];
        window.window_upper_left_corner_x = br.ReadBits(16);
        window.window_upper_left_corner_y = br.ReadBits(16);
        window.window_lower_right_corner_x = br.ReadBits(16);
        window.window_lower_right_corner_y = br.ReadBits(16);
        window.center_of_ellipse_x = br.ReadBits(16);
        window.center_of_ellipse_y = br.ReadBits(16);
        window.rotation_angle = br.ReadBits(8);
        window.semimajor_axis_internal_ellipse = br.ReadBits(16);
        window.semimajor_axis_external_ellipse = br.ReadBits(16);
        window.semiminor_axis_external_ellipse = br.ReadBits(16);
        window.overlap_process_option = br.ReadBits(1);
    }
  }

  metadata.targeted_system_display_maximum_luminance = br.ReadBits(27);
  metadata.targeted_system_display_actual_peak_luminance_flag = br.ReadBits(1);

  if (metadata.targeted_system_display_actual_peak_luminance_flag) {
      ActualTargetedSystemDisplay& display = metadata.actual_targeted_system_display;
      display.num_rows_targeted_system_display_actual_peak_luminance = br.ReadBits(5);
      display.num_cols_targeted_system_display_actual_peak_luminance = br.ReadBits(5);
      display.targeted_system_display_actual_peak_luminance.resize(
          display.num_rows_targeted_system_display_actual_peak_luminance,
          std::vector<uint8_t>(display.num_cols_targeted_system_display_actual_peak_luminance)
      );
      for (uint8_t i = 0; i < display.num_rows_targeted_system_display_actual_peak_luminance; i++) {
          for (uint8_t j = 0; j < display.num_cols_targeted_system_display_actual_peak_luminance; j++) {
              display.targeted_system_display_actual_peak_luminance[i][j] = br.ReadBits(4);
          }
      }
  }

  // Parse luminance info
  if (metadata.num_windows > 0) {

    metadata.luminance = std::vector<Luminance>(metadata.num_windows);

    for (uint8_t i = 0; i < metadata.num_windows; i++) {

      Luminance& luminance = metadata.luminance[i];
      // Parse maxscl and average_maxrgb
      for (int i = 0; i < 3; i++) {
        luminance.maxscl[i] = br.ReadBits(17);
      }
      luminance.average_maxrgb = br.ReadBits(17);

      // Parse distribution maxrgb
      luminance.num_distribution_maxrgb_percentiles = br.ReadBits(4);
      luminance.distribution_maxrgb.resize(luminance.num_distribution_maxrgb_percentiles);
      for (uint8_t i = 0; i < luminance.num_distribution_maxrgb_percentiles; i++) {
        luminance.distribution_maxrgb[i].percentage = br.ReadBits(7);
        luminance.distribution_maxrgb[i].percentile = br.ReadBits(17);
      }
      luminance.fraction_bright_pixels = br.ReadBits(10);
    }
  }

  // Parse mastering display info
  metadata.mastering_display_actual_peak_luminance_flag = br.ReadBits(1);
  if (metadata.mastering_display_actual_peak_luminance_flag) {
      ActualMasteringDisplay& display = metadata.actual_mastering_display;
      display.num_rows_mastering_display_actual_peak_luminance = br.ReadBits(5);
      display.num_cols_mastering_display_actual_peak_luminance = br.ReadBits(5);
      display.mastering_display_actual_peak_luminance.resize(
          display.num_rows_mastering_display_actual_peak_luminance *
          display.num_cols_mastering_display_actual_peak_luminance
      );
      for (uint8_t i = 0; i < display.mastering_display_actual_peak_luminance.size(); i++) {
          display.mastering_display_actual_peak_luminance[i] = br.ReadBits(4);
      }
  }

  // Parse tone mapping info
  metadata.tone_mapping_flag = br.ReadBits(1);
  if (metadata.tone_mapping_flag) {
      BezierCurve& curve = metadata.bezier_curve;
      curve.knee_point_x = br.ReadBits(12);
      curve.knee_point_y = br.ReadBits(12);
      curve.num_bezier_curve_anchors = br.ReadBits(4);
      curve.bezier_curve_anchors.resize(curve.num_bezier_curve_anchors);
      for (uint8_t i = 0; i < curve.num_bezier_curve_anchors; i++) {
          curve.bezier_curve_anchors[i] = br.ReadBits(10);
      }
  }

  // Parse color saturation info
  metadata.color_saturation_mapping_flag = br.ReadBits(1);
  if (metadata.color_saturation_mapping_flag) {
      metadata.color_saturation_weight = br.ReadBits(6);
  }

  return metadata;
}