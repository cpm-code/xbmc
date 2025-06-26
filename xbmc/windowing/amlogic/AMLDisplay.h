/*
 *  Copyright (C) 2025 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <memory>
#include <utility>
#include <vector>

#include "rendering/RenderSystemTypes.h"
#include "windowing/Resolution.h"

struct drm_fb
{
  uint32_t fb_id;
  uint32_t format;
};

class CAMLGBMUtils
{
public:
  CAMLGBMUtils(int fd);
  virtual ~CAMLGBMUtils();
  struct gbm_device *GetDevice() const { return m_device; }
  struct gbm_surface *GetSurface() const { return m_surface; }
  bool CreateSurface(int width, int height, uint32_t format);
  uint32_t GetFBId() { return m_drm_fb->fb_id; }
  void LockFrontBuffer(int fd);
  void UnlockFrontBuffer() { if (m_buffer)
                               gbm_surface_release_buffer(m_surface, m_buffer); }
private:
  struct drm_fb* GetFBFromBo(int fd, struct gbm_bo* bo);

  uint32_t m_format;
  struct gbm_device *m_device;
  struct gbm_surface *m_surface;
  struct gbm_bo* m_buffer;
  struct drm_fb* m_drm_fb;
};

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
  bool aml_set_drmDevice_active(std::string mode, bool active);
  bool aml_set_drmDevice_hotplug_mode(std::string mode);
  bool aml_get_drmDevice_connected() const { return m_connection == DRM_MODE_CONNECTED; }
  void FlipPage(uint32_t fb_id);

  void SetInFenceFd(int fd) { m_inFenceFd = fd; }
  int TakeOutFenceFd()
  {
    int fd{-1};
    return std::exchange(m_outFenceFd, fd);
  }
private:
  void CleanAndClose();
  void aml_init_drmDevice_display();
  void aml_set_framebuffer_resolution(unsigned int width,
    unsigned int height, std::string framebuffer_name);
  int aml_get_drmDevice();
  int get_drmProp(unsigned int id, std::string name, unsigned int obj_type);
  void set_drmProp(unsigned int id, std::string name,
    unsigned int obj_type, unsigned int value, drmModeAtomicReqPtr req);
  bool SupportsFormat(drmModePlane *plane, uint32_t format);
  int m_fd{-1};
  int m_width;
  int m_height;
  int m_ScreenWidth;
  int m_ScreenHeight;

  drmModeResPtr m_resources{nullptr};
  drmModeConnectorPtr m_connector{nullptr};
  drmModeConnection m_connection{DRM_MODE_DISCONNECTED};
  drmModeEncoderPtr m_encoder{nullptr};
  drmModeCrtcPtr m_crtc{nullptr};
  drmModeCrtcPtr m_orig_crtc{nullptr};
  drmModePlanePtr m_plane{nullptr};

  int m_inFenceFd{-1};
  int m_outFenceFd{-1};
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
  void FlipPage(uint32_t fb_id) { m_amlDRMUtils->FlipPage(fb_id); }
  bool aml_set_drmDevice_active(bool active) const
    { return m_amlDRMUtils->aml_set_drmDevice_active(m_amlDRMUtils->aml_get_drmDevice_mode(), active); }

  void SetInFenceFd(int fd) { m_amlDRMUtils->SetInFenceFd(fd); }
  int TakeOutFenceFd() const { return m_amlDRMUtils->TakeOutFenceFd(); }
private:
  std::unique_ptr<CAMLDRMUtils> m_amlDRMUtils;
  bool aml_mode_to_resolution(const char *mode, RESOLUTION_INFO *res);
};
