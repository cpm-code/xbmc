/*
 *  Copyright (C) 2025 Team CoreELEC
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <amcodec/codec.h>

#include "AMLDisplay.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIComponent.h"
#include "guilib/GUIWindowManager.h"
#include "platform/linux/SysfsPath.h"
#include "linux/fb.h"
#include "rendering/RenderSystem.h"
#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"

void FbDestroyCallback(gbm_bo* bo, void* data)
{
  drm_fb* fb = static_cast<drm_fb*>(data);

  if (fb->fb_id > 0)
  {
    CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - removing framebuffer: {}", __FUNCTION__, fb->fb_id);
    int drm_fd = gbm_device_get_fd(gbm_bo_get_device(bo));
    drmModeRmFB(drm_fd, fb->fb_id);
  }

  delete fb;
}

CAMLGBMUtils::CAMLGBMUtils(int fd)
{
  m_device = gbm_create_device(fd);
  if (!m_device)
  {
    CLog::Log(LOGERROR, "CAMLGBMUtils::{} - failed to create GBM device", __FUNCTION__);
    throw std::runtime_error("failed to create GBM device");
  }
}

CAMLGBMUtils::~CAMLGBMUtils()
{
  if (m_surface)
    gbm_surface_destroy(m_surface);
  gbm_device_destroy(m_device);
}

bool CAMLGBMUtils::CreateSurface(int width, int height, uint32_t format)
{
  uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
  m_surface = gbm_surface_create_with_modifiers(m_device, width, height, format, &modifier, 1);

  if (!m_surface)
  {
    m_surface = gbm_surface_create(m_device, width, height, format,
                                 GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  }

  if (!m_surface)
  {
    CLog::Log(LOGERROR, "CAMLGBMUtils::{} - failed to create surface: {}", __FUNCTION__,
              strerror(errno));
    return false;
  }

  CLog::Log(LOGDEBUG, "CAMLGBMUtils::{} - created surface with size {}x{}", __FUNCTION__, width,
            height);

  m_format = format;

  return true;
}

struct drm_fb* CAMLGBMUtils::GetFBFromBo(int fd, struct gbm_bo* bo)
{
  {
    struct drm_fb* fb = static_cast<drm_fb*>(gbm_bo_get_user_data(bo));
    if (fb)
    {
      if (fb->format == m_format)
        return fb;
      else
        FbDestroyCallback(bo, gbm_bo_get_user_data(bo));
    }
  }

  struct drm_fb* fb = new drm_fb;
  fb->format = m_format;

  uint32_t width, height, handles[4] = {0}, strides[4] = {0}, offsets[4] = {0};

  uint64_t modifiers[4] = {0};

  width = gbm_bo_get_width(bo);
  height = gbm_bo_get_height(bo);

  for (int i = 0; i < gbm_bo_get_plane_count(bo); i++)
  {
    handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
    strides[i] = gbm_bo_get_stride_for_plane(bo, i);
    offsets[i] = gbm_bo_get_offset(bo, i);
    modifiers[i] = gbm_bo_get_modifier(bo);
  }

  uint32_t flags = 0;

  if (modifiers[0] && modifiers[0] != DRM_FORMAT_MOD_INVALID)
    flags |= DRM_MODE_FB_MODIFIERS;

  int ret = drmModeAddFB2WithModifiers(fd, width, height, fb->format, handles, strides, offsets,
                                       modifiers, &fb->fb_id, flags);

  if (ret < 0)
  {
    ret = drmModeAddFB2(fd, width, height, fb->format, handles, strides, offsets, &fb->fb_id,
                        flags);

    if (ret < 0)
    {
      delete (fb);
      CLog::Log(LOGDEBUG, "CAMLGBMUtils::{} - failed to add framebuffer: {} ({})", __FUNCTION__,
                strerror(errno), errno);
      return nullptr;
    }
  }

  gbm_bo_set_user_data(bo, fb, FbDestroyCallback);

  return fb;
}

void CAMLGBMUtils::LockFrontBuffer(int fd)
{
  if (gbm_surface_has_free_buffers(m_surface))
  {
    m_buffer = gbm_surface_lock_front_buffer(m_surface);

    if (m_buffer)
      m_drm_fb = GetFBFromBo(fd, m_buffer);
  }
}

CAMLDRMUtils::CAMLDRMUtils()
{
  // get drmDevice
  m_fd = aml_get_drmDevice();
  if (m_fd < 0)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - could not get drmDevice", __FUNCTION__);
    throw std::runtime_error("could not get drmDevice");
  }

  /* caps need to be set before allocating connectors, encoders, crtcs, and planes */
  int ret = drmSetClientCap(m_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to set universal planes capability: {}",
              __FUNCTION__, strerror(errno));
    throw std::runtime_error("failed to set universal planes capability");
  }

  aml_init_drmDevice();

  if (aml_get_drmDevice_connected())
    aml_init_drmDevice_display();
}

