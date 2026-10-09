/*
 * mainmirror_display: draws the main CarPlay screen, captured by the hook's mainmirror
 * module, into cluster displayable 3 while the hook marks the mirror active. Idle it
 * holds no Screen/EGL objects, so displayable 3 stays free for AltScreen; when the
 * mirror ends it removes its marker and exits, and carplay_monitor.sh starts it again.
 */

#include "cluster_surface.h"
#include "mainmirror_shm.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OUT_ID 3
#define OUT_W  1440
#define OUT_H  455
#define STALE_S 3
#define BG_W 180
#define BG_H 57

static volatile sig_atomic_t g_quit;
static mm_header_t* g_hdr;
static void* g_shm;
static mm_conf_t g_conf;

static cluster_surface_t* g_cs;
static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_glctx = EGL_NO_CONTEXT;
static GLuint g_prog, g_tex;
static GLint g_u_swap, g_u_tex, g_u_blur, g_u_texel, g_u_dim;
static GLuint g_bg_fbo, g_bg_tex;
static int g_tex_w, g_tex_h;
static int g_ready;

static void logf_(const char* fmt, ...) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    fprintf(stderr, "%02d:%02d:%02d mainmirror: ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void on_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

static int open_shm(void) {
    int fd = shm_open(MM_SHM_NAME, O_RDWR, 0);
    if (fd < 0) return -1;
    void* p = mmap(NULL, MM_SHM_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;
    g_shm = p;
    g_hdr = (mm_header_t*)p;
    return 0;
}

/* Writer alive, publishing, and asking for the mirror. */
static int mirror_wanted(uint32_t* last_beat, time_t* beat_at) {
    if (!g_hdr || g_hdr->magic != MM_MAGIC || g_hdr->version != MM_VERSION) return 0;
    if (!g_hdr->writer_pid || kill((pid_t)g_hdr->writer_pid, 0) != 0) return 0;
    time_t now = time(NULL);
    if (g_hdr->heartbeat != *last_beat) {
        *last_beat = g_hdr->heartbeat;
        *beat_at = now;
    }
    if (now - *beat_at > STALE_S) return 0;
    return g_hdr->active && g_hdr->seq && access(MM_OFF_MARKER, F_OK) != 0 &&
           access(MM_OFF_TMP, F_OK) != 0;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        logf_("shader compile failed: %s", log);
    }
    return s;
}

static int gl_setup(void) {
    static const char* vs =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char* fs =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "uniform float u_swap;\n"
        "uniform float u_blur;\n"
        "uniform vec2 u_texel;\n"
        "uniform float u_dim;\n"
        "void main() {\n"
        "  vec3 c = texture2D(u_tex, v_uv).rgb;\n"
        "  if (u_blur > 0.5) {\n"
        "    c = vec3(0.0);\n"
        "    for (int y = -1; y <= 1; y++)\n"
        "      for (int x = -1; x <= 1; x++)\n"
        "        c += texture2D(u_tex, v_uv + vec2(float(x), float(y)) * u_texel).rgb;\n"
        "    c /= 9.0;\n"
        "  }\n"
        "  gl_FragColor = vec4(mix(c, c.bgr, u_swap) * u_dim, 1.0);\n"
        "}\n";
    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(g_prog, 0, "a_pos");
    glBindAttribLocation(g_prog, 1, "a_uv");
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        logf_("program link failed");
        return -1;
    }
    g_u_swap = glGetUniformLocation(g_prog, "u_swap");
    g_u_tex = glGetUniformLocation(g_prog, "u_tex");
    g_u_blur = glGetUniformLocation(g_prog, "u_blur");
    g_u_texel = glGetUniformLocation(g_prog, "u_texel");
    g_u_dim = glGetUniformLocation(g_prog, "u_dim");
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    /* small offscreen buffer for the blurred side fill */
    glGenTextures(1, &g_bg_tex);
    glBindTexture(GL_TEXTURE_2D, g_bg_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, BG_W, BG_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &g_bg_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_bg_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_bg_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        logf_("side-fill framebuffer incomplete, plain black sides");
        glDeleteFramebuffers(1, &g_bg_fbo);
        g_bg_fbo = 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, OUT_W, OUT_H);
    return 0;
}

