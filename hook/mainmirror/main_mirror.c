/*
 * Main-screen mirror for sessions without the alt screen (wireless adapters).
 * libairplay (dio::CScreenRender) posts the CarPlay video windows through its PLT, so the
 * exported screen_post_window/screen_destroy_window below see every one of them. The main
 * window is read back and published to MM_SHM_NAME for mainmirror_display, which draws it
 * on the cluster.
 * Off with /tmp/carplay_mainmirror_off or /mnt/app/root/hooks/mainmirror.off.
 */

#include "../framework/common.h"
#include "../framework/logging.h"
#include "../framework/bus.h"
#include "../framework/bus_protocol.h"
#include "../../common/mainmirror_shm.h"
#include "main_mirror.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

DEFINE_LOG_MODULE(MAINMIRROR);

/* libscreen ABI, kept local so the hook still links only libc/libz/libsocket */
typedef void* scr_ctx_t;
typedef void* scr_win_t;
typedef void* scr_pix_t;
typedef void* scr_buf_t;

#define SCR_APPLICATION_CONTEXT 0
#define SCR_PROP_BUFFER_SIZE    5
#define SCR_PROP_FORMAT         14
#define SCR_PROP_ID_STRING      20
#define SCR_PROP_POINTER        34
#define SCR_PROP_RENDER_BUFFERS 37
#define SCR_PROP_SIZE           40
#define SCR_PROP_SOURCE_SIZE    42
#define SCR_PROP_STRIDE         44
#define SCR_PROP_USAGE          48
#define SCR_PROP_VISIBLE        51
#define SCR_PROP_ZORDER         54
#define SCR_FORMAT_RGBA8888     8
#define SCR_USAGE_READ          (1 << 1)
#define SCR_USAGE_WRITE         (1 << 2)
#define SCR_USAGE_NATIVE        (1 << 3)

typedef int (*post_window_f)(scr_win_t, scr_buf_t, int, const int*, int);
typedef int (*destroy_window_f)(scr_win_t);
typedef int (*get_win_iv_f)(scr_win_t, int, int*);
typedef int (*get_win_cv_f)(scr_win_t, int, int, char*);
typedef int (*create_ctx_f)(scr_ctx_t*, int);
typedef int (*create_pix_f)(scr_pix_t*, scr_ctx_t);
typedef int (*set_pix_iv_f)(scr_pix_t, int, const int*);
typedef int (*create_pix_buf_f)(scr_pix_t);
typedef int (*get_pix_pv_f)(scr_pix_t, int, void**);
typedef int (*get_buf_pv_f)(scr_buf_t, int, void**);
typedef int (*get_buf_iv_f)(scr_buf_t, int, int*);
typedef int (*read_window_f)(scr_win_t, scr_buf_t, int, const int*, int);
typedef int (*destroy_pix_f)(scr_pix_t);

static struct {
    post_window_f post;
    destroy_window_f destroy_window;
    get_win_iv_f get_win_iv;
    get_win_cv_f get_win_cv;
    create_ctx_f create_ctx;
    create_pix_f create_pix;
    set_pix_iv_f set_pix_iv;
    create_pix_buf_f create_pix_buf;
    get_pix_pv_f get_pix_pv;
    get_buf_pv_f get_buf_pv;
    get_buf_iv_f get_buf_iv;
    read_window_f read_window;
    destroy_pix_f destroy_pix;
} g_scr;

#define MAX_WINDOWS 8
#define ALT_POSTS   30          /* a second window posting this much means AltScreen video */
#define IDLE_US     3000000u    /* window silent this long: not the posting main window */
#define GRACE_US    8000000u    /* once on, the mirror outlives a window re-create this long */

typedef struct {
    scr_win_t win;
    uint32_t posts;
    uint32_t posts_at_report;
    uint64_t last_post_us, streak_since_us;
    int w, h, src_w, src_h, format, zorder, visible;
    char id[64];
} win_rec_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static win_rec_t g_windows[MAX_WINDOWS];
static volatile int g_stop;
static pthread_t g_worker, g_capture;
static int g_worker_started, g_capture_started;
static int g_alt_seen;
static int g_was_on;
static uint64_t g_last_destroy_us;
static mm_conf_t g_conf;