CAMLDRMUtils::~CAMLDRMUtils()
{
  if (m_orig_crtc)
  {
    drmModeSetCrtc(m_fd, m_orig_crtc->crtc_id, m_orig_crtc->buffer_id,
                   m_orig_crtc->x, m_orig_crtc->y, m_resources->connectors,
                   1, &m_orig_crtc->mode);
  }

  drmDropMaster(m_fd);

  CleanAndClose();

  if (m_fd > 0)
    close(m_fd);
}

void CAMLDRMUtils::CleanAndClose()
{
  if (m_resources)
    drmModeFreeResources(m_resources);

  if (m_connector)
    drmModeFreeConnector(m_connector);

  if (m_encoder)
    drmModeFreeEncoder(m_encoder);

  if (m_crtc)
    drmModeFreeCrtc(m_crtc);

  if (m_orig_crtc)
    free(m_orig_crtc);

  if (m_plane)
    drmModeFreePlane(m_plane);
}

void CAMLDRMUtils::aml_init_drmDevice()
{
  CleanAndClose();

  // get resources of drmDevice
  m_resources = drmModeGetResources(m_fd);
  if (!m_resources)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get resources of drmDevice", __FUNCTION__);
    CleanAndClose();
    throw std::runtime_error("failed to get resources of drmDevice");
  }

  // get connector of drmDevice
  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - devices have {:d} connector(s)", __FUNCTION__,
    m_resources->count_connectors);

  for (int i = 0; i < m_resources->count_connectors; i++)
  {
    m_connector = drmModeGetConnector(m_fd, m_resources->connectors[i]);

    if (m_connector == NULL)
      continue;

    // connector state as always connected but encoder_id == 0 if not connected
    m_connection = (m_connector->encoder_id ? DRM_MODE_CONNECTED : DRM_MODE_DISCONNECTED);

    if (m_connector->connector_type == DRM_MODE_CONNECTOR_HDMIA)
      break;
  }
  if (!m_connector)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get connector of drmDevice", __FUNCTION__);
    CleanAndClose();
    throw std::runtime_error("failed to get connector of drmDevice");
  }

  // check if connector of drmDevice is connected
  if (!aml_get_drmDevice_connected())
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - connector of drmDevice is not connected", __FUNCTION__);

  drmModePlaneResPtr planeResources = drmModeGetPlaneResources(m_fd);
  if (!planeResources)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get plane resources of drmDevice", __FUNCTION__);
    throw std::runtime_error("failed to get plane resources of drmDevice");
  }

  for (uint32_t i = 0; i < planeResources->count_planes; i++)
  {
    m_plane = drmModeGetPlane(m_fd, planeResources->planes[i]);

    if (m_plane == NULL)
      continue;

    if (get_drmProp(m_plane->plane_id, "type", DRM_MODE_OBJECT_PLANE) == DRM_PLANE_TYPE_PRIMARY)
      break;

    drmModeFreePlane(m_plane);
    m_plane = NULL;
  }
  drmModeFreePlaneResources(planeResources);
  if (!m_plane)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get primary plane of drmDevice", __FUNCTION__);
    CleanAndClose();
    throw std::runtime_error("failed to get primary plane of drmDevice");
  }
}

void CAMLDRMUtils::aml_init_drmDevice_display()
{
  // get encoder of drmDevice
  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - connector[{:d}] is connected with {:d} encoder(s)", __FUNCTION__,
    m_connector->connector_id, m_connector->count_encoders);

  for (int i = 0; i < m_connector->count_encoders; i++)
  {
    m_encoder = drmModeGetEncoder(m_fd, m_connector->encoders[i]);

    if (m_encoder == NULL)
      continue;

    if (m_encoder->encoder_id == m_connector->encoder_id)
    {
      CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - using encoder {}", __FUNCTION__, m_encoder->encoder_id);
      break;
    }
    else
    {
      drmModeFreeEncoder(m_encoder);
      m_encoder = NULL;
    }
  }
  if (!m_encoder)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get encoder of drmDevice", __FUNCTION__);
    CleanAndClose();
    throw std::runtime_error("failed to get encoder of drmDevice");
  }

  // get crtc of drmDevice
  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - check {:d} crtc(s)", __FUNCTION__, m_resources->count_crtcs);

  for (int i = 0; i < m_resources->count_crtcs; i++)
  {
    m_crtc = drmModeGetCrtc(m_fd, m_resources->crtcs[i]);

    if (m_crtc == NULL)
      continue;

    if (m_encoder->possible_crtcs & (1 << i) && m_crtc->crtc_id == m_encoder->crtc_id)
    {
      CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - using crtc {}", __FUNCTION__, m_crtc->crtc_id);
      break;
    }
    else
    {
      drmModeFreeCrtc(m_crtc);
      m_crtc = NULL;
    }
  }
  if (!m_crtc)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get crtc of drmDevice", __FUNCTION__);
    CleanAndClose();
    throw std::runtime_error("failed to get crtc of drmDevice");
  }

  m_orig_crtc = static_cast<drmModeCrtcPtr>(malloc(sizeof(drmModeCrtc)));
  if (!m_orig_crtc)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to create backup of current crtc of drmDevice", __FUNCTION__);
    CleanAndClose();
    throw std::runtime_error("failed to get create backup of current crtc of drmDevice");
  }
  memcpy(m_orig_crtc, m_crtc, sizeof(drmModeCrtc));

  drmSetMaster(m_fd);
}