static void draw_quad(const GLfloat* quad, GLuint tex, int blur, float tx, float ty, int swap, float dim) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(g_u_tex, 0);
    glUniform1f(g_u_blur, blur ? 1.0f : 0.0f);
    glUniform2f(g_u_texel, tx, ty);
    glUniform1f(g_u_swap, swap ? 1.0f : 0.0f);
    glUniform1f(g_u_dim, dim);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), quad + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static int display_up(void) {
    cluster_surface_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.id = OUT_ID;
    cfg.width = OUT_W;
    cfg.height = OUT_H;
    cfg.format = 8;      /* SCREEN_FORMAT_RGBA8888 */
    cfg.usage = 0x20;    /* SCREEN_USAGE_OPENGL_ES2 */
    cfg.nbuffers = 2;
    cfg.transparent = 0;
    /* the window (and its screen context) must exist before eglGetDisplay on this GPU */
    g_cs = cluster_surface_create(&cfg);
    if (!g_cs) {
        logf_("cluster_surface_create(%d) failed", OUT_ID);
        return -1;
    }
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, NULL, NULL)) {
        logf_("EGL init failed 0x%x", eglGetError());
        return -1;
    }
    EGLint attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
    };
    EGLConfig config;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, attrs, &config, 1, &n) || n == 0) {
        logf_("eglChooseConfig failed 0x%x", eglGetError());
        return -1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_glctx = eglCreateContext(g_dpy, config, EGL_NO_CONTEXT, ctx_attrs);
    if (g_glctx == EGL_NO_CONTEXT) {
        logf_("eglCreateContext failed 0x%x", eglGetError());
        return -1;
    }
    g_surf = eglCreateWindowSurface(g_dpy, config,
                                    (EGLNativeWindowType)cluster_surface_window(g_cs), NULL);
    if (g_surf == EGL_NO_SURFACE || !eglMakeCurrent(g_dpy, g_surf, g_surf, g_glctx)) {
        logf_("EGL surface failed 0x%x", eglGetError());
        return -1;
    }
    eglSwapInterval(g_dpy, 1);
    if (gl_setup() != 0) return -1;
    logf_("displayable %d up %dx%d", OUT_ID, OUT_W, OUT_H);
    return 0;
}

static void display_down(void) {
    if (g_ready) {
        unlink(MM_READY_MARKER);
        g_ready = 0;
    }
    if (g_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_surf != EGL_NO_SURFACE) eglDestroySurface(g_dpy, g_surf);
        if (g_glctx != EGL_NO_CONTEXT) eglDestroyContext(g_dpy, g_glctx);
    }
    g_surf = EGL_NO_SURFACE;
    g_glctx = EGL_NO_CONTEXT;
    if (g_cs) cluster_surface_destroy(g_cs);
    g_cs = NULL;
}

/* Source rectangle (uv) and target rectangle on the cluster (output pixels).
 * crop= fills the whole cluster with part of the frame; otherwise the whole frame is
 * fitted, aspect kept, into the fit= rectangle and the rest stays black. */
static void layout(int fw, int fh, float uv[4], float dst[4]) {
    if (g_conf.crop[2] > 0) {
        float x = (float)g_conf.crop[0], y = (float)g_conf.crop[1];
        float w = (float)g_conf.crop[2], h = (float)g_conf.crop[3];
        if (x + w > fw) w = fw - x;
        if (y + h > fh) h = fh - y;
        uv[0] = x / fw; uv[1] = y / fh; uv[2] = (x + w) / fw; uv[3] = (y + h) / fh;
        dst[0] = 0; dst[1] = 0; dst[2] = OUT_W; dst[3] = OUT_H;
        return;
    }
    uv[0] = 0; uv[1] = 0; uv[2] = 1; uv[3] = 1;
    float bw = (float)g_conf.fit[2], bh = (float)g_conf.fit[3];
    float w = bw, h = bw * fh / fw;
    if (h > bh) { h = bh; w = bh * fw / fh; }
    dst[0] = g_conf.fit[0] + (bw - w) / 2;
    dst[1] = g_conf.fit[1] + (bh - h) / 2;
    dst[2] = dst[0] + w;
    dst[3] = dst[1] + h;
}