static mm_header_t* g_hdr;
static void* g_shm;

static scr_ctx_t g_ctx;
static scr_pix_t g_pix;
static int g_pix_w, g_pix_h;


static int mirror_disabled(void) {
    return access(MM_OFF_TMP, F_OK) == 0 || access(MM_OFF_MARKER, F_OK) == 0;
}

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int resolve_screen(void) {
    if (g_scr.post) return 1;
    void* h = dlopen("libscreen.so.1", RTLD_NOW);
    if (!h) return 0;
    g_scr.destroy_window = (destroy_window_f)dlsym(RTLD_NEXT, "screen_destroy_window");
    g_scr.get_win_iv = (get_win_iv_f)dlsym(h, "screen_get_window_property_iv");
    g_scr.get_win_cv = (get_win_cv_f)dlsym(h, "screen_get_window_property_cv");
    g_scr.create_ctx = (create_ctx_f)dlsym(h, "screen_create_context");
    g_scr.create_pix = (create_pix_f)dlsym(h, "screen_create_pixmap");
    g_scr.set_pix_iv = (set_pix_iv_f)dlsym(h, "screen_set_pixmap_property_iv");
    g_scr.create_pix_buf = (create_pix_buf_f)dlsym(h, "screen_create_pixmap_buffer");
    g_scr.get_pix_pv = (get_pix_pv_f)dlsym(h, "screen_get_pixmap_property_pv");
    g_scr.get_buf_pv = (get_buf_pv_f)dlsym(h, "screen_get_buffer_property_pv");
    g_scr.get_buf_iv = (get_buf_iv_f)dlsym(h, "screen_get_buffer_property_iv");
    g_scr.read_window = (read_window_f)dlsym(h, "screen_read_window");
    g_scr.destroy_pix = (destroy_pix_f)dlsym(h, "screen_destroy_pixmap");
    post_window_f post = (post_window_f)dlsym(RTLD_NEXT, "screen_post_window");
    if (!post || !g_scr.destroy_window || !g_scr.get_win_iv || !g_scr.get_win_cv) {
        LOG_WARN(LOG_MODULE, "libscreen symbols missing, mirror off");
        return -1;
    }
    LOG_WARN(LOG_MODULE, "libscreen resolved post=%p destroy=%p read_window=%p",
             (void*)post, (void*)g_scr.destroy_window, (void*)g_scr.read_window);
    g_scr.post = post;
    return 1;
}

/* ---- window bookkeeping (caller holds g_lock) ---------------------------- */

static win_rec_t* window_find(scr_win_t win) {
    for (unsigned i = 0; i < MAX_WINDOWS; i++)
        if (g_windows[i].win == win) return &g_windows[i];
    return NULL;
}

static win_rec_t* window_add(scr_win_t win) {
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        if (!g_windows[i].win) {
            memset(&g_windows[i], 0, sizeof g_windows[i]);
            g_windows[i].win = win;
            return &g_windows[i];
        }
    }
    return NULL;
}

static void describe(win_rec_t* r) {
    int v[2] = { 0, 0 };
    if (g_scr.get_win_iv(r->win, SCR_PROP_SIZE, v) == 0) { r->w = v[0]; r->h = v[1]; }
    v[0] = v[1] = 0;
    if (g_scr.get_win_iv(r->win, SCR_PROP_SOURCE_SIZE, v) == 0) { r->src_w = v[0]; r->src_h = v[1]; }
    g_scr.get_win_iv(r->win, SCR_PROP_FORMAT, &r->format);
    g_scr.get_win_iv(r->win, SCR_PROP_ZORDER, &r->zorder);
    g_scr.get_win_iv(r->win, SCR_PROP_VISIBLE, &r->visible);
    r->id[0] = 0;
    g_scr.get_win_cv(r->win, SCR_PROP_ID_STRING, (int)sizeof r->id, r->id);
    r->id[sizeof r->id - 1] = 0;
}

/* ---- exported wrappers (libairplay binds to these) ------------------------- */