void CAMLDRMUtils::aml_set_framebuffer_resolution(unsigned int width,
  unsigned int height, std::string framebuffer_name)
{
  int fd0;
  std::string framebuffer = "/dev/" + framebuffer_name;

  if ((fd0 = open(framebuffer.c_str(), O_RDWR)) >= 0)
  {
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd0, FBIOGET_VSCREENINFO, &vinfo) == 0)
    {
      if (width != vinfo.xres || height != vinfo.yres)
      {
        vinfo.xres = width;
        vinfo.yres = height;
        vinfo.xres_virtual = width;
        vinfo.yres_virtual = height * 2;
        vinfo.bits_per_pixel = 32;
        vinfo.activate = FB_ACTIVATE_ALL;
        ioctl(fd0, FBIOPUT_VSCREENINFO, &vinfo);
      }
    }
    close(fd0);
  }
}

void CAMLDRMUtils::aml_drmDevice_vsync()
{
  if (m_fd != -1)
  {
    drmVBlank vbl = {};
    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    drmWaitVBlank(m_fd, &vbl);
  }
}

// get drmDevice
int CAMLDRMUtils::aml_get_drmDevice()
{
  int fd = -1;
  int numDevices = drmGetDevices2(0, nullptr, 0);
  if (numDevices <= 0)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - no drm devices found: ({})", __FUNCTION__,
              strerror(errno));
    return fd;
  }

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - drm devices found: {:d}", __FUNCTION__, numDevices);

  std::vector<drmDevicePtr> devices(numDevices);

  int ret = drmGetDevices2(0, devices.data(), devices.size());
  if (ret < 0)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - drmGetDevices2 return an error: ({})", __FUNCTION__,
              strerror(errno));
    return fd;
  }

  for (const auto device : devices)
  {
    if (!(device->available_nodes & 1 << DRM_NODE_PRIMARY))
      continue;

    if (fd >= 0)
      close(fd);

    fd = open(device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
    if (fd < 0)
      continue;

    CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - using DRM device {}", __FUNCTION__, device->nodes[DRM_NODE_PRIMARY]);

    break;
  }

  drmFreeDevices(devices.data(), devices.size());

  return fd;
}

// get current mode of drmDevice
std::string CAMLDRMUtils::aml_get_drmDevice_mode()
{
  std::string mode = "";
  std::string default_mode = "dummy_l";

  if (!aml_get_drmDevice_connected())
  {
    mode.assign(default_mode);
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - connector of drmDevice is not connected, use default mode '{}'", __FUNCTION__, mode);
    return mode;
  }

  mode = static_cast<std::string>(m_crtc->mode.name);

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - current mode: {}", __FUNCTION__, mode);

  return mode;
}

// get all modes of current connected device
std::string CAMLDRMUtils::aml_get_drmDevice_modes(void)
{
  std::string modes ="";

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - connector have {:d} modes", __FUNCTION__, m_connector->count_modes);
  for (int i = 0; i < m_connector->count_modes; i++)
  {
    std::string mode = static_cast<std::string>(m_connector->modes[i].name);
    CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - mode[{:d}]: {}", __FUNCTION__, i, mode);
    modes += mode + "\n";
  }

  return modes;
}