static int draw_frame(void) {
    uint32_t idx = g_hdr->front;
    g_hdr->reading = idx;
    __sync_synchronize();
    if (g_hdr->front != idx) {
        idx = g_hdr->front;
        g_hdr->reading = idx;
        __sync_synchronize();
    }
    int fw = (int)g_hdr->w, fh = (int)g_hdr->h;
    if (fw <= 0 || fh <= 0 || fw > MM_MAX_W || fh > MM_MAX_H) {
        g_hdr->reading = MM_NONE;
        return 0;
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    if (fw != g_tex_w || fh != g_tex_h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, mm_frame(g_shm, idx));
        g_tex_w = fw;
        g_tex_h = fh;
        logf_("frames %dx%d", fw, fh);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, mm_frame(g_shm, idx));
    }
    g_hdr->reading = MM_NONE;

    float uv[4], dst[4];
    layout(fw, fh, uv, dst);
    /* output pixels (top-down) to clip space; frame rows are top-down too */
    GLfloat x0 = dst[0] / OUT_W * 2 - 1, x1 = dst[2] / OUT_W * 2 - 1;
    GLfloat y0 = 1 - dst[1] / OUT_H * 2, y1 = 1 - dst[3] / OUT_H * 2;
    const GLfloat quad[] = {
        x0, y1, uv[0], uv[3],
        x1, y1, uv[2], uv[3],
        x0, y0, uv[0], uv[1],
        x1, y0, uv[2], uv[1],
    };
    glUseProgram(g_prog);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (g_conf.bg_blur && g_bg_fbo && g_conf.crop[2] <= 0) {
        /* whole frame squeezed into the small buffer (blurred), then stretched over the
         * cluster and dimmed; the frame itself goes on top */
        static const GLfloat frame_full[] = { -1, -1, 0, 1,  1, -1, 1, 1,  -1, 1, 0, 0,  1, 1, 1, 0 };
        static const GLfloat bg_full[] = { -1, -1, 0, 0,  1, -1, 1, 0,  -1, 1, 0, 1,  1, 1, 1, 1 };
        glBindFramebuffer(GL_FRAMEBUFFER, g_bg_fbo);
        glViewport(0, 0, BG_W, BG_H);
        draw_quad(frame_full, g_tex, 1, 2.0f / BG_W, 2.0f / BG_H, g_conf.swap_rb, 1.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, OUT_W, OUT_H);
        draw_quad(bg_full, g_bg_tex, 1, 1.0f / BG_W, 1.0f / BG_H, 0, g_conf.bg_dim / 100.0f);
    }
    draw_quad(quad, g_tex, 0, 0, 0, g_conf.swap_rb, 1.0f);
    if (!eglSwapBuffers(g_dpy, g_surf)) {
        logf_("eglSwapBuffers failed 0x%x", eglGetError());
        return -1;
    }
    if (!g_ready) {
        int fd = open(MM_READY_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        g_ready = 1;
        logf_("first frame on the cluster, %s set", MM_READY_MARKER);
    }
    return 0;
}

int main(void) {
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGPIPE, SIG_IGN);
    unlink(MM_READY_MARKER);
    mm_conf_load(&g_conf);
    logf_("pid %d waiting (fit=%d,%d,%d,%d crop=%d,%d,%d,%d swap_rb=%d bg_blur=%d bg_dim=%d)", getpid(),
          g_conf.fit[0], g_conf.fit[1], g_conf.fit[2], g_conf.fit[3], g_conf.crop[0], g_conf.crop[1],
          g_conf.crop[2], g_conf.crop[3], g_conf.swap_rb, g_conf.bg_blur, g_conf.bg_dim);

    uint32_t beat = 0;
    time_t beat_at = time(NULL);
    while (!g_quit) {
        if (!g_hdr && open_shm() != 0) {
            sleep(1);
            continue;
        }
        if (mirror_wanted(&beat, &beat_at)) break;
        usleep(250000);
    }
    if (g_quit) return 0;

    logf_("mirror wanted, writer pid %u", g_hdr->writer_pid);
    if (display_up() != 0) {
        display_down();
        sleep(5);
        return 1;
    }

    uint32_t seen = 0, frames = 0;
    time_t last_lost_check = time(NULL), last_stat = time(NULL);
    while (!g_quit && mirror_wanted(&beat, &beat_at)) {
        uint32_t seq = g_hdr->seq;
        if (seq != seen) {
            if (draw_frame() != 0) break;
            seen = seq;
            frames++;
        }
        time_t now = time(NULL);
        if (now - last_lost_check >= 5) {
            last_lost_check = now;
            if (cluster_surface_lost(g_cs)) {
                logf_("window lost, restarting");
                break;
            }
        }
        if (now - last_stat >= 30) {
            logf_("%u frames in %lds", frames, (long)(now - last_stat));
            frames = 0;
            last_stat = now;
        }
        usleep(20000);
    }
    logf_("mirror ended");
    display_down();
    return 0;
}
