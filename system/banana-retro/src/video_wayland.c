#define _GNU_SOURCE

#include "video_wayland.h"
#include "banana-retro-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { SLOT_COUNT = 2, MAX_DIMENSION = 4096, MAX_POOL = 64 * 1024 * 1024, WAIT_MS = 3000 };

struct slot {
    struct wl_buffer *buffer;
    uint32_t *pixels;
    unsigned width, height;
    bool busy;
};

struct wayland_video {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct banana_retro_manager_v1 *manager;
    struct wl_surface *surface;
    struct xdg_surface *xdg;
    struct xdg_toplevel *toplevel;
    struct banana_retro_surface_v1 *native;
    struct wl_shm_pool *pool;
    struct wl_callback *sync, *frame;
    struct slot slots[SLOT_COUNT], *latest;
    size_t slot_size;
    unsigned max_width, max_height;
    uint32_t capabilities;
    bool configured, closed, presented;
};

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Pump both directions with a single deadline, including when the socket send queue is full. */
static bool wait_events(struct wayland_video *v, int64_t deadline)
{
    struct pollfd fd = { .fd = wl_display_get_fd(v->display), .events = POLLIN };
    int64_t now = monotonic_ms();
    if (now < 0 || now >= deadline) {
        errno = ETIMEDOUT;
        return false;
    }
    if (wl_display_flush(v->display) < 0) {
        if (errno != EAGAIN)
            return false;
        fd.events |= POLLOUT;
    }
    int n = poll(&fd, 1, (int)(deadline - now));
    if (n < 0 && errno == EINTR)
        return true;
    if (n <= 0) {
        if (n == 0)
            errno = ETIMEDOUT;
        return false;
    }
    if (fd.revents & (POLLIN | POLLERR | POLLHUP))
        return wl_display_dispatch(v->display) >= 0;
    if (fd.revents & POLLNVAL) {
        errno = EBADF;
        return false;
    }
    return true;
}

static bool wait_ready(struct wayland_video *v, bool configuring)
{
    int64_t start = monotonic_ms();
    if (start < 0)
        return false;
    for (;;) {
        if (wl_display_dispatch_pending(v->display) < 0)
            return false;
        if (v->closed) {
            fputs("Native Wayland surface closed\n", stderr);
            return false;
        }
        if (!v->sync && !v->frame && (!configuring || v->configured))
            return true;
        if (!wait_events(v, start + WAIT_MS)) {
            perror("Native Wayland session");
            return false;
        }
    }
}

static void sync_done(void *data, struct wl_callback *cb, uint32_t serial)
{
    struct wayland_video *v = data;
    (void)serial;
    wl_callback_destroy(cb);
    v->sync = NULL;
}
static const struct wl_callback_listener sync_listener = { sync_done };