HOOK_EXPORT int screen_post_window(scr_win_t win, scr_buf_t buf, int count, const int* dirty, int flags) {
    if (!g_scr.post) {
        static post_window_f real;
        if (!real) real = (post_window_f)dlsym(RTLD_NEXT, "screen_post_window");
        return real ? real(win, buf, count, dirty, flags) : -1;
    }
    int rc = g_scr.post(win, buf, count, dirty, flags);
    if (pthread_mutex_trylock(&g_lock) != 0) return rc;
    win_rec_t* r = window_find(win);
    if (!r && (r = window_add(win)) != NULL) {
        describe(r);
        LOG_WARN(LOG_MODULE, "window %p id='%s' size=%dx%d src=%dx%d fmt=%d z=%d visible=%d",
                 win, r->id, r->w, r->h, r->src_w, r->src_h, r->format, r->zorder, r->visible);
    }
    if (r) {
        uint64_t t = now_us();
        if (!r->streak_since_us || t - r->last_post_us > IDLE_US) r->streak_since_us = t;
        r->last_post_us = t;
        r->posts++;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

HOOK_EXPORT int screen_destroy_window(scr_win_t win) {
    if (!g_scr.destroy_window) {
        static destroy_window_f real;
        if (!real) real = (destroy_window_f)dlsym(RTLD_NEXT, "screen_destroy_window");
        return real ? real(win) : -1;
    }
    pthread_mutex_lock(&g_lock);
    win_rec_t* r = window_find(win);
    if (r) {
        LOG_WARN(LOG_MODULE, "window %p id='%s' destroyed after %u posts", win, r->id, r->posts);
        memset(r, 0, sizeof *r);
        g_last_destroy_us = now_us();
    }
    pthread_mutex_unlock(&g_lock);
    return g_scr.destroy_window(win);
}

/* ---- capture ------------------------------------------------------------- */

static int ensure_pixmap(int w, int h) {
    if (g_pix && g_pix_w == w && g_pix_h == h) return 0;
    if (!g_scr.create_pix || !g_scr.set_pix_iv || !g_scr.create_pix_buf || !g_scr.get_pix_pv ||
        !g_scr.read_window || !g_scr.get_buf_pv || !g_scr.get_buf_iv)
        return -1;
    if (!g_ctx && g_scr.create_ctx(&g_ctx, SCR_APPLICATION_CONTEXT) != 0) {
        LOG_WARN(LOG_MODULE, "screen_create_context failed errno=%d", errno);
        g_ctx = NULL;
        return -1;
    }
    if (g_pix && g_scr.destroy_pix) g_scr.destroy_pix(g_pix);
    g_pix = NULL;
    if (g_scr.create_pix(&g_pix, g_ctx) != 0) {
        LOG_WARN(LOG_MODULE, "screen_create_pixmap failed errno=%d", errno);
        g_pix = NULL;
        return -1;
    }
    int fmt = SCR_FORMAT_RGBA8888;
    int usage = SCR_USAGE_READ | SCR_USAGE_WRITE | SCR_USAGE_NATIVE;
    int size[2] = { w, h };
    if (g_scr.set_pix_iv(g_pix, SCR_PROP_FORMAT, &fmt) != 0 ||
        g_scr.set_pix_iv(g_pix, SCR_PROP_USAGE, &usage) != 0 ||
        g_scr.set_pix_iv(g_pix, SCR_PROP_BUFFER_SIZE, size) != 0 ||
        g_scr.create_pix_buf(g_pix) != 0) {
        LOG_WARN(LOG_MODULE, "pixmap %dx%d setup failed errno=%d", w, h, errno);
        if (g_scr.destroy_pix) g_scr.destroy_pix(g_pix);
        g_pix = NULL;
        return -1;
    }
    g_pix_w = w;
    g_pix_h = h;
    LOG_WARN(LOG_MODULE, "capture pixmap %dx%d ready", w, h);
    return 0;
}

/* Read `r` into the pixmap. Caller holds g_lock, so libairplay can't destroy the window
 * meanwhile. Returns the pixel pointer and stride, or NULL. */
static const uint8_t* grab(win_rec_t* r, int* out_w, int* out_h, int* out_stride, uint32_t* took_us) {
    int w = r->w, h = r->h;
    if (w <= 0 || h <= 0) return NULL;
    if (w > MM_MAX_W) { h = h * MM_MAX_W / w; w = MM_MAX_W; }
    if (h > MM_MAX_H) { w = w * MM_MAX_H / h; h = MM_MAX_H; }
    w &= ~1;
    if (ensure_pixmap(w, h) != 0) return NULL;
    scr_buf_t buf = NULL;
    if (g_scr.get_pix_pv(g_pix, SCR_PROP_RENDER_BUFFERS, &buf) != 0 || !buf) return NULL;
    uint64_t t0 = now_us();
    int rc = g_scr.read_window(r->win, buf, 0, NULL, 0);
    *took_us = (uint32_t)(now_us() - t0);
    if (rc != 0) {
        static int warned;
        if (warned++ < 5)
            LOG_WARN(LOG_MODULE, "read_window '%s' rc=%d errno=%d", r->id, rc, errno);
        return NULL;
    }
    void* ptr = NULL;
    int stride = 0;
    g_scr.get_buf_pv(buf, SCR_PROP_POINTER, &ptr);
    g_scr.get_buf_iv(buf, SCR_PROP_STRIDE, &stride);
    if (!ptr || stride < w * 4) return NULL;
    *out_w = w;
    *out_h = h;
    *out_stride = stride;
    return ptr;
}

static void publish(const uint8_t* px, int w, int h, int stride) {
    if (!g_hdr) return;
    uint32_t front = g_hdr->front, reading = g_hdr->reading, idx = 0;
    while (idx == front || idx == reading) idx++;
    uint8_t* dst = mm_frame(g_shm, idx);
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)w * 4 * y, px + (size_t)stride * y, (size_t)w * 4);
    g_hdr->w = (uint32_t)w;
    g_hdr->h = (uint32_t)h;
    __sync_synchronize();
    g_hdr->front = idx;
    __sync_synchronize();
    g_hdr->seq++;
}

