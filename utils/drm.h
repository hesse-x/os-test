#ifndef COMMON_DRM_H
#define COMMON_DRM_H

#include <stdint.h>
#include <stddef.h>
#include "common/ioctl.h"

/* ===== DRM ioctl base ===== */
#define DRM_IOCTL_BASE 'd'

/* DRM _IOWR helpers (type 'd') */
#define _DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)
#define _DRM_IOW(nr, type)  _IOW(DRM_IOCTL_BASE, nr, type)
#define _DRM_IOR(nr, type)  _IOR(DRM_IOCTL_BASE, nr, type)
#define _DRM_IO(nr)         _IO(DRM_IOCTL_BASE, nr)

/* ===== drm_version ===== */
#define DRM_IOCTL_VERSION          _DRM_IOWR(0x00, struct drm_version)

struct drm_version {
    int version_major;
    int version_minor;
    int version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};

/* ===== drm_getcap / set_clientcap ===== */
#define DRM_IOCTL_GET_CAP          _DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP   _DRM_IOW(0x0d, struct drm_set_client_cap)

#define DRM_CAP_DUMB_BUFFER         0x1
#define DRM_CAP_VBLANK_HIGH_CRTC_TIME 0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                 0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC   0x6
#define DRM_CAP_ASYNC_PAGE_FLIP       0x7

struct drm_get_cap {
    uint64_t capability;
    uint64_t value;
};

#define DRM_CLIENT_CAP_UNIVERSAL_PLANES 0x2
#define DRM_CLIENT_CAP_ATOMIC           0x3

struct drm_set_client_cap {
    uint64_t capability;
    uint64_t value;
};

/* ===== auth / master ===== */
#define DRM_IOCTL_GET_MAGIC        _DRM_IOR(0x02, struct drm_auth)
#define DRM_IOCTL_AUTH_MAGIC       _DRM_IOW(0x03, struct drm_auth)
#define DRM_IOCTL_SET_MASTER       _DRM_IO(0x04)
#define DRM_IOCTL_DROP_MASTER      _DRM_IO(0x05)

struct drm_auth {
    int magic;
};

/* ===== KMS: mode_card_res ===== */
#define DRM_IOCTL_MODE_GETRESOURCES _DRM_IOWR(0xA0, struct drm_mode_card_res)

struct drm_mode_card_res {
    uint64_t fb_id_ptr;
    uint64_t crtc_id_ptr;
    uint64_t connector_id_ptr;
    uint64_t encoder_id_ptr;
    uint32_t count_fbs;
    uint32_t count_crtcs;
    uint32_t count_connectors;
    uint32_t count_encoders;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
};

/* ===== KMS: mode_crtc ===== */
#define DRM_IOCTL_MODE_GETCRTC     _DRM_IOWR(0xA1, struct drm_mode_crtc)
#define DRM_IOCTL_MODE_SETCRTC     _DRM_IOWR(0xA2, struct drm_mode_crtc)

struct drm_mode_modeinfo {
    uint32_t clock;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t hskew;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint16_t vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
};

struct drm_mode_crtc {
    uint64_t set_connectors_ptr;
    uint32_t count_connectors;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t x;
    uint32_t y;
    /* mode_info follows */
    uint32_t mode_valid;
    uint32_t blob_id;  /* unused, set to 0 */
    struct drm_mode_modeinfo mode;
};

#define DRM_MODE_TYPE_DRIVER  0x01
#define DRM_MODE_TYPE_PREFERRED 0x02

#define DRM_MODE_FLAG_NHSYNC 0x01
#define DRM_MODE_FLAG_PVSYNC 0x02

/* ===== KMS: mode_connector ===== */
#define DRM_IOCTL_MODE_GETCONNECTOR _DRM_IOWR(0xA3, struct drm_mode_get_connector)

#define DRM_MODE_CONNECTOR_VGA 1
#define DRM_MODE_CONNECTOR_HDMIA 11
#define DRM_MODE_CONNECTOR_VIRTUAL 15
#define DRM_MODE_CONNECTOR_DISPLAYPORT 10

#define DRM_MODE_SUBCONNECTOR_Automatic 0

#define DRM_MODE_PROPERTYBLOB 0x02

struct drm_mode_get_connector {
    uint64_t encoders_ptr;
    uint64_t prop_values_ptr;
    uint64_t props_ptr;
    uint64_t modes_ptr;
    uint32_t count_props;
    uint32_t count_encoders;
    uint32_t count_modes;
    uint32_t encoder_id;
    uint32_t connector_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mm_width;
    uint32_t mm_height;
    uint32_t subpixel;
    uint32_t pad;
};

#define DRM_MODE_CONNECTED 1
#define DRM_MODE_DISCONNECTED 2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* ===== KMS: mode_encoder ===== */
#define DRM_IOCTL_MODE_GETENCODER _DRM_IOWR(0xA4, struct drm_mode_get_encoder)

struct drm_mode_get_encoder {
    uint32_t encoder_id;
    uint32_t encoder_type;
    uint32_t crtc_id;
    uint32_t possible_crtcs;
    uint32_t possible_clones;
};

#define DRM_MODE_ENCODER_NONE 0
#define DRM_MODE_ENCODER_DAC 1
#define DRM_MODE_ENCODER_TMDS 2
#define DRM_MODE_ENCODER_LVDS 3
#define DRM_MODE_ENCODER_TVDAC 4
#define DRM_MODE_ENCODER_VIRTUAL 5

