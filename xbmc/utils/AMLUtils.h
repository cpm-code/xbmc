/*
 *  Copyright (C) 2011-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "rendering/RenderSystemTypes.h"
#include "windowing/Resolution.h"
#include "utils/StreamDetails.h"

#include <cstdint>
#include <string>
#include <vector>

enum AML_DEVICE_TYPE
{
  AML_DEVICE_TYPE_UNINIT   = -2,
  AML_DEVICE_TYPE_UNKNOWN  = -1,
  AML_DEVICE_TYPE_M1,
  AML_DEVICE_TYPE_M3,
  AML_DEVICE_TYPE_M6,
  AML_DEVICE_TYPE_M8,   // S802
  AML_DEVICE_TYPE_M8B,  // S805
  AML_DEVICE_TYPE_M8M2  // S812
};

enum AML_DISPLAY_AXIS_PARAM
{
  AML_DISPLAY_AXIS_PARAM_X = 0,
  AML_DISPLAY_AXIS_PARAM_Y,
  AML_DISPLAY_AXIS_PARAM_WIDTH,
  AML_DISPLAY_AXIS_PARAM_HEIGHT
};

enum AML_SUPPORT_H264_4K2K
{
  AML_SUPPORT_H264_4K2K_UNINIT = -1,
  AML_NO_H264_4K2K,
  AML_HAS_H264_4K2K,
  AML_HAS_H264_4K2K_SAME_PROFILE
};

enum class DV_MODE : int
{
  ON = 0,
  ON_DEMAND,
  OFF
};

enum class DV_TYPE : int
{
  DISPLAY_LED = 0,
  PLAYER_LED_LLDV,
  PLAYER_LED_HDR,
  VS10_ONLY
};

enum class DV_COLORIMETRY : int
{
  AMLOGIC = 0,
  BT2020NC,
  REMOVE
};

#define AML_GXBB    0x1F
#define AML_GXL     0x21
#define AML_GXM     0x22
#define AML_G12A    0x28
#define AML_G12B    0x29
#define AML_SM1     0x2B
#define AML_SC2     0x32
#define AML_T7      0x36
#define AML_S4      0x37
#define AML_S5      0x3E
#define AML_S7D     0x47
#define AML_S6      0x48

#define FLAG_FORCE_DOVI_LL      (unsigned int)(0x4000)
#define FLAG_TOGGLE_FRAME       (unsigned int)(0x80000000)

#define DOLBY_VISION_LL_DISABLE (unsigned int)(0)
#define DOLBY_VISION_LL_YUV422  (unsigned int)(1)

#define DOLBY_VISION_FOLLOW_SOURCE     (unsigned int)(1)
#define DOLBY_VISION_FORCE_OUTPUT_MODE (unsigned int)(2)

#define DOLBY_VISION_OUTPUT_MODE_IPT        (unsigned int)(0)
#define DOLBY_VISION_OUTPUT_MODE_IPT_TUNNEL (unsigned int)(1)
#define DOLBY_VISION_OUTPUT_MODE_HDR10      (unsigned int)(2)
#define DOLBY_VISION_OUTPUT_MODE_SDR10      (unsigned int)(3)
#define DOLBY_VISION_OUTPUT_MODE_BYPASS     (unsigned int)(5)

struct AMLHdrSetupPolicy
{
  StreamHdrType srcHdr{StreamHdrType::HDR_TYPE_NONE};
  StreamHdrType srcAltHdr{StreamHdrType::HDR_TYPE_NONE};
  DOVIStreamInfo srcDvInfo{};
  StreamHdrType resolvedHdr{StreamHdrType::HDR_TYPE_NONE};
  StreamHdrType finalHdr{StreamHdrType::HDR_TYPE_NONE};
  bool dvAvail{false};
  bool dualPri10Plus{false};
  bool convHdr10Plus{false};
  bool prefConv10Plus{false};
  bool rmHdr10PlusForVs10{false};

  bool HasDolbyVisionSource() const
  {
    return ((srcHdr == StreamHdrType::HDR_TYPE_DOLBYVISION) ||
            (srcAltHdr == StreamHdrType::HDR_TYPE_DOLBYVISION));
  }
};

int  aml_get_cpufamily_id();
std::string aml_get_cpufamily_name(int cpuid = -1);
bool aml_display_support_hdr_pq();
bool aml_display_support_hdr_hlg();
bool aml_display_support_dv();
bool aml_display_support_dv_ll();
bool aml_display_support_dv_std();
bool aml_display_support_3d();
bool aml_support_hevc();
bool aml_support_hevc_4k2k();
bool aml_support_hevc_8k4k();
bool aml_support_hevc_10bit();
AML_SUPPORT_H264_4K2K aml_support_h264_4k2k();
bool aml_support_vp9();
bool aml_support_av1();
bool aml_support_dolby_vision();
std::string aml_dv_output_mode_to_string(unsigned int mode);
std::string aml_dv_mode_to_string(enum DV_MODE mode);
std::string aml_dv_type_to_string(enum DV_TYPE type);
void aml_dv_set_vs10_mode(unsigned int mode);
void aml_dv_wait_video_off(int timeout);
int aml_blackout_policy(int new_blackout);
int aml_osd_blank(int fbIndex, int blankMode);
unsigned int aml_dv_target_mode(StreamHdrType hdrType, unsigned int bitDepth);
bool aml_dv_target_enabled(StreamHdrType hdrType, unsigned int bitDepth);
unsigned int aml_dv_on(unsigned int mode);
void aml_dv_off();
unsigned int aml_dv_dolby_vision_mode();
void aml_dv_open(StreamHdrType hdrType, unsigned int bitDepth);
void aml_dv_close();
void aml_dv_set_osd_max(int max);
bool aml_is_dv_enable();
void aml_dv_display_trigger();
void aml_dv_display_auto_now();
void aml_dv_start();
void aml_dv_set_subtitles(bool visible);
void aml_dv_set_xbmc_osd();
AMLHdrSetupPolicy aml_get_hdr_setup_policy(const CDVDStreamInfo& fallbackInfo);
AMLHdrSetupPolicy aml_get_hdr_setup_policy(StreamHdrType fallbackHdr,
                                           const DOVIStreamInfo& fallbackDvInfo,
                                           unsigned int bitDepth);
StreamHdrType aml_get_final_hdr_type(StreamHdrType hdrType, unsigned int bitDepth);
void aml_apply_display_transition(StreamHdrType sourceHdrType,
                                  StreamHdrType resolvedHdrType,
                                  unsigned int bitDepth,
                                  bool resolutionChangePending);
void aml_update_hdr_mode_state(StreamHdrType hdrType, unsigned int bitDepth);
bool aml_dv_use_active_area();
enum DV_MODE aml_dv_mode();
bool aml_dv_mode_off();
enum DV_TYPE aml_dv_type();
unsigned int aml_vs10_by_setting(const std::string setting);
void aml_dv_enable_fel();
bool aml_has_frac_rate_policy();
void aml_video_mute(bool mute);
void aml_set_audio_passthrough(bool passthrough);
void aml_set_3d_video_mode(unsigned int mode, bool framepacking_support, int view_mode);
bool aml_mode_to_resolution(const char *mode, RESOLUTION_INFO &res);
bool aml_get_native_resolution(RESOLUTION_INFO &res);
bool aml_set_native_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name, RenderStereoMode stereo_mode, bool force_mode_switch);
bool aml_probe_resolutions(std::vector<RESOLUTION_INFO> &resolutions);
bool aml_set_display_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name, bool force_mode_switch);
void aml_handle_scale(const RESOLUTION_INFO &res);
void aml_handle_display_stereo_mode(RenderStereoMode stereo_mode);
void aml_enable_freeScale(const RESOLUTION_INFO &res);
void aml_disable_freeScale();
void aml_set_framebuffer_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name);
void aml_set_framebuffer_resolution(unsigned int width, unsigned int height, std::string framebuffer_name);
bool aml_read_reg(const std::string &reg, uint32_t &reg_val);
bool aml_has_capability_ignore_alpha();
bool aml_set_reg_ignore_alpha();
bool aml_unset_reg_ignore_alpha();
std::string aml_video_fps_info();
std::string aml_video_fps_drop();
void aml_pin_thread_to_core(unsigned int core_id);
void aml_wait(double waitUs);
bool aml_get_time_to_next_vsync_us(int& timeToNextVsyncUs);
bool aml_wait_until_next_vsync_window_us(int offsetUs, int& timeToNextVsyncUs);
bool aml_try_set_thread_nice(int niceLevel);
bool aml_set_timer_slack_ns(long slackNs);
bool aml_video_started();