static int open_shm(void) {
    int fd = shm_open(MM_SHM_NAME, O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        LOG_WARN(LOG_MODULE, "shm_open %s failed errno=%d", MM_SHM_NAME, errno);
        return -1;
    }
    if (ftruncate(fd, MM_SHM_BYTES) != 0) {
        LOG_WARN(LOG_MODULE, "ftruncate shm failed errno=%d", errno);
        close(fd);
        return -1;
    }
    void* p = mmap(NULL, MM_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        LOG_WARN(LOG_MODULE, "mmap shm failed errno=%d", errno);
        return -1;
    }
    g_shm = p;
    g_hdr = (mm_header_t*)p;
    g_hdr->active = 0;
    g_hdr->reading = MM_NONE;
    g_hdr->front = 0;
    g_hdr->w = g_hdr->h = 0;
    g_hdr->writer_pid = (uint32_t)getpid();
    g_hdr->version = MM_VERSION;
    __sync_synchronize();
    g_hdr->magic = MM_MAGIC;
    return 0;
}

/* Caller holds g_lock. Picks the main window and decides whether the mirror is wanted. */
static win_rec_t* pick_main(uint64_t t, int* want) {
    win_rec_t* mainwin = NULL;
    unsigned busy = 0;
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        win_rec_t* r = &g_windows[i];
        if (!r->win) continue;
        if (r->posts >= ALT_POSTS) busy++;
        if (t - r->last_post_us < IDLE_US && (!mainwin || r->posts > mainwin->posts)) mainwin = r;
    }
    if (busy >= 2 && !g_alt_seen) {
        g_alt_seen = 1;
        LOG_WARN(LOG_MODULE, "second video window seen (AltScreen), mirror off for this session");
    }
    int allowed = !g_alt_seen && access("/tmp/mmi-mirror-basevideo.ready", F_OK) != 0 && !mirror_disabled();
    if (!g_was_on) {
        *want = allowed && mainwin &&
                t - mainwin->streak_since_us >= (uint64_t)g_conf.delay_s * 1000000u;
        if (*want) g_was_on = 1;
    } else {
        /* CarPlay re-creates its window now and then: keep the last frame up meanwhile */
        int any = 0;
        for (unsigned i = 0; i < MAX_WINDOWS; i++)
            if (g_windows[i].win) any = 1;
        *want = allowed && (any || t - g_last_destroy_us < GRACE_US);
    }
    return mainwin;
}

