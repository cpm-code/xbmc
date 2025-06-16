/*
 *  Copyright (C) 2025 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <memory>
#include <vector>

#include "rendering/RenderSystemTypes.h"
#include "windowing/Resolution.h"

class CAMLDRMUtils
{
public:
  CAMLDRMUtils();
  virtual ~CAMLDRMUtils();

  int aml_get_drmDevice_handle() const { return m_fd; }
  void aml_init_drmDevice();
  std::string aml_get_drmDevice_mode();
  std::string aml_get_drmDevice_modes();
  bool aml_set_drmDevice_mode(const RESOLUTION_INFO &res, std::string mode,
    std::string framebuffer_name, bool force_mode_switch);
  int aml_get_drmProperty(std::string name, unsigned int obj_type);
  void aml_set_drmProperty(std::string name, unsigned int obj_type, unsigned int value);
  int aml_get_drmDevice_modes_count(drmModeConnection *connection);
  std::string aml_get_drmDevice_preferred_mode();
  bool aml_set_drmDevice_hotplug_mode(std::string mode);
  bool aml_get_drmDevice_connected() const { return m_connection == DRM_MODE_CONNECTED; }
private:
  void CleanAndClose();
  void aml_init_drmDevice_display();
  void aml_set_framebuffer_resolution(unsigned int width,
    unsigned int height, std::string framebuffer_name);
  int aml_get_drmDevice();
  int get_drmProp(unsigned int id, std::string name, unsigned int obj_type);
  void set_drmProp(unsigned int id, std::string name,
    unsigned int obj_type, unsigned int value, drmModeAtomicReqPtr req);
  int m_fd{-1};

  drmModeResPtr m_resources{nullptr};
  drmModeConnectorPtr m_connector{nullptr};
  drmModeConnection m_connection{DRM_MODE_DISCONNECTED};
  drmModeEncoderPtr m_encoder{nullptr};
  drmModeCrtcPtr m_crtc{nullptr};
  drmModeCrtcPtr m_orig_crtc{nullptr};
};

class CAMLDisplay
{
public:
  CAMLDisplay();

  int aml_get_Device_handle() const { return m_amlDRMUtils->aml_get_drmDevice_handle(); }
  void aml_init_drmDevice() { m_amlDRMUtils->aml_init_drmDevice(); }
  bool aml_get_display_connected() const { return m_amlDRMUtils->aml_get_drmDevice_connected(); }
  bool set_native_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
    const RenderStereoMode stereo_mode, bool force_mode_switch);
  void handle_display_stereo_mode(const RenderStereoMode stereo_mode);
  bool set_display_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
    bool force_mode_switch);
  int aml_get_display_modes_count(drmModeConnection *connection) const
    { return m_amlDRMUtils->aml_get_drmDevice_modes_count(connection); }
  std::string aml_get_preferred_mode();
  bool aml_set_hotplug_mode(std::string mode);
  bool aml_get_native_resolution(RESOLUTION_INFO *res);
  bool aml_probe_resolutions(std::vector<RESOLUTION_INFO> &resolutions);
  int aml_get_drmProperty(std::string name, unsigned int obj_type) const
    { return m_amlDRMUtils->aml_get_drmProperty(name, obj_type); }
private:
  std::unique_ptr<CAMLDRMUtils> m_amlDRMUtils;
  bool aml_mode_to_resolution(const char *mode, RESOLUTION_INFO *res);
};