// set mode of drmDevice
bool CAMLDRMUtils::aml_set_drmDevice_mode(const RESOLUTION_INFO &res, std::string mode,
  std::string framebuffer_name, bool force_mode_switch)
{
  std::string current_mode = aml_get_drmDevice_mode();
  bool ret = false;

  m_width = res.iWidth;
  m_height = res.iHeight;
  m_ScreenWidth = res.iScreenWidth;
  m_ScreenHeight = res.iScreenHeight;

  if (!aml_get_drmDevice_connected())
  {
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - connector of drmDevice is not connected", __FUNCTION__);
    ret = true;
    return ret;
  }

  if (!m_crtc->buffer_id)
  {
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - current crtc do not have frame buffer", __FUNCTION__);
    ret = true;
    return ret;
  }

  for (int i = 0; i < m_connector->count_modes; i++)
  {
    if (StringUtils::EqualsNoCase(m_connector->modes[i].name, mode))
    {
      CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - found mode in connector mode list: [{:d}]:{}", __FUNCTION__, i, mode);
      drmModeFBPtr drm_fb = drmModeGetFB(m_fd, m_crtc->buffer_id);

      aml_set_framebuffer_resolution(res.iScreenWidth, res.iScreenHeight, framebuffer_name);

      ret = drmModeSetCrtc(m_fd, m_crtc->crtc_id, drm_fb->fb_id, 0, 0,
        m_resources->connectors, 1, &m_connector->modes[i]);
      m_crtc->mode = m_connector->modes[i];

      if (!CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING))
        aml_set_framebuffer_resolution(res.iWidth, res.iHeight, framebuffer_name);
      else
        aml_set_framebuffer_resolution(res.iScreenWidth, res.iScreenHeight, framebuffer_name);

      if (force_mode_switch)
        set_drmProp(m_connector->connector_id, "UPDATE", DRM_MODE_OBJECT_CONNECTOR, 1, NULL);

      drmModeFreeFB(drm_fb);
      break;
    }
  }

  return ret;
}

int CAMLDRMUtils::get_drmProp(unsigned int id, std::string name, unsigned int obj_type)
{
  int ret = -1;
  unsigned int i;
  drmModeObjectPropertiesPtr props = NULL;

  props = drmModeObjectGetProperties(m_fd, id, obj_type);
  if (!props)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get properties", __FUNCTION__);
    return ret;
  }

  for(i = 0; i < props->count_props; i++)
  {
    drmModePropertyPtr prop = drmModeGetProperty(m_fd, props->props[i]);

    if (!prop)
      continue;

    if (StringUtils::EqualsNoCase(prop->name, name))
    {
      ret = (int)props->prop_values[i];
      CLog::Log(LOGDEBUG, LOGWINDOWING, "CAMLDRMUtils::{} - get property '{}', value: {:d}", __FUNCTION__, prop->name, ret);
      drmModeFreeProperty(prop);
      break;
    }

    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
  return ret;
}

void CAMLDRMUtils::set_drmProp(unsigned int id, std::string name,
  unsigned int obj_type, unsigned int value, drmModeAtomicReqPtr req)
{
  unsigned int i;
  int res;
  drmModeObjectPropertiesPtr props = NULL;

  props = drmModeObjectGetProperties(m_fd, id, obj_type);
  if (!props)
  {
    CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to get properties", __FUNCTION__);
    return;
  }

  for(i = 0; i < props->count_props; i++)
  {
    drmModePropertyPtr prop = drmModeGetProperty(m_fd, props->props[i]);

    if (!prop)
      continue;

    if (StringUtils::EqualsNoCase(prop->name, name))
    {
      if (req == NULL)
      {
        if ((res = drmModeObjectSetProperty(m_fd, id, obj_type, props->props[i], value)) != 0)
          CLog::Log(LOGERROR, "CAMLDRMUtils::{} - unable to set property '{}', value: {:d}, res: {:d}", __FUNCTION__, prop->name, value, res);
      }
      else
      {
        if ((res = drmModeAtomicAddProperty(req, id, props->props[i], value)) < 0)
          CLog::Log(LOGERROR, "CAMLDRMUtils::{} - unable to add property '{}', value: {:d}, res: {:d}", __FUNCTION__, prop->name, value, res);
      }

      CLog::Log(LOGDEBUG, LOGWINDOWING, "CAMLDRMUtils::{} - set property '{}', value: {:d}", __FUNCTION__, prop->name, value);
      drmModeFreeProperty(prop);
      break;
    }

    drmModeFreeProperty(prop);
  }

  drmModeFreeObjectProperties(props);
}

// get a property
int CAMLDRMUtils::aml_get_drmProperty(std::string name, unsigned int obj_type)
{
  int ret = -1;
  unsigned int id;

  if (!aml_get_drmDevice_connected())
  {
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - connector of drmDevice is not connected", __FUNCTION__);

    switch (obj_type) {
      case DRM_MODE_OBJECT_CONNECTOR:
        id = m_connector->connector_id;
        ret = get_drmProp(id, name, obj_type);
        [[fallthrough]];
      default:
        return ret;
    }
  }

  switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC:
      id = m_crtc->crtc_id;
      break;
    case DRM_MODE_OBJECT_CONNECTOR:
      id = m_connector->connector_id;
      break;
    case DRM_MODE_OBJECT_ENCODER:
      id = m_encoder->encoder_id;
      break;
    default:
      return ret;
  }

  ret = get_drmProp(id, name, obj_type);

  return ret;
}