static void* capture_loop(void* arg) {
    (void)arg;
    uint32_t last_posts = 0, frames = 0, slow = 0, took_max = 0;
    uint64_t last_beat = 0, last_stat = 0;
    scr_win_t last_win = NULL;
    int was_active = 0;
    while (!g_stop) {
        usleep(1000000u / (unsigned)g_conf.fps);
        if (!g_scr.post) continue;
        uint64_t t = now_us();
        int want = 0;
        pthread_mutex_lock(&g_lock);
        win_rec_t* mainwin = pick_main(t, &want);
        int fresh = mainwin && (mainwin->win != last_win || mainwin->posts != last_posts);
        if (want && fresh) {
            int w, h, stride;
            uint32_t took = 0;
            if (mainwin->w <= 0) describe(mainwin);
            const uint8_t* px = grab(mainwin, &w, &h, &stride, &took);
            if (px) {
                publish(px, w, h, stride);
                last_posts = mainwin->posts;
                last_win = mainwin->win;
                frames++;
                if (took > took_max) took_max = took;
                if (took > 1000000u / (unsigned)g_conf.fps) slow++;
            }
        }
        pthread_mutex_unlock(&g_lock);

        if (g_hdr) {
            if (want != was_active) {
                LOG_WARN(LOG_MODULE, "mirror %s", want ? "ON" : "off");
                was_active = want;
            }
            g_hdr->active = (uint32_t)want;
            if (t - last_beat >= 1000000u) { g_hdr->heartbeat++; last_beat = t; }
        }
        if (t - last_stat >= 30000000u) {
            if (frames)
                LOG_WARN(LOG_MODULE, "capture: %u frames in 30s, slowest %u us, %u over budget",
                         frames, took_max, slow);
            frames = slow = took_max = 0;
            last_stat = t;
        }
    }
    if (g_hdr) g_hdr->active = 0;
    return NULL;
}

/* ---- worker -------------------------------------------------------------- */

static void report_rates(void) {
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < MAX_WINDOWS; i++) {
        win_rec_t* r = &g_windows[i];
        if (!r->win || r->posts == r->posts_at_report) continue;
        LOG_WARN(LOG_MODULE, "window %p id='%s' posts=%u (+%u in 30s)", r->win, r->id,
                 r->posts, r->posts - r->posts_at_report);
        r->posts_at_report = r->posts;
    }
    pthread_mutex_unlock(&g_lock);
}

static void* worker(void* arg) {
    (void)arg;
    unsigned tick = 0;
    while (!g_stop) {
        sleep(1);
        if (g_stop) break;
        int ok = resolve_screen();
        if (ok < 0) break;
        if (ok > 0 && !g_capture_started && open_shm() == 0) {
            if (pthread_create(&g_capture, NULL, capture_loop, NULL) == 0) g_capture_started = 1;
        }
        if (++tick % 30 == 0) report_rates();
    }
    return NULL;
}

/* ---- screen ownership ------------------------------------------------------
 * When the car shows its own UI (camera, menus, screen retracted) it tells the phone
 * "screen: take/borrow" and the phone stops sending video, so the cluster mirror
 * freezes. In wireless mode we drop the screen part of those requests (and the matching
 * untake/unborrow); audio and the rest go through unchanged. Layout of AirPlayModeChanges
 * checked against AirPlayCreateModesDictionary in /eso/lib/libairplay.so (MU1329). */
enum { MODE_NA = 0, MODE_TAKE = 1, MODE_UNTAKE = 2, MODE_BORROW = 3, MODE_UNBORROW = 4 };
typedef struct {
    int32_t screen[4];      /* type, priority, take constraint, borrow/unborrow constraint */
    int32_t main_audio[4];
    int32_t speech;
    int32_t phone_call;
    int32_t turn_by_turn;
} airplay_mode_changes_t;
typedef int32_t (*change_modes_f)(void*, const airplay_mode_changes_t*, const void*, void*, void*);

