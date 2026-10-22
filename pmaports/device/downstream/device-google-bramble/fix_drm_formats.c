#define _GNU_SOURCE
#include <dlfcn.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x00001000U
#endif
#ifndef STATX_MNT_ID_UNIQUE
#define STATX_MNT_ID_UNIQUE 0x00004000U
#endif

#define DRM_MODE_OBJECT_PLANE 0xeeeeeeee

struct udev_device;

static drmModePlanePtr (*orig_drmModeGetPlane)(int fd, uint32_t plane_id) = NULL;
static ssize_t (*orig_read)(int fd, void *buf, size_t count) = NULL;
static int (*orig_drmGetCap)(int fd, uint64_t capability, uint64_t *value) = NULL;
static drmModeObjectPropertiesPtr (*orig_drmModeObjectGetProperties)(int fd, uint32_t object_id, uint32_t object_type) = NULL;
static const char *(*orig_udev_device_get_property_value)(struct udev_device *udev_device, const char *property) = NULL;
static int (*orig_faccessat)(int dirfd, const char *pathname, int mode, int flags) = NULL;
static int (*orig_statx)(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf) = NULL;

int statx(int dirfd, const char *pathname, int flags, unsigned int mask, struct statx *statxbuf) {
    if (!orig_statx) {
        orig_statx = dlsym(RTLD_NEXT, "statx");
    }
    int ret = orig_statx(dirfd, pathname, flags, mask, statxbuf);
    if (ret == 0 && statxbuf) {
        if (mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE)) {
            statxbuf->stx_mask |= (mask & (STATX_MNT_ID | STATX_MNT_ID_UNIQUE));
            statxbuf->stx_mnt_id = 1;
        }
    }
    return ret;
}

static void ensure_display_active(int fd) {
    int bfd = open("/sys/class/backlight/panel0-backlight/brightness", O_WRONLY);
    if (bfd >= 0) {
        write(bfd, "1023\n", 5);
        close(bfd);
    }
    // Connector 29: DSI-1
    // Property 2: DPMS -> 0 (On)
    // Property 37: autorefresh -> 1 (Auto-push frames in command mode)
    drmModeObjectSetProperty(fd, 29, DRM_MODE_OBJECT_CONNECTOR, 2, 0);
    drmModeObjectSetProperty(fd, 29, DRM_MODE_OBJECT_CONNECTOR, 37, 1);
}

int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (!orig_faccessat) {
        orig_faccessat = dlsym(RTLD_NEXT, "faccessat");
    }

    if (flags != 0) {
        int ret = orig_faccessat(dirfd, pathname, mode, flags);
        if (ret == -1 && errno == EINVAL) {
            if (flags & AT_SYMLINK_NOFOLLOW) {
                struct stat st;
                return fstatat(dirfd, pathname, &st, AT_SYMLINK_NOFOLLOW);
            }
            return orig_faccessat(dirfd, pathname, mode, 0);
        }
        return ret;
    }
    return orig_faccessat(dirfd, pathname, mode, 0);
}

static const char *(*orig_udev_device_get_devnode)(struct udev_device *udev_device) = NULL;
static const char *(*orig_udev_device_get_sysname)(struct udev_device *udev_device) = NULL;

static void init_udev_ptrs(void) {
    if (!orig_udev_device_get_property_value)
        orig_udev_device_get_property_value = dlsym(RTLD_NEXT, "udev_device_get_property_value");
    if (!orig_udev_device_get_devnode)
        orig_udev_device_get_devnode = dlsym(RTLD_NEXT, "udev_device_get_devnode");
    if (!orig_udev_device_get_sysname)
        orig_udev_device_get_sysname = dlsym(RTLD_NEXT, "udev_device_get_sysname");

    if (!orig_udev_device_get_property_value || !orig_udev_device_get_devnode) {
        void *h = dlopen("libudev.so.1", RTLD_LAZY | RTLD_GLOBAL);
        if (h) {
            if (!orig_udev_device_get_property_value)
                orig_udev_device_get_property_value = dlsym(h, "udev_device_get_property_value");
            if (!orig_udev_device_get_devnode)
                orig_udev_device_get_devnode = dlsym(h, "udev_device_get_devnode");
            if (!orig_udev_device_get_sysname)
                orig_udev_device_get_sysname = dlsym(h, "udev_device_get_sysname");
        }
    }
}