// set a property
void CAMLDRMUtils::aml_set_drmProperty(std::string name, unsigned int obj_type, unsigned int value)
{
  unsigned int id;

  if (!aml_get_drmDevice_connected())
  {
    CLog::Log(LOGWARNING, "CAMLDRMUtils::{} - connector of drmDevice is not connected", __FUNCTION__);
    return;
  }

  switch (obj_type) {
    case DRM_MODE_OBJECT_CRTC:
      id = m_crtc->crtc_id;
      break;
    case DRM_MODE_OBJECT_CONNECTOR:
      id = m_connector->connector_id;
      break;
    case DRM_MODE_OBJECT_ENCODER:
      id = m_encoder->encoder_id;
      break;
    default:
      return;
  }

  set_drmProp(id, name, obj_type, value, NULL);
}

// get modes count and status if current device is connected
int CAMLDRMUtils::aml_get_drmDevice_modes_count(drmModeConnection *connection)
{
  int mode_count = 0;

  if (connection)
    *connection = m_connection;

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - connector have {:d} modes", __FUNCTION__, m_connector->count_modes);
  mode_count = m_connector->count_modes;

  return mode_count;
}

// get preferred mode of drmDevice
std::string CAMLDRMUtils::aml_get_drmDevice_preferred_mode()
{
  std::string mode, modes = "";

  if (!aml_get_drmDevice_connected())
    return mode;

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - connector have {:d} modes", __FUNCTION__, m_connector->count_modes);
  for (int i = 0; i < m_connector->count_modes; i++)
  {
    if (m_connector->modes[i].type & DRM_MODE_TYPE_PREFERRED)
    {
      mode.assign(m_connector->modes[i].name);
      break;
    }
  }

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - preferred mode: {}", __FUNCTION__, mode);

  return mode;
}

bool CAMLDRMUtils::aml_set_drmDevice_active(std::string mode, bool active)
{
  bool ret = false;
  drmModeModeInfoPtr drmDevicemode = NULL;

  for (int i = 0; i < m_connector->count_modes; i++)
  {
    std::string connector_mode = static_cast<std::string>(m_connector->modes[i].name);
    if (StringUtils::EqualsNoCase(connector_mode, mode))
    {
      CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - use mode[{:d}]: {}", __FUNCTION__, i, connector_mode);
      drmDevicemode = &m_connector->modes[i];
      break;
    }
  }

  if (drmDevicemode != NULL)
  {
    uint32_t mode_blobid = 0;
    drmModeAtomicReqPtr req = drmModeAtomicAlloc();

    int res = drmSetClientCap(m_fd, DRM_CLIENT_CAP_ATOMIC, 1);
    if (res)
    {
      CLog::Log(LOGERROR, "CAMLDRMUtils::{} - failed to set client cap of drmDevice ({:d})", __FUNCTION__, res);
      return ret;
    }

    if (req)
    {
      if (!m_crtc)
      {
        m_crtc = drmModeGetCrtc(m_fd, m_resources->crtcs[0]);
        m_crtc->mode = *drmDevicemode;
      }

      set_drmProp(m_connector->connector_id, "CRTC_ID", DRM_MODE_OBJECT_CONNECTOR, m_crtc->crtc_id, req);

      drmModeCreatePropertyBlob(m_fd, drmDevicemode, sizeof(*drmDevicemode), &mode_blobid);

      set_drmProp(m_crtc->crtc_id, "MODE_ID", DRM_MODE_OBJECT_CRTC, mode_blobid, req);
      set_drmProp(m_crtc->crtc_id, "ACTIVE", DRM_MODE_OBJECT_CRTC, active ? 1 : 0, req);

      ret = drmModeAtomicCommit(m_fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
      if (ret)
        CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - failed to set drmDevice mode: {}", __FUNCTION__, drmDevicemode->name);

      drmModeAtomicFree(req);
      drmModeDestroyPropertyBlob(m_fd, mode_blobid);
    }
  }

  return ret;
}

bool CAMLDRMUtils::aml_set_drmDevice_hotplug_mode(std::string mode)
{
  std::string current_mode = aml_get_drmDevice_mode();
  bool ret = false;

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - current mode: {}, new mode: {}", __FUNCTION__,
    current_mode, mode);

  if (StringUtils::EqualsNoCase(current_mode, mode))
  {
    CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - hotplug mode already changed: {}", __FUNCTION__, mode);
    ret = true;
    return ret;
  }

  ret = aml_set_drmDevice_active(mode, true);
  // force connected
  m_connection = DRM_MODE_CONNECTED;

  CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - reset of drmDevice finished", __FUNCTION__);

  return ret;
}

bool CAMLDRMUtils::SupportsFormat(drmModePlane *plane, uint32_t format)
{
  for (uint32_t i = 0; i < plane->count_formats; i++)
    if (plane->formats[i] == format)
      return true;

  return false;
}