static volatile int g_mirror_module_on;
static int g_screen_withheld;

HOOK_EXPORT int32_t AirPlayReceiverSessionChangeModes(void* session, const airplay_mode_changes_t* changes,
                                                     const void* reason, void* completion, void* ctx) {
    static change_modes_f real;
    if (!real) real = (change_modes_f)dlsym(RTLD_NEXT, "AirPlayReceiverSessionChangeModes");
    if (!real) return -6700;
    if (!changes || !g_mirror_module_on || !g_conf.keep_screen) return real(session, changes, reason, completion, ctx);
    airplay_mode_changes_t c = *changes;
    int type = c.screen[0];
    if (type == MODE_TAKE || type == MODE_BORROW) {
        g_screen_withheld = 1;
        c.screen[0] = MODE_NA;
        LOG_WARN(LOG_MODULE, "screen %s withheld from the phone, video keeps coming",
                 type == MODE_TAKE ? "take" : "borrow");
    } else if ((type == MODE_UNTAKE || type == MODE_UNBORROW) && g_screen_withheld) {
        g_screen_withheld = 0;
        c.screen[0] = MODE_NA;
        LOG_WARN(LOG_MODULE, "screen %s withheld (phone never lost the screen)",
                 type == MODE_UNTAKE ? "untake" : "unborrow");
    }
    return real(session, &c, reason, completion, ctx);
}

/* ---- steering-wheel roller as CarPlay knob -----------------------------------
 * dio creates each virtual HID device first and builds its descriptor after, so the knob is
 * the device created just before HIDKnobCreateDescriptor (car logs: the real knob posts to
 * it). A wheel report dio posts itself also pins it. We post wheel reports the same way
 * dio does for the MMI knob (HIDKnobFillReport: 4 bytes [buttons, x, y, wheel]). Signatures
 * checked in /eso/lib/libairplay.so (MU1329). */
typedef int (*knob_desc_f)(void*, void*, void*);
typedef int (*create_virtual_f)(void**, void*);
typedef void (*knob_fill_f)(uint8_t*, int, int, int, int, int, int, const uint8_t*);
typedef int (*post_report_f)(void*, const void*, size_t);

static void* volatile g_last_virtual;
static void* volatile g_knob_dev;

HOOK_EXPORT int HIDKnobCreateDescriptor(void* caps, void* out_desc, void* out_len) {
    static knob_desc_f real;
    if (!real) real = (knob_desc_f)dlsym(RTLD_NEXT, "HIDKnobCreateDescriptor");
    if (!real) return -6700;
    int rc = real(caps, out_desc, out_len);
    if (rc == 0 && g_last_virtual && !g_knob_dev) {
        g_knob_dev = g_last_virtual;
        LOG_WARN(LOG_MODULE, "CarPlay knob device %p", g_knob_dev);
    }
    return rc;
}

HOOK_EXPORT int HIDDeviceCreateVirtual(void** out_device, void* properties) {
    static create_virtual_f real;
    if (!real) real = (create_virtual_f)dlsym(RTLD_NEXT, "HIDDeviceCreateVirtual");
    if (!real) return -6700;
    int rc = real(out_device, properties);
    if (rc == 0 && out_device) g_last_virtual = *out_device;
    return rc;
}

/* a wheel report dio posts itself pins the knob device */
static volatile int g_in_inject;

HOOK_EXPORT int HIDDevicePostReport(void* device, const void* report, size_t len) {
    static post_report_f real;
    if (!real) real = (post_report_f)dlsym(RTLD_NEXT, "HIDDevicePostReport");
    if (!real) return -6700;
    int rc = real(device, report, len);
    if (!g_in_inject && report && len == 4 && ((const uint8_t*)report)[3] && device != g_knob_dev) {
        g_knob_dev = device;
        LOG_WARN(LOG_MODULE, "CarPlay knob device %p (from a real knob turn)", device);
    }
    return rc;
}