const char *udev_device_get_property_value(struct udev_device *udev_device, const char *property) {
    init_udev_ptrs();
    if (!udev_device || !property)
        return NULL;

    if (strcmp(property, "ID_INPUT_TOUCHSCREEN") == 0 ||
        strcmp(property, "ID_INPUT") == 0 ||
        strcmp(property, "ID_INPUT_KEYBOARD") == 0 ||
        strcmp(property, "ID_SEAT") == 0 ||
        strcmp(property, "WL_OUTPUT") == 0) {

        const char *node = orig_udev_device_get_devnode ? orig_udev_device_get_devnode(udev_device) : NULL;
        const char *sys = orig_udev_device_get_sysname ? orig_udev_device_get_sysname(udev_device) : NULL;

        if ((node && strstr(node, "event2")) || (sys && strstr(sys, "event2"))) {
            if (strcmp(property, "ID_INPUT_TOUCHSCREEN") == 0) return "1";
            if (strcmp(property, "ID_INPUT") == 0) return "1";
            if (strcmp(property, "ID_INPUT_KEYBOARD") == 0) return "0";
            if (strcmp(property, "ID_SEAT") == 0) return "seat0";
            if (strcmp(property, "WL_OUTPUT") == 0) return "DSI-1";
        }
    }

    if (orig_udev_device_get_property_value)
        return orig_udev_device_get_property_value(udev_device, property);
    return NULL;
}

drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id, uint32_t object_type) {
    if (!orig_drmModeObjectGetProperties)
        orig_drmModeObjectGetProperties = dlsym(RTLD_NEXT, "drmModeObjectGetProperties");
    drmModeObjectPropertiesPtr props = orig_drmModeObjectGetProperties(fd, object_id, object_type);
    if (props && object_type == DRM_MODE_OBJECT_PLANE) {
        // Promote Plane 77 to PRIMARY plane so Weston / Phoc has a primary plane to attach its desktop to
        if (object_id == 77) {
            for (uint32_t i = 0; i < props->count_props; i++) {
                if (props->props[i] == 7) { // prop_id 7 is "type"
                    props->prop_values[i] = 1; // DRM_PLANE_TYPE_PRIMARY
                    break;
                }
            }
        }
    }
    return props;
}

int drmGetCap(int fd, uint64_t capability, uint64_t *value) {
    if (capability == DRM_CAP_TIMESTAMP_MONOTONIC) {
        if (value) *value = 1;
        return 0;
    }
    if (!orig_drmGetCap) orig_drmGetCap = dlsym(RTLD_NEXT, "drmGetCap");
    return orig_drmGetCap(fd, capability, value);
}

ssize_t read(int fd, void *buf, size_t count) {
    if (!orig_read) orig_read = dlsym(RTLD_NEXT, "read");
    ssize_t ret = orig_read(fd, buf, count);
    if (count == 9 && ret == 8) {
        ((char *)buf)[8] = 0;
        return 9;
    }
    if (count >= sizeof(struct drm_event_vblank) && ret >= (ssize_t)sizeof(struct drm_event_vblank)) {
        struct drm_event *e = (struct drm_event *)buf;
        if (e->type == DRM_EVENT_VBLANK || e->type == DRM_EVENT_FLIP_COMPLETE) {
            struct drm_event_vblank *vbl = (struct drm_event_vblank *)buf;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            vbl->tv_sec = (uint32_t)now.tv_sec;
            vbl->tv_usec = (uint32_t)(now.tv_nsec / 1000);
        }
    }
    return ret;
}

drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id) {
    ensure_display_active(fd);

    if (!orig_drmModeGetPlane) {
        orig_drmModeGetPlane = dlsym(RTLD_NEXT, "drmModeGetPlane");
    }
    drmModePlanePtr p = orig_drmModeGetPlane(fd, plane_id);
    if (!p || !p->formats || p->count_formats <= 1) {
        return p;
    }

    uint32_t unique_count = 0;
    for (uint32_t i = 0; i < p->count_formats; i++) {
        uint32_t fmt = p->formats[i];
        int duplicate = 0;
        for (uint32_t j = 0; j < unique_count; j++) {
            if (p->formats[j] == fmt) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            p->formats[unique_count++] = fmt;
        }
    }
    p->count_formats = unique_count;
    return p;
}