void CAMLDRMUtils::FlipPage(uint32_t fb_id, bool rendered, bool videoLayer, bool async)
{
  if (!aml_get_drmDevice_connected())
    return;

  drmModeAtomicReqPtr req = drmModeAtomicAlloc();

  if (rendered)
  {
    set_drmProp(m_plane->plane_id, "FB_ID", DRM_MODE_OBJECT_PLANE , fb_id, req);
    set_drmProp(m_plane->plane_id, "CRTC_ID", DRM_MODE_OBJECT_PLANE , m_crtc->crtc_id, req);
    set_drmProp(m_plane->plane_id, "SRC_X", DRM_MODE_OBJECT_PLANE , 0, req);
    set_drmProp(m_plane->plane_id, "SRC_Y", DRM_MODE_OBJECT_PLANE , 0, req);
    set_drmProp(m_plane->plane_id, "SRC_W", DRM_MODE_OBJECT_PLANE , m_width << 16, req);
    set_drmProp(m_plane->plane_id, "SRC_H", DRM_MODE_OBJECT_PLANE , m_height << 16, req);
    set_drmProp(m_plane->plane_id, "CRTC_X", DRM_MODE_OBJECT_PLANE , 0, req);
    set_drmProp(m_plane->plane_id, "CRTC_Y", DRM_MODE_OBJECT_PLANE , 0, req);
    set_drmProp(m_plane->plane_id, "CRTC_W", DRM_MODE_OBJECT_PLANE , m_ScreenWidth, req);
    set_drmProp(m_plane->plane_id, "CRTC_H", DRM_MODE_OBJECT_PLANE , m_ScreenHeight, req);

    if (m_inFenceFd != -1)
    {
      set_drmProp(m_crtc->crtc_id, "OUT_FENCE_PTR", DRM_MODE_OBJECT_CRTC , reinterpret_cast<uint64_t>(&m_outFenceFd), req);
      set_drmProp(m_plane->plane_id, "IN_FENCE_FD", DRM_MODE_OBJECT_PLANE , m_inFenceFd, req);
    }
  }
  else if (videoLayer && !CServiceBroker::GetGUI()->GetWindowManager().HasVisibleControls())
  {
    // disable gui plane when video layer is active and gui has no visible controls
    set_drmProp(m_plane->plane_id, "FB_ID", DRM_MODE_OBJECT_PLANE , 0, req);
    set_drmProp(m_plane->plane_id, "CRTC_ID", DRM_MODE_OBJECT_PLANE , 0, req);
  }

  if (drmModeAtomicCommit(m_fd, req, async ? DRM_MODE_ATOMIC_NONBLOCK : 0, NULL))
    CLog::Log(LOGDEBUG, "CAMLDRMUtils::{} - failed to make drmDevice atomic commit", __FUNCTION__);

  if (m_inFenceFd != -1)
  {
    close(m_inFenceFd);
    m_inFenceFd = -1;
  }

  drmModeAtomicFree(req);
}

CAMLDisplay::CAMLDisplay()
:  m_amlDRMUtils(new CAMLDRMUtils)
{
}

bool CAMLDisplay::set_native_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
  const RenderStereoMode stereo_mode, bool force_mode_switch)
{
  bool result = false;

  if (aml_get_cpufamily_id() < AML_T7)
  {
    handle_display_stereo_mode(stereo_mode);
    result = set_display_resolution(res, framebuffer_name, force_mode_switch);
    if (stereo_mode != RenderStereoMode::OFF)
      CSysfsPath("/sys/class/amhdmitx/amhdmitx0/phy", 1);
  }
  else
  {
    if (stereo_mode == RenderStereoMode::HARDWAREBASED ||
        stereo_mode == RenderStereoMode::OFF)
      handle_display_stereo_mode(stereo_mode);
    result = set_display_resolution(res, framebuffer_name, force_mode_switch);
    if (stereo_mode != RenderStereoMode::HARDWAREBASED &&
        stereo_mode != RenderStereoMode::OFF)
      handle_display_stereo_mode(stereo_mode);
  }

  return result;
}

void CAMLDisplay::handle_display_stereo_mode(const RenderStereoMode stereo_mode)
{
  static RenderStereoMode kernel_stereo_mode = RenderStereoMode::UNDEFINED;

  if (kernel_stereo_mode == RenderStereoMode::UNDEFINED)
  {
    CSysfsPath _kernel_stereo_mode{"/sys/class/amhdmitx/amhdmitx0/stereo_mode"};
    if (_kernel_stereo_mode.Exists())
      kernel_stereo_mode = static_cast<RenderStereoMode>(_kernel_stereo_mode.Get<int>().value());
  }

  if (kernel_stereo_mode != stereo_mode)
  {
    std::string command = "3doff";
    switch (stereo_mode)
    {
      case RenderStereoMode::SPLIT_VERTICAL:
        command = "3dlr";
        break;
      case RenderStereoMode::SPLIT_HORIZONTAL:
        command = "3dtb";
        break;
      case RenderStereoMode::HARDWAREBASED:
        command = "3dfp";
        break;
      default:
        // nothing - command is already initialised to "3doff"
        break;
    }

    CLog::Log(LOGDEBUG, "CAMLDisplay::{} setting new mode: {}", __FUNCTION__, command);
    CSysfsPath("/sys/class/amhdmitx/amhdmitx0/config", command);
    kernel_stereo_mode = stereo_mode;
  }
}