static void mirror_on_knob(uint16_t type, uint8_t flags, const uint8_t* payload, uint32_t len, void* ctx) {
    (void)type; (void)flags; (void)ctx;
    static knob_fill_f fill;
    static post_report_f post;
    if (len < 1 || !g_knob_dev) return;
    if (!fill) fill = (knob_fill_f)dlsym(RTLD_DEFAULT, "HIDKnobFillReport");
    if (!post) post = (post_report_f)dlsym(RTLD_NEXT, "HIDDevicePostReport");
    if (!fill || !post) return;
    int steps = (int8_t)payload[0];
    if (g_conf.wheel_invert) steps = -steps;
    /* direction checked in the car: positive MapScale step = knob clockwise */
    int wheel = steps > 0 ? 1 : -1;
    int count = steps < 0 ? -steps : steps;
    if (count > 8) count = 8;
    static const uint8_t no_caps[2] = { 0, 0 };
    uint8_t report[4];
    int rc = 0;
    g_in_inject = 1;
    for (int i = 0; i < count; i++) {
        fill(report, 0, 0, 0, 0, 0, wheel, no_caps);
        rc |= post(g_knob_dev, report, sizeof report);
    }
    fill(report, 0, 0, 0, 0, 0, 0, no_caps);
    rc |= post(g_knob_dev, report, sizeof report);
    g_in_inject = 0;
    LOG_INFO(LOG_MODULE, "roller -> knob wheel %+d x%d rc=%d", wheel, count, rc);
}

static void mirror_on_select(uint16_t type, uint8_t flags, const uint8_t* payload, uint32_t len, void* ctx) {
    (void)type; (void)flags; (void)ctx;
    static post_report_f post;
    if (len < 1 || !g_knob_dev) return;
    if (!post) post = (post_report_f)dlsym(RTLD_NEXT, "HIDDevicePostReport");
    if (!post) return;
    /* button bits exactly as dio posts them for the real knob (select 0x01, back 0x04) */
    uint8_t report[4] = { payload[0], 0, 0, 0 };
    g_in_inject = 1;
    int rc = post(g_knob_dev, report, sizeof report);
    g_in_inject = 0;
    LOG_INFO(LOG_MODULE, "wheel key -> knob buttons %02x rc=%d", report[0], rc);
}

/* AltScreen interposes AirPlayReceiverSessionSetup; with it loaded the cluster belongs to
 * its video (wired mode), so the mirror stays off. */
static int altscreen_loaded(void) {
    void* p = dlsym(RTLD_DEFAULT, "AirPlayReceiverSessionSetup");
    Dl_info info;
    return p && dladdr(p, &info) && info.dli_fname && strstr(info.dli_fname, "altscreen");
}

static void main_mirror_init(void) {
    if (mirror_disabled()) {
        LOG_WARN(LOG_MODULE, "disabled by marker file");
        return;
    }
    if (altscreen_loaded()) {
        LOG_WARN(LOG_MODULE, "AltScreen loaded (wired mode), mirror off");
        return;
    }
    mm_conf_load(&g_conf);
    g_mirror_module_on = 1;
    bus_on(CMD_MIRROR_KNOB, mirror_on_knob, NULL);
    bus_on(CMD_MIRROR_SELECT, mirror_on_select, NULL);
    if (pthread_create(&g_worker, NULL, worker, NULL) == 0) {
        g_worker_started = 1;
        LOG_WARN(LOG_MODULE, "started fps=%d delay=%ds", g_conf.fps, g_conf.delay_s);
    }
}

static void main_mirror_shutdown(void) {
    if (g_mirror_module_on) {
        bus_off(CMD_MIRROR_KNOB);
        bus_off(CMD_MIRROR_SELECT);
    }
    g_stop = 1;
    if (g_capture_started) pthread_join(g_capture, NULL);
    if (g_worker_started) pthread_join(g_worker, NULL);
    if (g_hdr) g_hdr->active = 0;
}

const hook_module_def_t main_mirror_module_def = {
    .name = "mainmirror",
    .priority = HOOK_PRIORITY_LOW,
    .on_init = main_mirror_init,
    .on_shutdown = main_mirror_shutdown,
};