static bool sync_display(struct wayland_video *v)
{
    v->sync = wl_display_sync(v->display);
    return v->sync && wl_callback_add_listener(v->sync, &sync_listener, v) == 0 &&
           wait_ready(v, false);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    struct wayland_video *v = data;
    (void)time;
    wl_callback_destroy(cb);
    v->frame = NULL;
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void capabilities(void *data, struct banana_retro_manager_v1 *manager, uint32_t flags)
{
    struct wayland_video *v = data;
    (void)manager;
    v->capabilities = flags;
}
static const struct banana_retro_manager_v1_listener manager_listener = { capabilities };

static void registry_add(void *data, struct wl_registry *registry, uint32_t name,
                         const char *iface, uint32_t version)
{
    struct wayland_video *v = data;
    (void)version;
    if (strcmp(iface, wl_compositor_interface.name) == 0 && !v->compositor)
        v->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    else if (strcmp(iface, wl_shm_interface.name) == 0 && !v->shm)
        v->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0 && !v->wm) {
        v->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        if (v->wm)
            xdg_wm_base_add_listener(v->wm, &wm_listener, v);
    } else if (strcmp(iface, banana_retro_manager_v1_interface.name) == 0 && !v->manager) {
        v->manager = wl_registry_bind(registry, name, &banana_retro_manager_v1_interface, 1);
        if (v->manager)
            banana_retro_manager_v1_add_listener(v->manager, &manager_listener, v);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener registry_listener = { registry_add, registry_remove };

static void surface_configure(void *data, struct xdg_surface *xdg, uint32_t serial)
{
    struct wayland_video *v = data;
    xdg_surface_ack_configure(xdg, serial);
    v->configured = true;
}
static const struct xdg_surface_listener surface_listener = { surface_configure };

static void top_configure(void *data, struct xdg_toplevel *top, int32_t width,
                          int32_t height, struct wl_array *states)
{
    /* The compositor fits source pixels to the configured output. */
    (void)data;
    (void)top;
    (void)width;
    (void)height;
    (void)states;
}

static void top_close(void *data, struct xdg_toplevel *top)
{
    struct wayland_video *v = data;
    (void)top;
    v->closed = true;
}
static const struct xdg_toplevel_listener top_listener = {
    .configure = top_configure, .close = top_close
};

static void first_presented(void *data, struct banana_retro_surface_v1 *native, uint32_t time)
{
    struct wayland_video *v = data;
    (void)native;
    (void)time;
    if (!v->closed && !v->presented) {
        v->presented = true;
        puts("Native Wayland frame presented");
        fflush(stdout);
    }
}

static void frame_failed(void *data, struct banana_retro_surface_v1 *native)
{
    (void)data;
    (void)native;
    fputs("Native Wayland frame dropped by compositor\n", stderr);
}

static void surface_closed(void *data, struct banana_retro_surface_v1 *native)
{
    struct wayland_video *v = data;
    (void)native;
    v->closed = true;
}

static const struct banana_retro_surface_v1_listener native_listener = {
    first_presented, frame_failed, surface_closed
};

struct wayland_video *wayland_video_open(void)
{
    struct wayland_video *v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->display = wl_display_connect(NULL);
    if (!v->display) {
        perror("Connecting native Wayland client");
        goto fail;
    }
    v->registry = wl_display_get_registry(v->display);
    if (!v->registry || wl_registry_add_listener(v->registry, &registry_listener, v) < 0 ||
        !sync_display(v) || !v->compositor || !v->shm || !v->wm || !v->manager ||
        !sync_display(v) || !(v->capabilities & BANANA_RETRO_MANAGER_V1_CAPABILITY_XRGB8888_SHM)) {
        fputs("Native Wayland XRGB8888 SHM presentation unavailable\n", stderr);
        goto fail;
    }

    v->surface = wl_compositor_create_surface(v->compositor);
    if (!v->surface)
        goto fail;
    v->xdg = xdg_wm_base_get_xdg_surface(v->wm, v->surface);
    if (!v->xdg || xdg_surface_add_listener(v->xdg, &surface_listener, v) < 0)
        goto fail;
    v->toplevel = xdg_surface_get_toplevel(v->xdg);
    if (!v->toplevel || xdg_toplevel_add_listener(v->toplevel, &top_listener, v) < 0)
        goto fail;
    xdg_toplevel_set_app_id(v->toplevel, "banana-retro");
    xdg_toplevel_set_fullscreen(v->toplevel, NULL);
    v->native = banana_retro_manager_v1_get_surface(v->manager, v->surface);
    if (!v->native || banana_retro_surface_v1_add_listener(v->native, &native_listener, v) < 0)
        goto fail;
    wl_surface_commit(v->surface);
    if (wait_ready(v, true))
        return v;

fail:
    wayland_video_close(v);
    return NULL;
}

bool wayland_video_configure(struct wayland_video *v, const struct retro_game_geometry *geometry)
{
    unsigned w = geometry->max_width, h = geometry->max_height;
    if (v->pool || !w || !h || w > MAX_DIMENSION || h > MAX_DIMENSION ||
        (size_t)w * h > MAX_POOL / SLOT_COUNT / sizeof(uint32_t)) {
        fputs("Native video dimensions exceed the SHM pool limit\n", stderr);
        return false;
    }
    v->slot_size = (size_t)w * h * sizeof(uint32_t);
    size_t length = SLOT_COUNT * v->slot_size;
    int fd = memfd_create("banana-retro", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        perror("Creating native video SHM");
        return false;
    }
    if (ftruncate(fd, (off_t)length) < 0 ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) {
        perror("Sizing native video SHM");
        close(fd);
        return false;
    }
    void *map = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        perror("Mapping native video SHM");
        close(fd);
        return false;
    }
    v->pool = wl_shm_create_pool(v->shm, fd, (int32_t)length);
    close(fd);
    v->max_width = w;
    v->max_height = h;
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
        v->slots[i].pixels = (uint32_t *)((char *)map + i * v->slot_size);
    return v->pool != NULL;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct slot *slot = data;
    (void)buffer;
    slot->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { buffer_release };

static struct slot *free_slot(struct wayland_video *v, bool duplicate)
{
    if (duplicate && v->latest && !v->latest->busy)
        return v->latest;
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
        if (!v->slots[i].busy)
            return &v->slots[i];
    return NULL;
}

static bool resize_slot(struct wayland_video *v, struct slot *slot, unsigned w, unsigned h)
{
    if (slot->buffer && slot->width == w && slot->height == h)
        return true;
    if (slot->buffer)
        wl_buffer_destroy(slot->buffer);
    int32_t offset = (int32_t)((size_t)(slot - v->slots) * v->slot_size);
    slot->buffer = wl_shm_pool_create_buffer(v->pool, offset, (int32_t)w, (int32_t)h,
                                            (int32_t)(w * sizeof(uint32_t)), WL_SHM_FORMAT_XRGB8888);
    slot->width = w;
    slot->height = h;
    return slot->buffer && wl_buffer_add_listener(slot->buffer, &buffer_listener, slot) == 0;
}

static void convert_frame(uint32_t *dest, const void *pixels, unsigned w, unsigned h,
                          size_t pitch, enum retro_pixel_format format)
{
    const unsigned char *src = pixels;
    for (unsigned y = 0; y < h; ++y, dest += w) {
        const unsigned char *row = src + (size_t)y * pitch;
        if (format == RETRO_PIXEL_FORMAT_XRGB8888) {
            memcpy(dest, row, w * sizeof(*dest));
            continue;
        }
        unsigned green_bits = format == RETRO_PIXEL_FORMAT_RGB565 ? 6 : 5;
        for (unsigned x = 0; x < w; ++x) {
            uint16_t pixel;
            memcpy(&pixel, row + x * sizeof(pixel), sizeof(pixel));
            uint32_t red = (pixel >> (5 + green_bits)) & 31;
            uint32_t green = (pixel >> 5) & ((1u << green_bits) - 1);
            uint32_t blue = pixel & 31;
            red = (red << 3) | (red >> 2);
            green = (green << (8 - green_bits)) | (green >> (2 * green_bits - 8));
            blue = (blue << 3) | (blue >> 2);
            dest[x] = (red << 16) | (green << 8) | blue;
        }
    }
}

bool wayland_video_submit(void *data, const void *pixels, unsigned w, unsigned h, size_t pitch,
                          enum retro_pixel_format format, const struct retro_game_geometry *geometry,
                          unsigned rotation)
{
    struct wayland_video *v = data;
    if (v->closed || !v->configured || !v->pool || rotation > 3)
        return false;
    if (pixels) {
        size_t bpp = format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
        if (pixels == RETRO_HW_FRAME_BUFFER_VALID || !w || !h ||
            w > v->max_width || h > v->max_height || w > pitch / bpp || h > SIZE_MAX / pitch ||
            (format != RETRO_PIXEL_FORMAT_XRGB8888 && format != RETRO_PIXEL_FORMAT_0RGB1555 &&
             format != RETRO_PIXEL_FORMAT_RGB565))
            return false;
    } else {
        if (!v->latest)
            return true;
        w = v->latest->width;
        h = v->latest->height;
    }

    if (v->frame)
        return true;
    struct slot *slot = free_slot(v, !pixels);
    if (!slot)
        return true;
    if (!resize_slot(v, slot, w, h))
        return false;
    if (pixels)
        convert_frame(slot->pixels, pixels, w, h, pitch, format);
    else if (slot != v->latest)
        memcpy(slot->pixels, v->latest->pixels, (size_t)w * h * sizeof(uint32_t));

    uint32_t aw = geometry->base_width, ah = geometry->base_height;
    double aspect = geometry->aspect_ratio;
    /* Use bounded integer components for transport, retaining the orientation of the core image. */
    if (isfinite(aspect) && aspect > 0) {
        const uint32_t scale = 1u << 24;
        double part = aspect >= 1 ? scale / aspect : scale * aspect;
        uint32_t component = part < 1 ? 1 : (uint32_t)(part + 0.5);
        aw = aspect >= 1 ? scale : component;
        ah = aspect >= 1 ? component : scale;
    }
    if (!aw || !ah) {
        aw = w;
        ah = h;
    }

    v->frame = wl_surface_frame(v->surface);
    if (!v->frame || wl_callback_add_listener(v->frame, &frame_listener, v) < 0)
        return false;
    banana_retro_surface_v1_set_geometry(v->native, aw, ah, rotation);
    wl_surface_attach(v->surface, slot->buffer, 0, 0);
    wl_surface_damage(v->surface, 0, 0, (int32_t)w, (int32_t)h);
    wl_surface_commit(v->surface);
    slot->busy = true;
    v->latest = slot;
    return true;
}

bool wayland_video_wait(struct wayland_video *v)
{
    /* A tick with no commit may still need to service close, ping and release events. */
    if (!v->frame)
        return sync_display(v);
    return wait_ready(v, false);
}

bool wayland_video_presented(const struct wayland_video *v)
{
    return v->presented;
}

void wayland_video_close(struct wayland_video *v)
{
    if (!v)
        return;
    if (v->sync) wl_callback_destroy(v->sync);
    if (v->frame) wl_callback_destroy(v->frame);
    if (v->native) banana_retro_surface_v1_destroy(v->native);
    if (v->toplevel) xdg_toplevel_destroy(v->toplevel);
    if (v->xdg) xdg_surface_destroy(v->xdg);
    if (v->surface) wl_surface_destroy(v->surface);
    for (unsigned i = 0; i < SLOT_COUNT; ++i)
        if (v->slots[i].buffer) wl_buffer_destroy(v->slots[i].buffer);
    if (v->pool) wl_shm_pool_destroy(v->pool);
    if (v->manager) banana_retro_manager_v1_destroy(v->manager);
    if (v->wm) xdg_wm_base_destroy(v->wm);
    if (v->shm) wl_shm_destroy(v->shm);
    if (v->compositor) wl_compositor_destroy(v->compositor);
    if (v->registry) wl_registry_destroy(v->registry);
    if (v->display) {
        wl_display_flush(v->display);
        wl_display_disconnect(v->display);
    }
    if (v->slots[0].pixels)
        munmap(v->slots[0].pixels, SLOT_COUNT * v->slot_size);
    free(v);
}