bool CAMLDisplay::set_display_resolution(const RESOLUTION_INFO &res, std::string framebuffer_name,
  bool force_mode_switch)
{
  std::string mode = res.strId.c_str();
  std::string cur_mode;
  std::vector<std::string> _mode = StringUtils::Split(mode, ' ');
  std::string mode_options;

  if (_mode.size() > 1)
  {
    mode = _mode[0];
    unsigned int i = 1;
    while(i < (_mode.size() - 1))
    {
      if (i > 1)
        mode_options.append(" ");
      mode_options.append(_mode[i]);
      i++;
    }
    CLog::Log(LOGDEBUG, "CAMLDisplay::{}: try to set mode: {} ({})", __FUNCTION__, mode.c_str(), mode_options.c_str());
  }
  else
    CLog::Log(LOGDEBUG, "CAMLDisplay::{}: try to set mode: {}", __FUNCTION__, mode.c_str());

  cur_mode = m_amlDRMUtils->aml_get_drmDevice_mode();

  int fractional_rate = (res.fRefreshRate == floor(res.fRefreshRate)) ? 0 : 1;

  if (m_amlDRMUtils->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR) != fractional_rate)
    m_amlDRMUtils->aml_set_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR, fractional_rate);

  m_amlDRMUtils->aml_set_drmDevice_mode(res, mode, framebuffer_name, force_mode_switch);

  return true;
}

std::string CAMLDisplay::aml_get_preferred_mode()
{
  std::string mode = "";

  CSysfsPath cmdline{"/proc/cmdline"};
  if (cmdline.Exists())
  {
    std::vector<std::string> cmdlinestr = StringUtils::Split(cmdline.Get<std::string>().value(), " ");

    for (std::vector<std::string>::const_reverse_iterator item = cmdlinestr.rbegin(); item != cmdlinestr.rend(); ++item)
    {
      std::vector<std::string> itemstr = StringUtils::Split(*item, "=");
      if (itemstr.size() == 2)
      {
        std::string key = itemstr.front();
        std::string value = itemstr.back();
        if (StringUtils::EqualsNoCase(key, "vout"))
        {
          std::vector<std::string> vout = StringUtils::Split(value, ",");
          if (vout.size() > 0)
          {
            mode.assign(vout.front());
            break;
          }
        }
      }
    }
  }

  if (mode.empty())
    mode = m_amlDRMUtils->aml_get_drmDevice_preferred_mode();

  CLog::Log(LOGDEBUG, "CAMLDisplay::{} - preferred mode: {}", __FUNCTION__, mode);

  return mode;
}

bool CAMLDisplay::aml_set_hotplug_mode(std::string mode)
{
  return m_amlDRMUtils->aml_set_drmDevice_hotplug_mode(mode);
}

