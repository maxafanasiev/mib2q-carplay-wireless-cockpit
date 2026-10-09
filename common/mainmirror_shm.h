#ifndef MAINMIRROR_SHM_H
#define MAINMIRROR_SHM_H

/* Main-screen mirror frames, hook (writer, in dio_manager) -> mainmirror_display (reader). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MM_SHM_NAME     "/carplay_mainmirror"
#define MM_MAGIC        0x31524d4du /* "MMR1" */
#define MM_VERSION      1
#define MM_MAX_W        1280
#define MM_MAX_H        720
#define MM_NBUF         3
#define MM_HDR_BYTES    4096u
#define MM_FRAME_BYTES  ((uint32_t)MM_MAX_W * MM_MAX_H * 4u)
#define MM_SHM_BYTES    (MM_HDR_BYTES + MM_NBUF * MM_FRAME_BYTES)
#define MM_NONE         0xffffffffu

#define MM_READY_MARKER "/tmp/carplay_mainmirror.ready"
#define MM_CONF_PATH    "/mnt/app/root/hooks/mainmirror.conf"
#define MM_OFF_MARKER   "/mnt/app/root/hooks/mainmirror.off"
#define MM_OFF_TMP      "/tmp/carplay_mainmirror_off"

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t writer_pid;
    volatile uint32_t active;   /* writer wants the cluster mirror shown */
    volatile uint32_t seq;      /* bumped after each published frame */
    volatile uint32_t front;    /* newest complete buffer */
    volatile uint32_t reading;  /* buffer the reader is uploading, or MM_NONE */
    volatile uint32_t w, h;     /* frame size in front (stride = w * 4) */
    volatile uint32_t win_w, win_h;
    volatile uint32_t heartbeat; /* writer seconds counter, proves it is alive */
} mm_header_t;

static inline uint8_t* mm_frame(void* base, uint32_t i) {
    return (uint8_t*)base + MM_HDR_BYTES + i * MM_FRAME_BYTES;
}

typedef struct {
    int fps;          /* capture rate */
    int delay_s;      /* main window must post this long with no second window */
    int crop[4];      /* x y w h in frame pixels; set = fill the cluster with that part */
    int fit[4];       /* x y w h on the cluster; the whole frame is fitted into it */
    int swap_rb;      /* Screen RGBA8888 is B,G,R,A in memory */
    int keep_screen;  /* don't tell the phone the car took the screen, so video keeps coming */
    int bg_blur;      /* fill the sides with a blurred, dimmed copy of the frame */
    int bg_dim;       /* brightness of that fill, percent */
    int wheel_invert; /* steering-wheel roller -> CarPlay knob direction */
} mm_conf_t;

/* key=value lines: fps=15, delay=0, swap_rb=1, keep_screen=1, bg_blur=1, bg_dim=45, fit=x,y,w,h (default: between the gauges),
 * crop=x,y,w,h (instead of fit) */
static inline void mm_conf_load(mm_conf_t* c) {
    c->fps = 15;
    c->delay_s = 0;
    memset(c->crop, 0, sizeof c->crop);
    c->fit[0] = 1440 * 22 / 100;
    c->fit[1] = 0;
    c->fit[2] = 1440 * 56 / 100;
    c->fit[3] = 455;
    c->swap_rb = 1;
    c->keep_screen = 1;
    c->bg_blur = 1;
    c->bg_dim = 45;
    c->wheel_invert = 0;
    FILE* f = fopen(MM_CONF_PATH, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int a, b, d, e;
        if (sscanf(line, "fps=%d", &a) == 1 && a >= 1 && a <= 30) c->fps = a;
        else if (sscanf(line, "delay=%d", &a) == 1 && a >= 0 && a <= 120) c->delay_s = a;
        else if (sscanf(line, "swap_rb=%d", &a) == 1) c->swap_rb = a != 0;
        else if (sscanf(line, "keep_screen=%d", &a) == 1) c->keep_screen = a != 0;
        else if (sscanf(line, "bg_blur=%d", &a) == 1) c->bg_blur = a != 0;
        else if (sscanf(line, "wheel_invert=%d", &a) == 1) c->wheel_invert = a != 0;
        else if (sscanf(line, "bg_dim=%d", &a) == 1 && a >= 0 && a <= 100) c->bg_dim = a;
        else if (sscanf(line, "crop=%d,%d,%d,%d", &a, &b, &d, &e) == 4 && a >= 0 && b >= 0 && d > 0 && e > 0) {
            c->crop[0] = a; c->crop[1] = b; c->crop[2] = d; c->crop[3] = e;
        } else if (sscanf(line, "fit=%d,%d,%d,%d", &a, &b, &d, &e) == 4 && a >= 0 && b >= 0 && d > 0 && e > 0) {
            c->fit[0] = a; c->fit[1] = b; c->fit[2] = d; c->fit[3] = e;
        }
    }
    fclose(f);
}

#endif /* MAINMIRROR_SHM_H */