/* ===== KMS: plane ===== */
#define DRM_IOCTL_MODE_GETPLANERES    _DRM_IOWR(0xB5, struct drm_mode_get_plane_res)
#define DRM_IOCTL_MODE_GETPLANE       _DRM_IOWR(0xB6, struct drm_mode_get_plane)

struct drm_mode_get_plane_res {
    uint64_t plane_id_ptr;
    uint32_t count_planes;
};

struct drm_mode_get_plane {
    uint32_t plane_id;
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t possible_crtcs;
    uint32_t gamma_size;
    uint64_t count_format_type;
    uint64_t format_type_ptr;
};

/* ===== dumb buffer ===== */
#define DRM_IOCTL_MODE_CREATE_DUMB    _DRM_IOWR(0xB2, struct drm_mode_create_dumb)
#define DRM_IOCTL_MODE_MAP_DUMB       _DRM_IOWR(0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_DESTROY_DUMB   _DRM_IOWR(0xB4, struct drm_mode_destroy_dumb)

struct drm_mode_create_dumb {
    uint32_t height;
    uint32_t width;
    uint32_t bpp;
    uint32_t flags;
    uint32_t handle;
    uint32_t pitch;
    uint64_t size;
};

struct drm_mode_map_dumb {
    uint64_t handle;
    uint64_t offset;
    uint64_t pad;
};

struct drm_mode_destroy_dumb {
    uint32_t handle;
};

/* ===== framebuffer ===== */
#define DRM_IOCTL_MODE_ADDFB         _DRM_IOWR(0xAE, struct drm_mode_fb_cmd)
#define DRM_IOCTL_MODE_ADDFB2        _DRM_IOWR(0xB8, struct drm_mode_fb_cmd2)
#define DRM_IOCTL_MODE_RMFB          _DRM_IOWR(0xAF, uint32_t)
#define DRM_IOCTL_MODE_PAGE_FLIP     _DRM_IOWR(0xB0, struct drm_mode_crtc_page_flip)
#define DRM_IOCTL_MODE_DIRTYFB       _DRM_IOWR(0xB1, struct drm_mode_fb_dirty_cmd)

struct drm_mode_fb_cmd {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t depth;
    uint32_t handle;
};

struct drm_mode_fb_cmd2 {
    uint32_t fb_id;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t flags;
    uint32_t handles[4];
    uint32_t pitches[4];
    uint32_t offsets[4];
    uint64_t modifier[4];
};

struct drm_mode_crtc_page_flip {
    uint32_t crtc_id;
    uint32_t fb_id;
    uint32_t flags;
    uint32_t sequence;
    uint64_t user_data;
};

#define DRM_MODE_PAGE_FLIP_EVENT 0x01
#define DRM_MODE_PAGE_FLIP_ASYNC 0x02

struct drm_mode_fb_dirty_cmd {
    uint32_t fb_id;
    uint32_t flags;
    uint32_t color;
    uint32_t num_clips;
    uint64_t clips_ptr;
};

struct drm_mode_clip {
    int16_t x1;
    int16_t y1;
    int16_t x2;
    int16_t y2;
};

/* ===== DRM event (for page flip) ===== */
#define DRM_EVENT_VBLANK 0x01

struct drm_event_vblank {
    uint32_t type;       /* DRM_EVENT_VBLANK */
    uint32_t length;
    uint64_t user_data;
    uint32_t sequence;
    uint32_t sec;
    uint32_t usec;
    uint32_t crtc_id;
    uint32_t pad;
};

/* ===== stub ioctls (return -ENOSYS) ===== */
#define DRM_IOCTL_GETPROPERTY         _DRM_IOWR(0xAA, struct drm_mode_get_property)
#define DRM_IOCTL_SETPROPERTY         _DRM_IOW(0xAB, struct drm_mode_connector_set_property)
#define DRM_IOCTL_MODE_GETPROPROB     _DRM_IOWR(0xAC, struct drm_mode_get_property_blob)
#define DRM_IOCTL_MODE_OBJ_GETPROPERTIES _DRM_IOWR(0xBC, struct drm_mode_obj_get_properties)
#define DRM_IOCTL_MODE_OBJ_SETPROPERTY  _DRM_IOW(0xBD, struct drm_mode_obj_set_property)
#define DRM_IOCTL_PRIME_HANDLE_TO_FD  _DRM_IOWR(0x2D, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE  _DRM_IOWR(0x2E, struct drm_prime_handle)

struct drm_mode_get_property { uint32_t dummy; };  /* stub layout */
struct drm_mode_connector_set_property { uint32_t dummy; };
struct drm_mode_get_property_blob { uint32_t dummy; };
struct drm_mode_obj_get_properties { uint32_t dummy; };
struct drm_mode_obj_set_property { uint32_t dummy; };
struct drm_prime_handle { uint32_t dummy; };

/* DRM pixel format codes */
#define DRM_FORMAT_C8           0x20203843
#define DRM_FORMAT_XRGB8888     0x38345258
#define DRM_FORMAT_ARGB8888     0x38324152
#define DRM_FORMAT_RGB565       0x36314752

#endif /* COMMON_DRM_H */