bool CAMLDisplay::aml_mode_to_resolution(const char *mode, RESOLUTION_INFO *res)
{
  int width = 0, height = 0, rrate = 60;
  char smode[2] = { 0 };

  if (!res)
    return false;

  res->iWidth = 0;
  res->iHeight= 0;

  if(!mode)
    return false;

  const bool nativeGui = CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);
  std::string fromMode = mode;
  StringUtils::Trim(fromMode);
  // strips, for example, 720p* to 720p
  // the * indicate the 'native' mode of the display
  if (StringUtils::EndsWith(fromMode, "*"))
    fromMode.erase(fromMode.size() - 1);

  if (sscanf(fromMode.c_str(), "%dx%dp%dhz", &width, &height, &rrate) == 3)
  {
    *smode = 'p';
  }
  else if (sscanf(fromMode.c_str(), "%d%1[ip]%dhz", &height, smode, &rrate) >= 2)
  {
    switch (height)
    {
      case 480:
      case 576:
        width = 720;
        break;
      case 720:
        width = 1280;
        break;
      case 1080:
        width = 1920;
        break;
      case 2160:
        width = 3840;
        break;
    }
  }
  else if (sscanf(fromMode.c_str(), "%dcvbs", &height) == 1)
  {
    width = 720;
    *smode = 'i';
    rrate = (height == 576) ? 50 : 60;
  }
  else if (sscanf(fromMode.c_str(), "4k2k%d", &rrate) == 1)
  {
    width = 3840;
    height = 2160;
    *smode = 'p';
  }
  else if (StringUtils::EqualsNoCase(fromMode, "dummy_l"))
  {
    width = 1920;
    height = 1080;
    rrate = 60;
    *smode = 'p';
  }
  else
  {
    return false;
  }

  res->iWidth = nativeGui ? width : std::min(width, 1920);
  res->iHeight= nativeGui ? height : std::min(height, 1080);
  res->iScreenWidth = width;
  res->iScreenHeight = height;
  res->dwFlags = (*smode == 'p') ? D3DPRESENTFLAG_PROGRESSIVE : D3DPRESENTFLAG_INTERLACED;

  switch (rrate)
  {
    case 23:
    case 29:
    case 59:
      res->fRefreshRate = (float)((rrate + 1)/1.001f);
      break;
    default:
      res->fRefreshRate = (float)rrate;
      break;
  }

  res->bFullScreen   = true;
  res->iSubtitles    = (int)(0.965 * res->iHeight);
  res->fPixelRatio   = 1.0f;
  res->strId         = fromMode;
  res->strMode       = StringUtils::Format("{:d}x{:d} @ {:.2f}{} - Full Screen", res->iScreenWidth, res->iScreenHeight, res->fRefreshRate,
    res->dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "");

  if (fromMode.find("FramePacking") != std::string::npos)
  {
    res->iBlanking   = res->iScreenHeight == 1080 ? 45 : 30;
    res->dwFlags |= D3DPRESENTFLAG_MODE3DFP;
  }

  if (fromMode.find("TopBottom") != std::string::npos)
    res->dwFlags |= D3DPRESENTFLAG_MODE3DTB;

  if (fromMode.find("SidebySide") != std::string::npos)
    res->dwFlags |= D3DPRESENTFLAG_MODE3DSBS;

  return res->iWidth > 0 && res->iHeight> 0;
}

bool CAMLDisplay::aml_get_native_resolution(RESOLUTION_INFO *res)
{
  std::string mode = m_amlDRMUtils->aml_get_drmDevice_mode();
  bool result = aml_mode_to_resolution(mode.c_str(), res);

  if (m_amlDRMUtils->aml_get_drmProperty("FRAC_RATE_POLICY", DRM_MODE_OBJECT_CONNECTOR) == 1)
    res->fRefreshRate /= 1.001f;

  return result;
}

bool CAMLDisplay::aml_probe_resolutions(std::vector<RESOLUTION_INFO> &resolutions)
{
  std::string valstr, addstr;

  valstr = m_amlDRMUtils->aml_get_drmDevice_modes();

  CSysfsPath vesa{"/flash/vesa.enable"};
  if (vesa.Exists())
  {
    CSysfsPath vesa_cap{"/sys/class/amhdmitx/amhdmitx0/vesa_cap"};
    if (vesa_cap.Exists())
    {
      addstr = vesa_cap.Get<std::string>().value();
      valstr += "\n" + addstr;
    }
  }

  if (aml_display_support_3d())
  {
    CSysfsPath user_dcapfile_3d{CSpecialProtocol::TranslatePath("special://home/userdata/disp_cap_3d")};
    if (!user_dcapfile_3d.Exists())
    {
      CSysfsPath dcapfile3d{"/sys/class/amhdmitx/amhdmitx0/disp_cap_3d"};
      if (dcapfile3d.Exists())
      {
        addstr = dcapfile3d.Get<std::string>().value();
        valstr += "\n" + addstr;
      }
    }
    else
      valstr = user_dcapfile_3d.Get<std::string>().value();
  }


  std::vector<std::string> probe_str = StringUtils::Split(valstr, "\n");

  resolutions.clear();
  RESOLUTION_INFO res;
  for (std::vector<std::string>::const_iterator i = probe_str.begin(); i != probe_str.end(); ++i)
  {
    if (((StringUtils::StartsWith(i->c_str(), "4k2k")) && (aml_support_h264_4k2k() > AML_NO_H264_4K2K)) || !(StringUtils::StartsWith(i->c_str(), "4k2k")))
    {
      if (aml_mode_to_resolution(i->c_str(), &res))
      {
        // skip 'dummy_l' resolution when HDMI is connected
        if (StringUtils::EqualsNoCase(i->c_str(), "dummy_l") && resolutions.size() > 0)
          continue;
        else
          resolutions.push_back(res);

        // Add fractional frame rates: 23.976, 29.97 and 59.94 Hz
        switch ((int)res.fRefreshRate)
        {
          case 24:
          case 30:
          case 60:
            res.fRefreshRate /= 1.001f;
            res.strMode       = StringUtils::Format("{:d}x{:d} @ {:.2f}{} - Full Screen", res.iScreenWidth, res.iScreenHeight, res.fRefreshRate,
              res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "");
            resolutions.push_back(res);
            break;
        }
      }
    }
  }
  return resolutions.size() > 0;
}
