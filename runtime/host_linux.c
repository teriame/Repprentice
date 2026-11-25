/* Linux & SteamOS (Steam Deck) host platform: X11/OpenGL window, keyboard,
   mouse/touch, Linux evdev/js gamepad, PulseAudio/PipeWire/ALSA audio,
   high-precision timing, and interactive Steam Deck / AppImage GUI launcher
   & in-game overlay menu. */
#ifndef _WIN32
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/joystick.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include "runtime.h"
#include "render.h"
#include "state.h"
#include "cartoon.h"
#include "game_config.h"

int g_headless;
int g_turbo;
int g_fuzz;
int g_dump_frames;
int g_max_frames;
int g_callprof_from;
unsigned long long g_dump_from;
unsigned g_ramdump_at[16];
int g_nramdump;

static Display *dpy;
static Window win;
static Atom wm_delete_window;
static GC xgc;
static XImage *sw_ximg;
static uint32_t *sw_buf;
static int sw_w, sw_h;
static Cursor blank_cursor;
static int cursor_hidden;

static int fullscreen;
static int in_gamescope;
static int use_gpu = 1, start_fullscreen, win_w, win_h;
static int gpu_ok;
static int host_inited;
static char last_title[360];
static void save_settings(void);
static void update_title(void);
extern void frontend_osd(const char *msg);

/* ---------------------------------------------------------------- timing */
static struct timespec ts0;
static int ts_inited;

double host_time(void) {
    struct timespec ts;
    if (!ts_inited) {
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        ts_inited = 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)(ts.tv_sec - ts0.tv_sec) + (double)(ts.tv_nsec - ts0.tv_nsec) * 1e-9;
}

void host_sleep_until(double t) {
    for (;;) {
        double now = host_time();
        if (now >= t) return;
        double d = t - now;
        if (d > 0.0015) {
            struct timespec req;
            double sl = d - 0.0008;
            req.tv_sec = (time_t)sl;
            req.tv_nsec = (long)((sl - (double)req.tv_sec) * 1e9);
            nanosleep(&req, NULL);
        }
    }
}

/* ---------------------------------------------------------------- video settings */
static void settings_path(char *out, size_t n) { snprintf(out, n, "%s/settings.ini", g_data_dir); }

static void load_settings(void) {
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        int v;
        if (sscanf(line, "aspect=%d", &v) == 1) render_settings.aspect = v % RENDER_ASPECTS;
        if (sscanf(line, "filter=%d", &v) == 1) render_settings.filter = v % RENDER_FILTERS;
        if (sscanf(line, "crop_borders=%d", &v) == 1) render_settings.crop_borders = v != 0;
        if (sscanf(line, "fullscreen=%d", &v) == 1) start_fullscreen = v;
    }
    fclose(f);
}

static void save_settings(void) {
    if (g_headless) return;
    char p[600];
    settings_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fprintf(f, "aspect=%d\nfilter=%d\ncrop_borders=%d\nfullscreen=%d\n",
            render_settings.aspect, render_settings.filter, render_settings.crop_borders, fullscreen);
    fclose(f);
}

static void set_cursor_visible(int vis) {
    if (!dpy || !win) return;
    if (vis && cursor_hidden) {
        XUndefineCursor(dpy, win);
        cursor_hidden = 0;
    } else if (!vis && !cursor_hidden && blank_cursor) {
        XDefineCursor(dpy, win, blank_cursor);
        cursor_hidden = 1;
    }
}

static void toggle_fullscreen(void) {
    if (!dpy || !win) return;
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom wm_fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    fullscreen = !fullscreen;
    XEvent xev;
    memset(&xev, 0, sizeof xev);
    xev.type = ClientMessage;
    xev.xclient.window = win;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = fullscreen ? 1 : 0;
    xev.xclient.data.l[1] = (long)wm_fs;
    xev.xclient.data.l[2] = 0;
    xev.xclient.data.l[3] = 1;
    XSendEvent(dpy, DefaultRootWindow(dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &xev);
    XFlush(dpy);
    save_settings();
}

static void update_title(void) {
    if (!dpy || !win) return;
    char t[360];
    const char *ed = g_edition_hd ? " HD" : "";
    const char *keys_hd = g_edition_hd ? "F8 upscaler, " : "";
    if (gpu_ok)
        snprintf(t, sizeof t, GAME_TITLE "%s  [%s, %s]  F1/Select Menu, F2 save, F3 slot, F4 load, F5 invincible, "
                 "F6 run-ahead, F7 interp, %sF9 aspect, F10 filter, F11 fullscreen", ed,
                 render_aspect_name(render_settings.aspect), render_filter_name(render_settings.filter), keys_hd);
    else
        snprintf(t, sizeof t, GAME_TITLE "%s  (X11 Software)  F1/Select Menu", ed);
    if (!strcmp(t, last_title)) return;
    snprintf(last_title, sizeof last_title, "%s", t);
    XStoreName(dpy, win, t);
}

static void settings_changed(void) {
    update_title();
    save_settings();
}

void host_video_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--aspect") && val) render_settings.aspect = atoi(val) % RENDER_ASPECTS;
    else if (!strcmp(opt, "--filter") && val) render_settings.filter = atoi(val) % RENDER_FILTERS;
    else if (!strcmp(opt, "--fullscreen")) start_fullscreen = 1;
    else if (!strcmp(opt, "--windowed")) start_fullscreen = 0;
    else if (!strcmp(opt, "--gdi") || !strcmp(opt, "--sw")) use_gpu = 0;
    else if (!strcmp(opt, "--size") && val) sscanf(val, "%dx%d", &win_w, &win_h);
}

/* ---------------------------------------------------------------- hotkeys */
static int hotq[16], hotn;
static void push_hotkey(int k) { if (hotn < 16) hotq[hotn++] = k; }
int host_take_hotkey(void) {
    if (!hotn) return 0;
    int k = hotq[0];
    memmove(hotq, hotq + 1, (size_t)--hotn * sizeof hotq[0]);
    return k;
}

/* ---------------------------------------------------------------- input state */
enum {
    K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_SHIFT, K_CTRL, K_ALT, K_SPACE, K_RETURN, K_TAB, K_ESC, K_BACKSPACE,
    K_Z, K_X, K_C, K_A, K_S,
    K_COUNT
};
static int keys[K_COUNT];
static int gui_open;
static int gui_nav_up, gui_nav_down, gui_nav_left, gui_nav_right;
static int gui_nav_ok, gui_nav_back, gui_nav_tab;
static int mouse_x, mouse_y, mouse_clicked, mouse_win_w = 1280, mouse_win_h = 800;

static int keysym_to_idx(KeySym ks) {
    switch (ks) {
    case XK_Up: return K_UP;
    case XK_Down: return K_DOWN;
    case XK_Left: return K_LEFT;
    case XK_Right: return K_RIGHT;
    case XK_Shift_L: case XK_Shift_R: return K_SHIFT;
    case XK_Control_L: case XK_Control_R: return K_CTRL;
    case XK_Alt_L: case XK_Alt_R: return K_ALT;
    case XK_space: return K_SPACE;
    case XK_Return: case XK_KP_Enter: return K_RETURN;
    case XK_Tab: return K_TAB;
    case XK_Escape: return K_ESC;
    case XK_BackSpace: return K_BACKSPACE;
    case XK_z: case XK_Z: return K_Z;
    case XK_x: case XK_X: return K_X;
    case XK_c: case XK_C: return K_C;
    case XK_a: case XK_A: return K_A;
    case XK_s: case XK_S: return K_S;
    default: return -1;
    }
}

/* ---------------------------------------------------------------- Linux Gamepad (/dev/input/js*) */
static int js_fd = -1;
static double js_next_probe;
static int js_axes[16];
static int js_btns[32];
static int js_prev_btns[32];
static int js_prev_dpad_y, js_prev_dpad_x;

static void gamepad_open(void) {
    if (js_fd >= 0) return;
    double now = host_time();
    if (now < js_next_probe) return;
    js_next_probe = now + 2.0;
    const char *devs[] = {"/dev/input/js0", "/dev/input/js1", "/dev/input/js2", "/dev/input/js3"};
    for (int i = 0; i < 4; i++) {
        int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            js_fd = fd;
            memset(js_axes, 0, sizeof js_axes);
            memset(js_btns, 0, sizeof js_btns);
            memset(js_prev_btns, 0, sizeof js_prev_btns);
            char name[128] = "Linux Gamepad";
            ioctl(fd, JSIOCGNAME(sizeof name), name);
            LOG("gamepad connected: %s (%s)", name, devs[i]);
            break;
        }
    }
}

static void gamepad_read_events(void) {
    if (js_fd < 0) gamepad_open();
    if (js_fd < 0) return;
    struct js_event ev;
    while (read(js_fd, &ev, sizeof ev) == sizeof ev) {
        int is_init = (ev.type & JS_EVENT_INIT) != 0;
        uint8_t type = (uint8_t)(ev.type & ~JS_EVENT_INIT);
        if (type == JS_EVENT_AXIS && ev.number < 16) {
            js_axes[ev.number] = ev.value;
        } else if (type == JS_EVENT_BUTTON && ev.number < 32) {
            js_btns[ev.number] = ev.value;
            if (is_init) js_prev_btns[ev.number] = ev.value;
        }
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != 0) {
        close(js_fd);
        js_fd = -1;
        return;
    }
    /* On Linux xpad / Steam Deck X-Box 360 pad:
         axis 0 = Left Stick X,  axis 1 = Left Stick Y
         axis 2 = Left Trigger (-32767 released .. +32767 pressed)
         axis 3 = Right Stick X, axis 4 = Right Stick Y
         axis 5 = Right Trigger (-32767 released .. +32767 pressed)
         axis 6 = D-Pad Hat X,   axis 7 = D-Pad Hat Y
       Never treat triggers (axis 2 & axis 5, which rest at -32767) as D-Pad!
       Also support D-pad buttons 11..14 or 13..16 on controllers that map D-pad to buttons. */
    int dy = 0, dx = 0;
    if (js_axes[1] < -16000 || js_axes[7] < -16000 || js_axes[4] < -16000 || js_btns[13]) dy = -1;
    else if (js_axes[1] > 16000 || js_axes[7] > 16000 || js_axes[4] > 16000 || js_btns[14]) dy = 1;
    if (js_axes[0] < -16000 || js_axes[6] < -16000 || js_axes[3] < -16000 || js_btns[11]) dx = -1;
    else if (js_axes[0] > 16000 || js_axes[6] > 16000 || js_axes[3] > 16000 || js_btns[12]) dx = 1;

    if (dy < 0 && js_prev_dpad_y >= 0) gui_nav_up = 1;
    if (dy > 0 && js_prev_dpad_y <= 0) gui_nav_down = 1;
    if (dx < 0 && js_prev_dpad_x >= 0) gui_nav_left = 1;
    if (dx > 0 && js_prev_dpad_x <= 0) gui_nav_right = 1;
    js_prev_dpad_y = dy;
    js_prev_dpad_x = dx;

    /* Button 7 (Burger / Start / Menu on Steam Deck) or Button 8 (Guide / Mode): toggle Quick Menu overlay */
    if ((js_btns[7] && !js_prev_btns[7]) || (js_btns[8] && !js_prev_btns[8])) {
        gui_open = !gui_open;
    }
    if (gui_open) {
        /* A (btn 0): Confirm / Activate */
        if (js_btns[0] && !js_prev_btns[0]) gui_nav_ok = 1;
        /* B (btn 1) or Select/View (btn 6): Back / Close menu */
        if ((js_btns[1] && !js_prev_btns[1]) || (js_btns[6] && !js_prev_btns[6])) gui_nav_back = 1;
        /* X (btn 2) or Y (btn 3): Tab / Cycle */
        if ((js_btns[2] && !js_prev_btns[2]) || (js_btns[3] && !js_prev_btns[3])) gui_nav_tab = 1;
    } else {
        /* Button 6 (Select / View ⧉ on Steam Deck): trigger original CD-i in-game pause screen */
        if (js_btns[6] && !js_prev_btns[6]) g_cdi_pause_req = 4;
        /* In-game controller shortcuts: L1 (btn 4) = Quicksave, R1 (btn 5) = Quickload */
        if (js_btns[4] && !js_prev_btns[4]) push_hotkey(HOTKEY_SAVE);
        if (js_btns[5] && !js_prev_btns[5]) push_hotkey(HOTKEY_LOAD);
    }
    memcpy(js_prev_btns, js_btns, sizeof js_btns);
}

static void gamepad_apply_game_input(void) {
    if (js_fd < 0) return;
    int lx = js_axes[0], ly = js_axes[1];
    const int dead = 8000;
    if (lx > dead || lx < -dead) input.dx += (lx > 0 ? lx - dead : lx + dead) * 10 / (32767 - dead);
    if (ly > dead || ly < -dead) input.dy += (ly > 0 ? ly - dead : ly + dead) * 10 / (32767 - dead);
    /* D-pad hat axes (6/7 on XInput/Steam Deck) + D-pad buttons (11..14) */
    if (js_axes[6] < -16000 || js_btns[11]) input.dx -= 6;
    if (js_axes[6] >  16000 || js_btns[12]) input.dx += 6;
    if (js_axes[7] < -16000 || js_btns[13]) input.dy -= 6;
    if (js_axes[7] >  16000 || js_btns[14]) input.dy += 6;
    /* Standard gamepad buttons: 0=A (Button 1), 1=B / 3=Y (Button 2), 2=X (Button 1+2) */
    if (js_btns[0]) input.b1 = 1;
    if (js_btns[1] || js_btns[3]) input.b2 = 1;
    if (js_btns[2]) { input.b1 = 1; input.b2 = 1; }
}

/* ---------------------------------------------------------------- X11 Window & Presentation */
extern void render_bind_thread(void);
extern void render_unbind_thread(void);

/* Dedicated OpenGL presenter thread so external compositor frame limiters
   (e.g. Steam Deck Gamescope 45 / 40 / 30 / 24 / 15 FPS caps) block only the
   presenter thread inside glXSwapBuffers while the 60.0 Hz CD-i emulation and
   44.1 kHz audio continue running at an uninterrupted, exact 16.67 ms tick. */
static pthread_t pres_thread;
static int pres_thread_started;
static pthread_mutex_t pres_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pres_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pres_ack_cv = PTHREAD_COND_INITIALIZER;
static uint32_t *pres_staging;
static size_t pres_cap;
static int pres_w, pres_h;
static uint64_t pres_seq, pres_done_seq;
static int pres_quit;
static double pres_swap_sum;
static int pres_swap_count;
static int pres_last_hw_vsync;
static double pres_last_swap_done;
static double pres_vblank_dt = 1.0 / FIELD_RATE;

static void *presenter_thread_fn(void *arg) {
    (void)arg;
    render_bind_thread();
    uint32_t *local_buf = NULL;
    size_t local_cap = 0;
    uint64_t rendered_seq = 0;
    double prev_swap_done = -1.0;
    double ema_vblank_dt = 1.0 / FIELD_RATE;

    pthread_mutex_lock(&pres_mtx);
    while (!pres_quit) {
        while (!pres_quit && pres_seq <= rendered_seq) {
            pthread_cond_wait(&pres_cv, &pres_mtx);
        }
        if (pres_quit) break;

        /* When an external <=30 FPS limiter is active (ema_vblank_dt >= 29 ms)
           during 60 Hz gameplay (pres_h == 240), at least 2 simulation fields
           elapse per display refresh.  If glXSwapBuffers woke up slightly
           before the 2nd field finished (pres_seq == rendered_seq + 1), wait
           up to 6 ms for the 2nd field so 30 FPS locks strictly onto every
           2nd field (2, 4, 6, 8...) without +1/+3 phase slip. */
        if (ema_vblank_dt >= 0.029 && pres_h == 240 && pres_seq == rendered_seq + 1) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 6000000L; /* 6 ms */
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            while (!pres_quit && pres_seq < rendered_seq + 2) {
                if (pthread_cond_timedwait(&pres_cv, &pres_mtx, &ts) == ETIMEDOUT)
                    break;
            }
            if (pres_quit) break;
        }

        int w = pres_w, h = pres_h;
        size_t need = (size_t)w * (size_t)h;
        uint64_t seq = pres_seq;
        if (need > local_cap) {
            free(local_buf);
            local_buf = (uint32_t *)malloc(need * 4);
            local_cap = need;
        }
        if (local_buf && pres_staging) {
            memcpy(local_buf, pres_staging, need * 4);
        }
        pthread_mutex_unlock(&pres_mtx);

        double t0 = host_time();
        if (local_buf) render_present(local_buf, w, h);
        double t1 = host_time();
        double swap_dt = t1 - t0;
        if (prev_swap_done > 0.0) {
            double vdt = t1 - prev_swap_done;
            if (vdt > 0.005 && vdt < 0.250)
                ema_vblank_dt = ema_vblank_dt * 0.7 + vdt * 0.3;
        }
        prev_swap_done = t1;

        pthread_mutex_lock(&pres_mtx);
        rendered_seq = seq;
        pres_done_seq = seq;
        pres_swap_sum += swap_dt;
        pres_swap_count++;
        pres_last_hw_vsync = (swap_dt >= 0.004);
        pres_last_swap_done = t1;
        pres_vblank_dt = ema_vblank_dt;
        pthread_cond_broadcast(&pres_ack_cv);
    }
    pthread_mutex_unlock(&pres_mtx);
    render_unbind_thread();
    free(local_buf);
    return NULL;
}

int host_init(void) {
    host_time();
    if (g_headless) return 0;
    if (host_inited) {
        update_title();
        return 0;
    }
    host_inited = 1;
    XInitThreads();
    in_gamescope = (getenv("STEAM_GAMEPADUI") != NULL ||
                    getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL ||
                    getenv("SteamDeck") != NULL ||
                    getenv("STEAM_DECK") != NULL);
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        LOG("XOpenDisplay failed; check DISPLAY environment variable");
        return -1;
    }
    int screen = DefaultScreen(dpy);
    /* Also detect Gamescope via root window atoms (_GAMESCOPE_FPS_LIMIT / STEAM_GAME) */
    if (!in_gamescope) {
        Atom gs_atom = XInternAtom(dpy, "GAMESCOPE_XWAYLAND_SERVER_ID", True);
        Atom st_atom = XInternAtom(dpy, "STEAM_GAME", True);
        if (gs_atom != None || st_atom != None) in_gamescope = 1;
    }
    LOG("windowing: X11 display %dx%d (gamescope=%d)",
        DisplayWidth(dpy, screen), DisplayHeight(dpy, screen), in_gamescope);
    int save_aspect = render_settings.aspect, save_filter = render_settings.filter;
    int cli_fs = start_fullscreen;
    load_settings();
    if (save_aspect) render_settings.aspect = save_aspect;
    if (save_filter) render_settings.filter = save_filter;
    if (cli_fs) start_fullscreen = 1;

    int sh = DisplayHeight(dpy, screen);
    int cw = win_w > 0 ? win_w : 1280;
    int ch = win_h > 0 ? win_h : 800;
    if (win_w <= 0 && sh > 0) {
        /* On Steam Deck (1280x800), fill 1280x800; on larger screens, 3/4 height 16:9 */
        if (DisplayWidth(dpy, screen) == 1280 && sh == 800) {
            cw = 1280; ch = 800;
        } else {
            ch = sh * 3 / 4;
            cw = ch * 16 / 9;
        }
    }
    mouse_win_w = cw;
    mouse_win_h = ch;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, (unsigned)cw, (unsigned)ch, 1,
                              BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    wm_delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete_window, 1);

    /* Set WM_CLASS and STEAM_GAME atom for SteamOS / Gamescope integration */
    XClassHint *chint = XAllocClassHint();
    if (chint) {
        chint->res_name = (char *)EXE_NAME;
        chint->res_class = (char *)GAME_TITLE;
        XSetClassHint(dpy, win, chint);
        XFree(chint);
    }
    const char *s_appid = getenv("SteamAppId");
    if (!s_appid || !s_appid[0]) s_appid = getenv("STEAM_COMPAT_APP_ID");
    if (s_appid && s_appid[0]) {
        unsigned long appid_val = strtoul(s_appid, NULL, 10);
        if (appid_val > 0) {
            Atom steam_game = XInternAtom(dpy, "STEAM_GAME", False);
            XChangeProperty(dpy, win, steam_game, XA_CARDINAL, 32,
                            PropModeReplace, (unsigned char *)&appid_val, 1);
        }
    }

    XSelectInput(dpy, win, KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                           PointerMotionMask | StructureNotifyMask | FocusChangeMask | ExposureMask);
    /* If launching fullscreen or under Gamescope, set _NET_WM_STATE_FULLSCREEN and
       _NET_WM_BYPASS_COMPOSITOR=1 before XMapWindow so Gamescope immediately gives
       the window direct scanout and focus without mid-frame reconfigure stalls. */
    if (!start_fullscreen && (in_gamescope || (DisplayWidth(dpy, screen) == 1280 && sh == 800))) {
        start_fullscreen = 1;
    }
    if (start_fullscreen) {
        fullscreen = 1;
        Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
        Atom wm_fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
        XChangeProperty(dpy, win, wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)&wm_fs, 1);
        unsigned long bypass = 1;
        Atom wm_bypass = XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
        XChangeProperty(dpy, win, wm_bypass, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&bypass, 1);
    }
    XMapWindow(dpy, win);
    xgc = XCreateGC(dpy, win, 0, NULL);

    /* Blank cursor for in-game mouse hiding */
    Pixmap bm = XCreateBitmapFromData(dpy, win, "\0\0\0\0\0\0\0\0", 1, 1);
    XColor black;
    memset(&black, 0, sizeof black);
    blank_cursor = XCreatePixmapCursor(dpy, bm, bm, &black, &black, 0, 0);
    XFreePixmap(dpy, bm);

    if (use_gpu) gpu_ok = (render_init_x11(dpy, win) == 0);
    if (gpu_ok && !pres_thread_started) {
        render_unbind_thread();
        if (pthread_create(&pres_thread, NULL, presenter_thread_fn, NULL) == 0) {
            pres_thread_started = 1;
        } else {
            render_bind_thread();
        }
    }
    if (start_fullscreen) {
        /* Also send the EWMH ClientMessage for window managers that require it after map */
        fullscreen = 0;
        toggle_fullscreen();
    }
    update_title();
    XFlush(dpy);
    return 0;
}

static void sw_present(const uint32_t *fb, int w, int h) {
    if (!dpy || !win) return;
    int cw = mouse_win_w > 0 ? mouse_win_w : 1;
    int ch = mouse_win_h > 0 ? mouse_win_h : 1;
    if (!sw_ximg || cw != sw_w || ch != sw_h) {
        if (sw_ximg) { sw_ximg->data = NULL; XDestroyImage(sw_ximg); sw_ximg = NULL; }
        free(sw_buf);
        sw_w = cw; sw_h = ch;
        sw_buf = (uint32_t *)calloc((size_t)cw * ch, 4);
        sw_ximg = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                               (unsigned)DefaultDepth(dpy, DefaultScreen(dpy)),
                               ZPixmap, 0, (char *)sw_buf, (unsigned)cw, (unsigned)ch, 32, cw * 4);
    }
    memset(sw_buf, 0, (size_t)cw * ch * 4);
    int dw = cw, dh = cw * 3 / 4;
    if (dh > ch) { dh = ch; dw = ch * 4 / 3; }
    int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
    for (int y = 0; y < dh; y++) {
        int sy = y * h / dh;
        const uint32_t *srow = fb + sy * w;
        uint32_t *drow = sw_buf + (dy + y) * cw + dx;
        for (int x = 0; x < dw; x++) drow[x] = srow[x * w / dw];
    }
    XPutImage(dpy, win, xgc, sw_ximg, 0, 0, 0, 0, (unsigned)cw, (unsigned)ch);
    XFlush(dpy);
}

static void present_raw_sync(const uint32_t *fb, int w, int h, int wait_ack) {
    if (g_headless || !dpy || !win) return;
    if (gpu_ok && pres_thread_started) {
        size_t need = (size_t)w * (size_t)h;
        pthread_mutex_lock(&pres_mtx);
        if (need > pres_cap) {
            free(pres_staging);
            pres_staging = (uint32_t *)malloc(need * 4);
            pres_cap = need;
        }
        if (pres_staging) {
            memcpy(pres_staging, fb, need * 4);
            pres_w = w;
            pres_h = h;
            uint64_t seq = ++pres_seq;
            pthread_cond_signal(&pres_cv);
            if (wait_ack) {
                while (!pres_quit && pres_done_seq < seq) {
                    pthread_cond_wait(&pres_ack_cv, &pres_mtx);
                }
            }
        }
        pthread_mutex_unlock(&pres_mtx);
        return;
    }
    if (gpu_ok && render_present(fb, w, h) != 0) {
        LOG("GPU present failed; falling back to X11 software output");
        gpu_ok = 0;
        update_title();
    }
    if (!gpu_ok) sw_present(fb, w, h);
}

static void present_raw(const uint32_t *fb, int w, int h) {
    present_raw_sync(fb, w, h, 0);
}

static void pump_x11_events(void) {
    if (g_headless || !dpy) return;
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == wm_delete_window) {
                input.quit = 1;
                if (gui_open) exit(0);
            }
            break;
        case ConfigureNotify:
            mouse_win_w = ev.xconfigure.width > 0 ? ev.xconfigure.width : 1;
            mouse_win_h = ev.xconfigure.height > 0 ? ev.xconfigure.height : 1;
            render_set_window_size(mouse_win_w, mouse_win_h);
            break;
        case MotionNotify:
            mouse_x = ev.xmotion.x;
            mouse_y = ev.xmotion.y;
            break;
        case ButtonPress:
            mouse_x = ev.xbutton.x;
            mouse_y = ev.xbutton.y;
            if (ev.xbutton.button == Button1) mouse_clicked = 1;
            else if (ev.xbutton.button == Button3 && gui_open) gui_nav_back = 1;
            else if (ev.xbutton.button == Button4 && gui_open) gui_nav_up = 1;
            else if (ev.xbutton.button == Button5 && gui_open) gui_nav_down = 1;
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int idx = keysym_to_idx(ks);
            int was_down = (idx >= 0) ? keys[idx] : 0;
            if (idx >= 0) keys[idx] = 1;

            if (ks == XK_F1 || ks == XK_Menu) {
                gui_open = !gui_open;
                break;
            }
            if (gui_open) {
                if (ks == XK_Up || ks == XK_w || ks == XK_W) gui_nav_up = 1;
                else if (ks == XK_Down || ks == XK_s || ks == XK_S) gui_nav_down = 1;
                else if (ks == XK_Left || ks == XK_a || ks == XK_A) gui_nav_left = 1;
                else if (ks == XK_Right || ks == XK_d || ks == XK_D) gui_nav_right = 1;
                else if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_space) gui_nav_ok = 1;
                else if (ks == XK_Escape || ks == XK_BackSpace) gui_nav_back = 1;
                else if (ks == XK_Tab) gui_nav_tab = 1;
            } else {
                if (ks == XK_Escape) input.quit = 1;
                if (!was_down) {
                    if (ks == XK_p || ks == XK_P || ks == XK_Pause) g_cdi_pause_req = 4;
                    if (ks == XK_F2) push_hotkey(HOTKEY_SAVE);
                    if (ks == XK_F3) push_hotkey(HOTKEY_SLOT);
                    if (ks == XK_F4) push_hotkey(HOTKEY_LOAD);
                    if (ks == XK_F5) push_hotkey(HOTKEY_INVINCIBLE);
                    if (ks == XK_F6) push_hotkey(HOTKEY_RUNAHEAD);
                    if (ks == XK_F7) push_hotkey(HOTKEY_INTERP);
                    if (ks == XK_F8) push_hotkey(HOTKEY_UPSCALER);
                }
            }
            if (ks == XK_F9) { render_settings.aspect = (render_settings.aspect + 1) % RENDER_ASPECTS; settings_changed(); }
            if (ks == XK_F10) { render_settings.filter = (render_settings.filter + 1) % RENDER_FILTERS; settings_changed(); }
            if (ks == XK_F12) {
                render_settings.crop_borders = !render_settings.crop_borders;
                settings_changed();
                frontend_osd(render_settings.crop_borders ? "Crop overscan borders: on" : "Crop overscan borders: off");
            }
            if (ks == XK_F11 || (ks == XK_Return && keys[K_ALT])) {
                toggle_fullscreen();
                keys[K_RETURN] = 0;
            }
            break;
        }
        case KeyRelease: {
            /* Ignore X11 key auto-repeat release+press pairs */
            if (XEventsQueued(dpy, QueuedAfterReading)) {
                XEvent nev;
                XPeekEvent(dpy, &nev);
                if (nev.type == KeyPress && nev.xkey.time == ev.xkey.time &&
                    nev.xkey.keycode == ev.xkey.keycode) {
                    break;
                }
            }
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            int idx = keysym_to_idx(ks);
            if (idx >= 0) keys[idx] = 0;
            break;
        }
        case FocusOut:
            memset(keys, 0, sizeof keys);
            break;
        }
    }
}

/* ---------------------------------------------------------------- Built-in 5x7 Font & GUI Renderer */
#define GUI_W 768
#define GUI_H 480
static uint32_t *gui_fb;

/* Full ASCII 32..90 5x7 bitmap font (matching frontend.c + punctuation) */
static const uint8_t gui_font5x7[][7] = {
    {0,0,0,0,0,0,0},{4,4,4,4,4,0,4},{10,10,0,0,0,0,0},{10,31,10,10,31,10,0},{4,15,20,14,5,30,4},
    {24,25,2,4,8,19,3},{12,18,20,8,21,18,13},{4,4,0,0,0,0,0},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},
    {0,4,21,14,21,4,0},{0,4,4,31,4,4,0},{0,0,0,0,12,4,8},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},
    {0,1,2,4,8,16,0},{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},
    {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},{0,12,12,0,12,12,0},{0,12,12,0,12,4,8},{2,4,8,16,8,4,2},{0,0,31,0,31,0,0},
    {8,4,2,1,2,4,8},{14,17,1,2,4,0,4},{14,17,1,13,21,21,14},{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14},{28,18,17,17,17,18,28},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},
    {16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},
    {31,4,4,4,4,4,4},{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
    {17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
    {14,8,8,8,8,8,14},{16,8,4,2,1,0,0},{14,2,2,2,2,2,14},{4,10,17,0,0,0,0},{0,0,0,0,0,0,31}
};

static void gui_fill_rect(int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GUI_W) w = GUI_W - x;
    if (y + h > GUI_H) h = GUI_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t *row = gui_fb + yy * GUI_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = col;
    }
}

static void gui_border_rect(int x, int y, int w, int h, int t, uint32_t col) {
    gui_fill_rect(x, y, w, t, col);
    gui_fill_rect(x, y + h - t, w, t, col);
    gui_fill_rect(x, y, t, h, col);
    gui_fill_rect(x + w - t, y, t, h, col);
}

static void gui_draw_text(int x0, int y0, int scale, uint32_t col, const char *str) {
    int x = x0;
    for (const char *p = str; *p; p++, x += 6 * scale) {
        int c = (unsigned char)*p;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == '~') c = '-';
        if (c < ' ' || c > '_') c = ' ';
        const uint8_t *g = gui_font5x7[c - ' '];
        for (int yy = 0; yy < 7; yy++) {
            for (int xx = 0; xx < 5; xx++) {
                if (!(g[yy] & (16 >> xx))) continue;
                gui_fill_rect(x + xx * scale, y0 + yy * scale, scale, scale, col);
            }
        }
    }
}

/* Map window mouse coordinate (mouse_x, mouse_y) to GUI_W x GUI_H canvas */
static void gui_mouse_coords(int *gx, int *gy) {
    int cw = mouse_win_w > 0 ? mouse_win_w : 1;
    int ch = mouse_win_h > 0 ? mouse_win_h : 1;
    double win_aspect = (double)cw / ch, aspect = 4.0 / 3.0;
    double pw, ph;
    int mode = render_settings.aspect % RENDER_ASPECTS;
    if (mode == 2) { pw = cw; ph = ch; }
    else if (mode == 3) { pw = cw; ph = cw / aspect; if (ph < ch) { ph = ch; pw = ch * aspect; } }
    else if (win_aspect > aspect) { ph = ch; pw = ch * aspect; }
    else { pw = cw; ph = cw / aspect; }
    double ox = (cw - pw) * 0.5;
    double oy = (ch - ph) * 0.5;
    *gx = (int)((mouse_x - ox) * GUI_W / (pw > 1.0 ? pw : 1.0));
    *gy = (int)((mouse_y - oy) * GUI_H / (ph > 1.0 ? ph : 1.0));
}

/* External frontend & ini helpers */
extern int  frontend_get_runahead(void);
extern void frontend_set_runahead(int v);
extern int  frontend_get_interp(void);
extern void frontend_set_interp(int v);
extern int  frontend_get_upscaler(void);
extern void frontend_set_upscaler(int v);
extern int  frontend_get_slot(void);
extern void frontend_set_slot(int v);
extern void frontend_osd(const char *msg);

/* Native desktop file dialog (Zenity / KDialog) when available */
static int try_system_file_dialog(const char *start_dir, char *out, size_t n) {
    char cmd[1200];
    if (access("/usr/bin/zenity", X_OK) == 0) {
        snprintf(cmd, sizeof cmd,
                 "/usr/bin/zenity --file-selection --title='Select " GAME_TITLE " disc image (.cue, .bin, .chd)' "
                 "--filename='%s/' --file-filter='CD-i Disc Images (*.cue *.bin *.chd) | *.cue *.bin *.chd *.CUE *.BIN *.CHD' "
                 "--file-filter='All files | *' 2>/dev/null", start_dir);
    } else if (access("/usr/bin/kdialog", X_OK) == 0) {
        snprintf(cmd, sizeof cmd,
                 "/usr/bin/kdialog --getopenfilename '%s' '*.cue *.bin *.chd *.CUE *.BIN *.CHD|CD-i Disc Images' "
                 "--title 'Select " GAME_TITLE " disc image' 2>/dev/null", start_dir);
    } else {
        return -1;
    }
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[1024] = "";
    if (!fgets(line, sizeof line, p)) { pclose(p); return -1; }
    int rc = pclose(p);
    line[strcspn(line, "\r\n")] = 0;
    if (rc != 0 || !line[0]) return -1;
    snprintf(out, n, "%s", line);
    return 0;
}

extern int  runtime_extract_embedded_tools(char *out_dir, size_t n);
extern void runtime_cleanup_embedded_tools(const char *tool_dir);

/* Convert a .chd file using embedded tools/chd.py into saves/ (inside CWD or /tmp) */
static int convert_chd_image(const char *exe_dir, const char *chd_path, char *cue_out, size_t n) {
    (void)exe_dir;
    char tool_dir[600] = "";
    if (runtime_extract_embedded_tools(tool_dir, sizeof tool_dir) != 0) return -1;
    char script[700];
    snprintf(script, sizeof script, "%s/tools/chd.py", tool_dir);
    if (gui_fb) {
        gui_fill_rect(80, 180, GUI_W - 160, 120, 0xFF141E30u);
        gui_border_rect(80, 180, GUI_W - 160, 120, 2, 0xFF50A0FFu);
        gui_draw_text(110, 210, 2, 0xFFFFE070u, "CONVERTING CHD TO BIN/CUE...");
        gui_draw_text(110, 245, 1, 0xFFD0D8E8u, "THIS TAKES 1-2 MINUTES ON FIRST RUN. PLEASE WAIT.");
        present_raw(gui_fb, GUI_W, GUI_H);
    }
    /* Always output converted .bin/.cue into g_data_dir (saves/ inside CWD or /tmp)
       so we never write to external ROM directories outside CWD or /tmp. */
    const char *sl = strrchr(chd_path, '/');
    const char *base_name = sl ? sl + 1 : chd_path;
    char stem[256];
    snprintf(stem, sizeof stem, "%s", base_name);
    size_t L = strlen(stem);
    if (L > 4) stem[L - 4] = 0;
    char out_prefix[1024];
    snprintf(out_prefix, sizeof out_prefix, "%s/%s", g_data_dir, stem);

    char cmd[2600];
    snprintf(cmd, sizeof cmd, "python3 '%s' '%s' '%s'", script, chd_path, out_prefix);
    int r = system(cmd);
    runtime_cleanup_embedded_tools(tool_dir);
    if (r != 0) return -1;
    snprintf(cue_out, n, "%s.cue", out_prefix);
    return access(cue_out, R_OK) == 0 ? 0 : -1;
}

/* ---------------------------------------------------------------- Built-in In-Window File Browser */
typedef struct {
    char name[256];
    char full[1024];
    int is_dir;
} FileEntry;

static int cmp_file_entry(const void *a, const void *b) {
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    if (fa->is_dir != fb->is_dir) return fb->is_dir - fa->is_dir;
    return strcasecmp(fa->name, fb->name);
}

static int load_dir_entries(const char *dir, const char *exe_dir, FileEntry *list, int max_entries) {
    int n = 0;
    /* Top quick-jump shortcuts for Steam Deck & Desktop convenience */
    snprintf(list[n].name, sizeof list[n].name, "[.. PARENT DIRECTORY]");
    snprintf(list[n].full, sizeof list[n].full, "%s/..", dir);
    list[n].is_dir = 1; n++;

    char rom_dir[600];
    snprintf(rom_dir, sizeof rom_dir, "%s/rom", exe_dir);
    if (access(rom_dir, R_OK) == 0 && n < max_entries) {
        snprintf(list[n].name, sizeof list[n].name, "[GAME ROM FOLDER]");
        snprintf(list[n].full, sizeof list[n].full, "%s", rom_dir);
        list[n].is_dir = 1; n++;
    }
    const char *home = getenv("HOME");
    if (home && home[0] && n < max_entries) {
        snprintf(list[n].name, sizeof list[n].name, "[HOME FOLDER (%s)]", home);
        snprintf(list[n].full, sizeof list[n].full, "%s", home);
        list[n].is_dir = 1; n++;
    }
    if (access("/run/media", R_OK) == 0 && n < max_entries) {
        snprintf(list[n].name, sizeof list[n].name, "[STEAM DECK SD CARD (/run/media)]");
        snprintf(list[n].full, sizeof list[n].full, "/run/media");
        list[n].is_dir = 1; n++;
    }
    if ((access("/usr/bin/zenity", X_OK) == 0 || access("/usr/bin/kdialog", X_OK) == 0) && n < max_entries) {
        snprintf(list[n].name, sizeof list[n].name, "[OPEN SYSTEM FILE DIALOG...]");
        snprintf(list[n].full, sizeof list[n].full, "::SYS_DIALOG::");
        list[n].is_dir = 2; n++;
    }

    int fixed_top = n;
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL && n < max_entries) {
            if (de->d_name[0] == '.') continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            struct stat st;
            if (stat(p, &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                snprintf(list[n].name, sizeof list[n].name, "[DIR] %s", de->d_name);
                snprintf(list[n].full, sizeof list[n].full, "%s", p);
                list[n].is_dir = 1;
                n++;
            } else if (S_ISREG(st.st_mode)) {
                size_t L = strlen(de->d_name);
                if (L > 4 && (!strcasecmp(de->d_name + L - 4, ".cue") ||
                              !strcasecmp(de->d_name + L - 4, ".bin") ||
                              !strcasecmp(de->d_name + L - 4, ".chd"))) {
                    snprintf(list[n].name, sizeof list[n].name, "      %s", de->d_name);
                    snprintf(list[n].full, sizeof list[n].full, "%s", p);
                    list[n].is_dir = 0;
                    n++;
                }
            }
        }
        closedir(d);
    }
    if (n > fixed_top) {
        qsort(list + fixed_top, (size_t)(n - fixed_top), sizeof(FileEntry), cmp_file_entry);
    }
    return n;
}

static int run_file_browser(const char *exe_dir, char *out, size_t out_n) {
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);
    FileEntry *entries = (FileEntry *)calloc(512, sizeof(FileEntry));
    char cur_dir[PATH_MAX];
    if (!realpath(exe_dir, cur_dir)) snprintf(cur_dir, sizeof cur_dir, "%s", exe_dir);
    int count = load_dir_entries(cur_dir, exe_dir, entries, 512);
    int sel = 0, scroll = 0;
    const int vis_rows = 12;
    char status_msg[128] = "SELECT A .CUE, .BIN OR .CHD DISC IMAGE";

    set_cursor_visible(1);
    gui_open = 1;
    gui_nav_up = gui_nav_down = gui_nav_left = gui_nav_right = gui_nav_ok = gui_nav_back = 0;
    mouse_clicked = 0;

    int result = -1;
    while (!input.quit) {
        pump_x11_events();
        gamepad_read_events();

        if (gui_nav_up) { sel = (sel - 1 + count) % (count > 0 ? count : 1); gui_nav_up = 0; }
        if (gui_nav_down) { sel = (sel + 1) % (count > 0 ? count : 1); gui_nav_down = 0; }
        if (gui_nav_left) { sel = sel >= vis_rows ? sel - vis_rows : 0; gui_nav_left = 0; }
        if (gui_nav_right) { sel = sel + vis_rows < count ? sel + vis_rows : (count > 0 ? count - 1 : 0); gui_nav_right = 0; }

        int gx, gy;
        gui_mouse_coords(&gx, &gy);
        if (mouse_clicked) {
            mouse_clicked = 0;
            /* Check back button at top right */
            if (gx >= GUI_W - 130 && gx <= GUI_W - 24 && gy >= 16 && gy <= 46) {
                gui_nav_back = 1;
            }
            /* Check list rows */
            for (int r = 0; r < vis_rows && scroll + r < count; r++) {
                int ry = 92 + r * 26;
                if (gx >= 32 && gx <= GUI_W - 32 && gy >= ry && gy < ry + 24) {
                    sel = scroll + r;
                    gui_nav_ok = 1;
                    break;
                }
            }
        }

        if (gui_nav_back) {
            gui_nav_back = 0;
            result = -1;
            break;
        }
        if (gui_nav_ok && count > 0) {
            gui_nav_ok = 0;
            FileEntry *e = &entries[sel];
            if (e->is_dir == 2) {
                char picked[1024] = "";
                if (try_system_file_dialog(cur_dir, picked, sizeof picked) == 0 && picked[0]) {
                    size_t L = strlen(picked);
                    if (L > 4 && !strcasecmp(picked + L - 4, ".chd")) {
                        if (convert_chd_image(exe_dir, picked, out, out_n) == 0) { result = 0; break; }
                        snprintf(status_msg, sizeof status_msg, "CHD CONVERSION FAILED");
                    } else {
                        snprintf(out, out_n, "%s", picked);
                        result = 0;
                        break;
                    }
                }
            } else if (e->is_dir == 1) {
                char resolved[PATH_MAX];
                if (realpath(e->full, resolved)) snprintf(cur_dir, sizeof cur_dir, "%s", resolved);
                else snprintf(cur_dir, sizeof cur_dir, "%s", e->full);
                count = load_dir_entries(cur_dir, exe_dir, entries, 512);
                sel = 0; scroll = 0;
            } else {
                size_t L = strlen(e->full);
                if (L > 4 && !strcasecmp(e->full + L - 4, ".chd")) {
                    if (convert_chd_image(exe_dir, e->full, out, out_n) == 0) { result = 0; break; }
                    snprintf(status_msg, sizeof status_msg, "CHD CONVERSION FAILED");
                } else {
                    snprintf(out, out_n, "%s", e->full);
                    result = 0;
                    break;
                }
            }
        }

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + vis_rows) scroll = sel - vis_rows + 1;

        /* Draw File Browser UI */
        gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
        gui_fill_rect(0, 0, GUI_W, 56, 0xFF182438u);
        gui_draw_text(24, 14, 2, 0xFFFFE070u, "SELECT DISC IMAGE (.CUE / .BIN / .CHD)");
        gui_draw_text(24, 38, 1, 0xFF98B0D0u, cur_dir);

        /* Cancel button */
        gui_fill_rect(GUI_W - 126, 16, 102, 26, 0xFF2C3B55u);
        gui_border_rect(GUI_W - 126, 16, 102, 26, 1, 0xFF6080B0u);
        gui_draw_text(GUI_W - 114, 25, 1, 0xFFFFFFFFu, "BACK [B/ESC]");

        gui_draw_text(32, 68, 1, 0xFF80C0FFu, status_msg);

        for (int r = 0; r < vis_rows; r++) {
            int idx = scroll + r;
            int ry = 92 + r * 26;
            if (idx >= count) break;
            int is_sel = (idx == sel);
            uint32_t bg = is_sel ? 0xFF284A78u : (r & 1 ? 0xFF151F30u : 0xFF121A28u);
            gui_fill_rect(28, ry, GUI_W - 56, 24, bg);
            if (is_sel) gui_border_rect(28, ry, GUI_W - 56, 24, 1, 0xFFFFE070u);
            uint32_t fg = entries[idx].is_dir ? (is_sel ? 0xFFFFE070u : 0xFF70C0FFu) : 0xFFFFFFFFu;
            char label[110];
            snprintf(label, sizeof label, "%.105s", entries[idx].name);
            gui_draw_text(40, ry + 8, 1, fg, label);
        }

        /* Footer legend */
        gui_fill_rect(0, GUI_H - 36, GUI_W, 36, 0xFF182438u);
        gui_draw_text(24, GUI_H - 22, 1, 0xFFB8C8E0u,
                      "D-PAD/ARROWS: NAVIGATE    [A/ENTER/CLICK]: OPEN/SELECT    [B/ESC]: BACK");

        present_raw(gui_fb, GUI_W, GUI_H);
        host_sleep_until(host_time() + 1.0 / 60.0);
    }
    free(entries);
    return result;
}

int host_pick_image(const char *exe_dir, char *out, size_t n) {
    if (g_headless) return -1;
    if (!host_inited) host_init();
    if (dpy && win) return run_file_browser(exe_dir, out, n);
    return try_system_file_dialog(exe_dir, out, n);
}

/* ---------------------------------------------------------------- Interactive Launcher & In-Game Overlay GUI */
static void ini_get_str(const char *exe_dir, const char *key, char *out, size_t n) {
    char p[600], line[1024];
    snprintf(p, sizeof p, "%s/%s.ini", exe_dir, EXE_NAME);
    FILE *f = fopen(p, "r");
    if (!f) return;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (!strncasecmp(line, key, kl) && line[kl] == '=') {
            char *v = line + kl + 1;
            v[strcspn(v, "\r\n")] = 0;
            snprintf(out, n, "%s", v);
        }
    }
    fclose(f);
}

static void ini_set_str(const char *exe_dir, const char *key, const char *value) {
    char p[600], line[1024], buf[16384] = "";
    snprintf(p, sizeof p, "%s/%s.ini", exe_dir, EXE_NAME);
    size_t kl = strlen(key), used = 0;
    FILE *f = fopen(p, "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            if (!strncasecmp(line, key, kl) && line[kl] == '=') continue;
            size_t L = strlen(line);
            if (used + L + 1 < sizeof buf) { memcpy(buf + used, line, L); used += L; buf[used] = 0; }
        }
        fclose(f);
    }
    f = fopen(p, "w");
    if (!f) return;
    fputs(buf, f);
    if (used && buf[used - 1] != '\n') fputc('\n', f);
    fprintf(f, "%s=%s\n", key, value);
    fclose(f);
}

static const char *upscaler_label(int u) {
    return u == CARTOON_HDPLUS ? "CARTOON HD+ (1536X960 INKED)" :
           u == CARTOON_HD     ? "CARTOON HD (1536X960)" :
                                 "ORIGINAL (384X240)";
}

/* Locate tools/steam_register.py (inside AppImage or repo) and run it */
int host_register_in_steam(const char *exe_dir, int check_only, int auto_only, char *status_out, size_t status_n) {
    char script[1024] = "";
    const char *appdir = getenv("APPDIR");
    if (appdir && appdir[0]) {
        snprintf(script, sizeof script, "%s/usr/share/apprentice/tools/steam_register.py", appdir);
        if (access(script, R_OK) != 0) script[0] = 0;
    }
    if (!script[0]) {
        snprintf(script, sizeof script, "%s/tools/steam_register.py", exe_dir);
        if (access(script, R_OK) != 0) snprintf(script, sizeof script, "tools/steam_register.py");
    }
    if (access(script, R_OK) != 0) {
        if (status_out && status_n) snprintf(status_out, status_n, "STEAM_REGISTER.PY NOT FOUND");
        return -1;
    }
    char target_exe[PATH_MAX] = "";
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0]) {
        if (!realpath(appimg, target_exe)) snprintf(target_exe, sizeof target_exe, "%s", appimg);
    } else {
        ssize_t r = readlink("/proc/self/exe", target_exe, sizeof target_exe - 1);
        if (r > 0) target_exe[r] = 0;
        else snprintf(target_exe, sizeof target_exe, "%s/%s", exe_dir, EXE_NAME);
    }

    char cmd[2600];
    if (check_only) {
        snprintf(cmd, sizeof cmd, "python3 '%s' --exe '%s' --check >/dev/null 2>&1", script, target_exe);
        return system(cmd) == 0 ? 1 : 0;
    }
    snprintf(cmd, sizeof cmd, "python3 '%s' --exe '%s'%s 2>&1",
             script, target_exe, auto_only ? " --auto" : "");
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char line[256] = "";
    if (fgets(line, sizeof line, p)) {
        line[strcspn(line, "\r\n")] = 0;
    }
    int rc = pclose(p);
    if (status_out && status_n && line[0]) snprintf(status_out, status_n, "%s", line);
    if (!auto_only) LOG("steam registration: %s (rc=%d)", line[0] ? line : "done", rc);
    return rc == 0 ? 0 : -1;
}

/* Pre-game Launcher GUI (returns 0 when user clicks Play with a valid ROM) */
int host_run_launcher(const char *exe_dir, char *img_out, size_t n, int has_img) {
    if (g_headless) return -1;
    if (!host_inited) host_init();
    if (!dpy || !win) return -1;
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);

    char show_lv[16] = "0";
    ini_get_str(exe_dir, "show_launcher", show_lv, sizeof show_lv);
    int show_on_start = (show_lv[0] == '1');
    int in_steam = (host_register_in_steam(exe_dir, 1, 0, NULL, 0) == 1);

    enum {
        LM_PLAY,
        LM_DISC,
        LM_STEAM,
        LM_UPSCALER,
        LM_ASPECT,
        LM_CROP,
        LM_FILTER,
        LM_FULLSCREEN,
        LM_RUNAHEAD,
        LM_INTERP,
        LM_INVINCIBLE,
        LM_SHOW_LAUNCHER,
        LM_QUIT,
        LM_COUNT
    };
    int sel = has_img ? LM_PLAY : LM_DISC;
    char status_msg[128] = "";
    snprintf(status_msg, sizeof status_msg, "%s",
             has_img ? "READY TO PLAY! PRESS [A / START / ENTER] TO LAUNCH"
                     : "DISC IMAGE NOT FOUND - SELECT 'DISC IMAGE' BELOW TO BROWSE");

    set_cursor_visible(1);
    gui_open = 1;
    gui_nav_up = gui_nav_down = gui_nav_left = gui_nav_right = gui_nav_ok = gui_nav_back = 0;
    mouse_clicked = 0;

    while (!input.quit) {
        pump_x11_events();
        gamepad_read_events();

        if (gui_nav_up) { sel = (sel - 1 + LM_COUNT) % LM_COUNT; gui_nav_up = 0; }
        if (gui_nav_down) { sel = (sel + 1) % LM_COUNT; gui_nav_down = 0; }

        int gx, gy;
        gui_mouse_coords(&gx, &gy);
        if (mouse_clicked) {
            mouse_clicked = 0;
            for (int i = 0; i < LM_COUNT; i++) {
                int ry = 80 + i * 27;
                if (gx >= 36 && gx <= GUI_W - 36 && gy >= ry && gy < ry + 24) {
                    sel = i;
                    gui_nav_ok = 1;
                    break;
                }
            }
        }

        int dir = 0;
        if (gui_nav_left) { dir = -1; gui_nav_left = 0; }
        if (gui_nav_right) { dir = 1; gui_nav_right = 0; }
        if (gui_nav_ok) { dir = 1; gui_nav_ok = 0; }

        if (gui_nav_back) {
            gui_nav_back = 0;
            if (has_img) break;
        }

        if (dir != 0) {
            switch (sel) {
            case LM_PLAY:
                if (has_img) {
                    gui_open = 0;
                    set_cursor_visible(0);
                    return 0;
                }
                /* If no disc image yet, open browser immediately */
                if (run_file_browser(exe_dir, img_out, n) == 0 && img_out[0]) {
                    has_img = 1;
                    ini_set_str(exe_dir, "rom", img_out);
                    snprintf(status_msg, sizeof status_msg, "DISC IMAGE SELECTED! PRESS [A / ENTER] TO PLAY");
                }
                break;
            case LM_DISC:
                if (run_file_browser(exe_dir, img_out, n) == 0 && img_out[0]) {
                    has_img = 1;
                    ini_set_str(exe_dir, "rom", img_out);
                    sel = LM_PLAY;
                    snprintf(status_msg, sizeof status_msg, "DISC IMAGE SELECTED! PRESS [A / ENTER] TO PLAY");
                }
                break;
            case LM_STEAM:
                if (host_register_in_steam(exe_dir, 0, 0, status_msg, sizeof status_msg) == 0) {
                    in_steam = 1;
                }
                break;
            case LM_UPSCALER: {
                int u = (frontend_get_upscaler() + (dir > 0 ? 1 : CARTOON_MODES - 1)) % CARTOON_MODES;
                frontend_set_upscaler(u);
                update_title();
                break;
            }
            case LM_ASPECT:
                render_settings.aspect = (render_settings.aspect + (dir > 0 ? 1 : RENDER_ASPECTS - 1)) % RENDER_ASPECTS;
                settings_changed();
                break;
            case LM_CROP:
                render_settings.crop_borders = !render_settings.crop_borders;
                settings_changed();
                break;
            case LM_FILTER:
                render_settings.filter = (render_settings.filter + (dir > 0 ? 1 : RENDER_FILTERS - 1)) % RENDER_FILTERS;
                settings_changed();
                break;
            case LM_FULLSCREEN:
                toggle_fullscreen();
                break;
            case LM_RUNAHEAD:
                frontend_set_runahead((frontend_get_runahead() + (dir > 0 ? 1 : 3)) % 4);
                break;
            case LM_INTERP:
                frontend_set_interp(!frontend_get_interp());
                break;
            case LM_INVINCIBLE:
                g_invincible = !g_invincible;
                break;
            case LM_SHOW_LAUNCHER:
                show_on_start = !show_on_start;
                ini_set_str(exe_dir, "show_launcher", show_on_start ? "1" : "0");
                break;
            case LM_QUIT:
                exit(0);
            }
        }

        /* Draw Launcher UI */
        gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
        gui_fill_rect(0, 0, GUI_W, 56, 0xFF182438u);
        gui_draw_text(28, 10, 2, 0xFFFFE070u, GAME_TITLE " - CD-I STATIC RECOMPILATION");
        gui_draw_text(28, 34, 1, 0xFF88B0E8u, "VERSION " APP_VERSION "  |  LINUX & STEAMOS (STEAM DECK) EDITION");
        gui_draw_text(36, 64, 1, has_img ? 0xFF70F090u : 0xFFFFB060u, status_msg);

        const char *disc_base = "NOT FOUND (CLICK TO BROWSE...)";
        if (has_img && img_out[0]) {
            const char *sl = strrchr(img_out, '/');
            disc_base = sl ? sl + 1 : img_out;
        }
        char labels[LM_COUNT][64];
        char values[LM_COUNT][80];
        snprintf(labels[LM_PLAY], 64, ">> START GAME");
        snprintf(values[LM_PLAY], 80, "%s", has_img ? "READY" : "SELECT DISC IMAGE FIRST");
        snprintf(labels[LM_DISC], 64, "DISC IMAGE (.CUE/.BIN/.CHD)");
        snprintf(values[LM_DISC], 80, "%.52s", disc_base);
        snprintf(labels[LM_STEAM], 64, "STEAM SHORTCUT (NATIVE LINUX)");
        snprintf(values[LM_STEAM], 80, "%s", in_steam ? "REGISTERED (CLICK TO UPDATE / CLEAR PROTON)" : "ADD TO STEAM (NO PROTON)");
        snprintf(labels[LM_UPSCALER], 64, "EDITION / UPSCALER (F8)");
        snprintf(values[LM_UPSCALER], 80, "< %s >", upscaler_label(frontend_get_upscaler()));
        snprintf(labels[LM_ASPECT], 64, "ASPECT RATIO (F9)");
        snprintf(values[LM_ASPECT], 80, "< %s >", render_aspect_name(render_settings.aspect));
        snprintf(labels[LM_CROP], 64, "CROP OVERSCAN BORDERS (F12)");
        snprintf(values[LM_CROP], 80, "< %s >", render_settings.crop_borders ? "ON (FILL 4:3 FRAME)" : "OFF (ORIGINAL BORDERS)");
        snprintf(labels[LM_FILTER], 64, "DISPLAY FILTER (F10)");
        snprintf(values[LM_FILTER], 80, "< %s >", render_filter_name(render_settings.filter));
        snprintf(labels[LM_FULLSCREEN], 64, "WINDOW MODE (F11)");
        snprintf(values[LM_FULLSCREEN], 80, "< %s >", fullscreen ? "FULLSCREEN" : "WINDOWED");
        snprintf(labels[LM_RUNAHEAD], 64, "RUN-AHEAD LATENCY (F6)");
        snprintf(values[LM_RUNAHEAD], 80, "< %d FIELD%s >", frontend_get_runahead(), frontend_get_runahead() == 1 ? "" : "S");
        snprintf(labels[LM_INTERP], 64, "SPRITE INTERPOLATION (F7)");
        snprintf(values[LM_INTERP], 80, "< %s >", frontend_get_interp() ? "ON (60 FPS)" : "OFF");
        snprintf(labels[LM_INVINCIBLE], 64, "PLAYER INVINCIBILITY (F5)");
        snprintf(values[LM_INVINCIBLE], 80, "< %s >", g_invincible ? "ON" : "OFF");
        snprintf(labels[LM_SHOW_LAUNCHER], 64, "SHOW LAUNCHER ON STARTUP");
        snprintf(values[LM_SHOW_LAUNCHER], 80, "< %s >", show_on_start ? "ALWAYS" : "ONLY IF DISC MISSING");
        snprintf(labels[LM_QUIT], 64, "QUIT TO DESKTOP");
        values[LM_QUIT][0] = 0;

        for (int i = 0; i < LM_COUNT; i++) {
            int ry = 80 + i * 27;
            int is_sel = (i == sel);
            uint32_t bg = is_sel ? 0xFF284A78u : (i == LM_PLAY ? 0xFF1A3228u : 0xFF141E30u);
            gui_fill_rect(36, ry, GUI_W - 72, 24, bg);
            if (is_sel) gui_border_rect(36, ry, GUI_W - 72, 24, 1, 0xFFFFE070u);
            uint32_t lcol = is_sel ? 0xFFFFE070u : (i == LM_PLAY ? 0xFF80FFB0u : 0xFFD8E2F0u);
            gui_draw_text(50, ry + 8, 1, lcol, labels[i]);
            gui_draw_text(350, ry + 8, 1, 0xFFFFFFFFu, values[i]);
        }

        gui_fill_rect(0, GUI_H - 36, GUI_W, 36, 0xFF182438u);
        gui_draw_text(28, GUI_H - 22, 1, 0xFFB8C8E0u,
                      "QUICK MENU: [START / BURGER] OR [F1]  |  CD-I PAUSE: [SELECT / VIEW] OR [P]");

        present_raw(gui_fb, GUI_W, GUI_H);
        host_sleep_until(host_time() + 1.0 / 60.0);
    }
    gui_open = 0;
    set_cursor_visible(0);
    return has_img ? 0 : -1;
}

/* In-Game Overlay Menu (triggered by F1 or Steam Deck Select/View/Guide button) */
static void run_ingame_overlay(const uint32_t *game_fb, int gw, int gh) {
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);
    uint32_t *bg_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);
    for (int y = 0; y < GUI_H; y++) {
        int sy = y * gh / GUI_H;
        const uint32_t *srow = game_fb + sy * gw;
        uint32_t *drow = bg_fb + y * GUI_W;
        for (int x = 0; x < GUI_W; x++) {
            uint32_t c = srow[x * gw / GUI_W];
            uint32_t r = ((c >> 16) & 255) / 3;
            uint32_t g = ((c >> 8) & 255) / 3;
            uint32_t b = (c & 255) / 3;
            drow[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }

    enum {
        OM_RESUME,
        OM_SAVE,
        OM_LOAD,
        OM_SLOT,
        OM_INVINCIBLE,
        OM_UPSCALER,
        OM_ASPECT,
        OM_CROP,
        OM_FILTER,
        OM_RUNAHEAD,
        OM_INTERP,
        OM_FULLSCREEN,
        OM_QUIT,
        OM_COUNT
    };
    int sel = OM_RESUME;
    char status_msg[128];
    snprintf(status_msg, sizeof status_msg, "SAVES: %.110s", g_data_dir);

    set_cursor_visible(1);
    gui_nav_up = gui_nav_down = gui_nav_left = gui_nav_right = gui_nav_ok = gui_nav_back = 0;
    mouse_clicked = 0;

    while (gui_open && !input.quit) {
        pump_x11_events();
        gamepad_read_events();

        if (gui_nav_up) { sel = (sel - 1 + OM_COUNT) % OM_COUNT; gui_nav_up = 0; }
        if (gui_nav_down) { sel = (sel + 1) % OM_COUNT; gui_nav_down = 0; }
        if (gui_nav_back) { gui_nav_back = 0; gui_open = 0; break; }

        int gx, gy;
        gui_mouse_coords(&gx, &gy);
        if (mouse_clicked) {
            mouse_clicked = 0;
            for (int i = 0; i < OM_COUNT; i++) {
                int ry = 74 + i * 26;
                if (gx >= 64 && gx <= GUI_W - 64 && gy >= ry && gy < ry + 23) {
                    sel = i;
                    gui_nav_ok = 1;
                    break;
                }
            }
        }

        int dir = 0;
        if (gui_nav_left) { dir = -1; gui_nav_left = 0; }
        if (gui_nav_right) { dir = 1; gui_nav_right = 0; }
        if (gui_nav_ok) { dir = 1; gui_nav_ok = 0; }

        if (dir != 0) {
            int slot = frontend_get_slot();
            switch (sel) {
            case OM_RESUME:
                gui_open = 0;
                break;
            case OM_SAVE:
                if (state_save_slot(slot)) snprintf(status_msg, sizeof status_msg, "SLOT %d: SAVE FAILED", slot);
                else {
                    snprintf(status_msg, sizeof status_msg, "SAVED TO SLOT %d", slot);
                    frontend_osd(status_msg);
                    gui_open = 0;
                }
                break;
            case OM_LOAD:
                if (!state_slot_exists(slot, NULL, 0)) {
                    snprintf(status_msg, sizeof status_msg, "SLOT %d IS EMPTY", slot);
                } else if (state_load_slot(slot)) {
                    snprintf(status_msg, sizeof status_msg, "SLOT %d: CANNOT LOAD", slot);
                } else {
                    snprintf(status_msg, sizeof status_msg, "LOADED SLOT %d", slot);
                    frontend_osd(status_msg);
                    gui_open = 0;
                }
                break;
            case OM_SLOT:
                frontend_set_slot(slot + (dir > 0 ? 1 : -1));
                break;
            case OM_INVINCIBLE:
                g_invincible = !g_invincible;
                snprintf(status_msg, sizeof status_msg, "INVINCIBILITY: %s", g_invincible ? "ON" : "OFF");
                break;
            case OM_UPSCALER:
                frontend_set_upscaler((frontend_get_upscaler() + (dir > 0 ? 1 : CARTOON_MODES - 1)) % CARTOON_MODES);
                update_title();
                break;
            case OM_ASPECT:
                render_settings.aspect = (render_settings.aspect + (dir > 0 ? 1 : RENDER_ASPECTS - 1)) % RENDER_ASPECTS;
                settings_changed();
                break;
            case OM_CROP:
                render_settings.crop_borders = !render_settings.crop_borders;
                settings_changed();
                break;
            case OM_FILTER:
                render_settings.filter = (render_settings.filter + (dir > 0 ? 1 : RENDER_FILTERS - 1)) % RENDER_FILTERS;
                settings_changed();
                break;
            case OM_RUNAHEAD:
                frontend_set_runahead((frontend_get_runahead() + (dir > 0 ? 1 : 3)) % 4);
                break;
            case OM_INTERP:
                frontend_set_interp(!frontend_get_interp());
                break;
            case OM_FULLSCREEN:
                toggle_fullscreen();
                break;
            case OM_QUIT:
                input.quit = 1;
                gui_open = 0;
                break;
            }
        }

        memcpy(gui_fb, bg_fb, GUI_W * GUI_H * 4);
        gui_fill_rect(48, 20, GUI_W - 96, GUI_H - 40, 0xFF101828u);
        gui_border_rect(48, 20, GUI_W - 96, GUI_H - 40, 2, 0xFF4878B8u);
        gui_draw_text(68, 34, 2, 0xFFFFE070u, GAME_TITLE " - QUICK MENU");
        gui_draw_text(68, 56, 1, 0xFF80C0FFu, status_msg);

        int slot = frontend_get_slot();
        char when[32] = "";
        int has_save = state_slot_exists(slot, when, sizeof when);

        char labels[OM_COUNT][48];
        char values[OM_COUNT][64];
        snprintf(labels[OM_RESUME], 48, ">> RESUME GAME");
        snprintf(values[OM_RESUME], 64, "[A / B / START / F1]");
        snprintf(labels[OM_SAVE], 48, "QUICKSAVE TO CURRENT SLOT (F2 / L1)");
        snprintf(values[OM_SAVE], 64, "SLOT %d", slot);
        snprintf(labels[OM_LOAD], 48, "QUICKLOAD FROM CURRENT SLOT (F4 / R1)");
        snprintf(values[OM_LOAD], 64, "%s", has_save ? when : "(EMPTY)");
        snprintf(labels[OM_SLOT], 48, "QUICKSAVE SLOT (F3)");
        snprintf(values[OM_SLOT], 64, "< SLOT %d (%s) >", slot, has_save ? when : "EMPTY");
        snprintf(labels[OM_INVINCIBLE], 48, "PLAYER INVINCIBILITY (F5)");
        snprintf(values[OM_INVINCIBLE], 64, "< %s >", g_invincible ? "ON" : "OFF");
        snprintf(labels[OM_UPSCALER], 48, "EDITION / UPSCALER (F8)");
        snprintf(values[OM_UPSCALER], 64, "< %s >", upscaler_label(frontend_get_upscaler()));
        snprintf(labels[OM_ASPECT], 48, "ASPECT RATIO (F9)");
        snprintf(values[OM_ASPECT], 64, "< %s >", render_aspect_name(render_settings.aspect));
        snprintf(labels[OM_CROP], 48, "CROP OVERSCAN BORDERS (F12)");
        snprintf(values[OM_CROP], 64, "< %s >", render_settings.crop_borders ? "ON (FILL 4:3 FRAME)" : "OFF (ORIGINAL)");
        snprintf(labels[OM_FILTER], 48, "DISPLAY FILTER (F10)");
        snprintf(values[OM_FILTER], 64, "< %s >", render_filter_name(render_settings.filter));
        snprintf(labels[OM_RUNAHEAD], 48, "RUN-AHEAD LATENCY (F6)");
        snprintf(values[OM_RUNAHEAD], 64, "< %d FIELD%s >", frontend_get_runahead(), frontend_get_runahead() == 1 ? "" : "S");
        snprintf(labels[OM_INTERP], 48, "SPRITE INTERPOLATION (F7)");
        snprintf(values[OM_INTERP], 64, "< %s >", frontend_get_interp() ? "ON (60 FPS)" : "OFF");
        snprintf(labels[OM_FULLSCREEN], 48, "FULLSCREEN (F11)");
        snprintf(values[OM_FULLSCREEN], 64, "< %s >", fullscreen ? "ON" : "OFF");
        snprintf(labels[OM_QUIT], 48, "QUIT GAME (ESC)");
        values[OM_QUIT][0] = 0;

        for (int i = 0; i < OM_COUNT; i++) {
            int ry = 74 + i * 26;
            int is_sel = (i == sel);
            uint32_t bg = is_sel ? 0xFF284A78u : 0xFF162236u;
            gui_fill_rect(64, ry, GUI_W - 128, 23, bg);
            if (is_sel) gui_border_rect(64, ry, GUI_W - 128, 23, 1, 0xFFFFE070u);
            gui_draw_text(78, ry + 8, 1, is_sel ? 0xFFFFE070u : 0xFFD8E2F0u, labels[i]);
            gui_draw_text(380, ry + 8, 1, 0xFFFFFFFFu, values[i]);
        }

        gui_draw_text(68, GUI_H - 38, 1, 0xFF98B0D0u,
                      "D-PAD/ARROWS: SELECT   [A/ENTER/LEFT/RIGHT]: CHANGE   [B/START/F1]: RESUME");

        present_raw(gui_fb, GUI_W, GUI_H);
        host_sleep_until(host_time() + 1.0 / 60.0);
    }
    memset(keys, 0, sizeof keys);
    set_cursor_visible(0);
    free(bg_fb);
}

void host_status_banner(const char *title, const char *subtitle) {
    if (g_headless || !dpy || !win) return;
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);
    gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
    gui_fill_rect(60, 180, GUI_W - 120, 120, 0xFF141E30u);
    gui_border_rect(60, 180, GUI_W - 120, 120, 2, 0xFF50A0FFu);
    gui_draw_text(84, 212, 2, 0xFFFFE070u, title ? title : "");
    gui_draw_text(84, 248, 1, 0xFFD0D8E8u, subtitle ? subtitle : "");
    present_raw_sync(gui_fb, GUI_W, GUI_H, 1);
}

/* Interactive Compiler Selection Screen when no system C compiler is installed
   on the host (e.g. SteamOS / Steam Deck).  Returns:
     1 = Download portable Zig/Clang C compiler into saves/derived/compiler/ & compile
     0 = Use built-in 68000 engine */
int host_choose_compiler(const char *exe_dir) {
    if (g_headless) return 0;
    char mode[32] = "";
    ini_get_str(exe_dir, "compiler_mode", mode, sizeof mode);
    if (!strcasecmp(mode, "interp") || !strcasecmp(mode, "builtin")) return 0;
    if (!strcasecmp(mode, "download") || !strcasecmp(mode, "zig")) return 1;

    if (!host_inited) host_init();
    if (!dpy || !win) return 0;
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);

    enum {
        CM_DOWNLOAD = 0,
        CM_BUILTIN_ONCE = 1,
        CM_BUILTIN_ALWAYS = 2,
        CM_COUNT = 3
    };
    int sel = CM_DOWNLOAD;
    int choice = 0;

    set_cursor_visible(1);
    gui_open = 1;
    gui_nav_up = gui_nav_down = gui_nav_left = gui_nav_right = gui_nav_ok = gui_nav_back = 0;
    mouse_clicked = 0;

    while (!input.quit) {
        pump_x11_events();
        gamepad_read_events();

        if (gui_nav_up) { sel = (sel - 1 + CM_COUNT) % CM_COUNT; gui_nav_up = 0; }
        if (gui_nav_down) { sel = (sel + 1) % CM_COUNT; gui_nav_down = 0; }
        if (gui_nav_back) { gui_nav_back = 0; choice = 0; break; }

        int gx, gy;
        gui_mouse_coords(&gx, &gy);
        if (mouse_clicked) {
            mouse_clicked = 0;
            for (int i = 0; i < CM_COUNT; i++) {
                int ry = 215 + i * 54;
                if (gx >= 48 && gx <= GUI_W - 48 && gy >= ry && gy < ry + 44) {
                    sel = i;
                    gui_nav_ok = 1;
                    break;
                }
            }
        }

        if (gui_nav_ok) {
            gui_nav_ok = 0;
            if (sel == CM_DOWNLOAD) {
                choice = 1;
            } else if (sel == CM_BUILTIN_ONCE) {
                choice = 0;
            } else if (sel == CM_BUILTIN_ALWAYS) {
                ini_set_str(exe_dir, "compiler_mode", "interp");
                choice = 0;
            }
            break;
        }

        gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
        gui_fill_rect(0, 0, GUI_W, 60, 0xFF182438u);
        gui_draw_text(28, 12, 2, 0xFFFFE070u, "NO SYSTEM C COMPILER FOUND");
        gui_draw_text(28, 38, 1, 0xFF88B0E8u, "CHOOSE HOW TO RUN THE DERIVED 68000 GAME CODE");

        gui_fill_rect(48, 78, GUI_W - 96, 115, 0xFF141E30u);
        gui_border_rect(48, 78, GUI_W - 96, 115, 1, 0xFF3A5888u);
        gui_draw_text(64, 92, 1, 0xFFD8E4F8u,
                      "THE 68000 MODULES FROM YOUR DISC WERE TRANSLATED TO C IN SAVES/DERIVED/GEN/.");
        gui_draw_text(64, 112, 1, 0xFFD8E4F8u,
                      "NO SYSTEM C COMPILER (GCC / CLANG) WAS DETECTED ON THIS DEVICE.");
        gui_draw_text(64, 138, 1, 0xFF80F0A8u,
                      "* OPTION 1: DOWNLOAD A PORTABLE C COMPILER (ZIG/CLANG) INTO SAVES/DERIVED/COMPILER/");
        gui_draw_text(64, 156, 1, 0xFF80F0A8u,
                      "  AND COMPILE NATIVE X86_64 CODE (RECOMP_USER.SO) ON THE FLY (~45 MB, ONE-TIME).");
        gui_draw_text(64, 174, 1, 0xFFB8C8E0u,
                      "* OPTION 2/3: RUN IMMEDIATELY WITH THE BUILT-IN 68000 ENGINE (NO DOWNLOAD NEEDED).");

        const char *titles[CM_COUNT] = {
            ">> DOWNLOAD PORTABLE C COMPILER & BUILD NATIVE CODE (RECOMMENDED)",
            ">> USE BUILT-IN 68000 ENGINE (PLAY IMMEDIATELY, ASK NEXT TIME)",
            ">> ALWAYS USE BUILT-IN 68000 ENGINE (PLAY IMMEDIATELY, DON'T ASK AGAIN)"
        };
        const char *subs[CM_COUNT] = {
            "DOWNLOADS PORTABLE ZIG CC TO SAVES/DERIVED/COMPILER/ & BUILDS RECOMP_USER.SO",
            "STARTS THE GAME RIGHT NOW WITHOUT DOWNLOADING ANYTHING",
            "SAVES COMPILER_MODE=INTERP IN APPRENTICE.INI FOR FUTURE LAUNCHES"
        };

        for (int i = 0; i < CM_COUNT; i++) {
            int ry = 215 + i * 54;
            int is_sel = (i == sel);
            uint32_t bg = is_sel ? 0xFF284A78u : (i == CM_DOWNLOAD ? 0xFF1A3228u : 0xFF162236u);
            gui_fill_rect(48, ry, GUI_W - 96, 44, bg);
            if (is_sel) gui_border_rect(48, ry, GUI_W - 96, 44, 2, 0xFFFFE070u);
            uint32_t tcol = is_sel ? 0xFFFFE070u : (i == CM_DOWNLOAD ? 0xFF80FFB0u : 0xFFFFFFFFu);
            gui_draw_text(64, ry + 10, 1, tcol, titles[i]);
            gui_draw_text(64, ry + 26, 1, 0xFFB0C4E0u, subs[i]);
        }

        gui_fill_rect(0, GUI_H - 36, GUI_W, 36, 0xFF182438u);
        gui_draw_text(28, GUI_H - 22, 1, 0xFFB8C8E0u,
                      "D-PAD / ARROWS: SELECT OPTION   |   [A / ENTER]: CONFIRM   |   [B / ESC]: BUILT-IN ENGINE");

        present_raw(gui_fb, GUI_W, GUI_H);
        host_sleep_until(host_time() + 1.0 / 60.0);
    }

    gui_open = 0;
    memset(keys, 0, sizeof keys);
    set_cursor_visible(0);
    return choice;
}

void host_fatal_box(const char *msg) {
    if (access("/usr/bin/zenity", X_OK) == 0) {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "/usr/bin/zenity --error --title='Recomp fatal error' --text='%s' 2>/dev/null", msg);
        int r = system(cmd); (void)r;
    } else if (access("/usr/bin/kdialog", X_OK) == 0) {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, "/usr/bin/kdialog --error '%s' --title 'Recomp fatal error' 2>/dev/null", msg);
        int r = system(cmd); (void)r;
    }
}

/* ---------------------------------------------------------------- Frame Dump & Presentation */
static double next_frame_time = -1;
static unsigned long long frame_counter;

#pragma pack(push, 1)
typedef struct {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BmpFileHdr;

typedef struct {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BmpInfoHdr;
#pragma pack(pop)

static void dump_bmp(const uint32_t *fb, int w, int h, unsigned long long n) {
    char name[600];
    snprintf(name, sizeof name, "%s/frame_%06llu.bmp", g_data_dir, n);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    BmpFileHdr bf = {0};
    BmpInfoHdr bi = {0};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
    bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + (uint32_t)(w * h * 4);
    fwrite(&bf, sizeof bf, 1, f);
    fwrite(&bi, sizeof bi, 1, f);
    fwrite(fb, 4, (size_t)w * h, f);
    fclose(f);
}

void host_present(const uint32_t *fb, int w, int h) {
    frame_counter++;
    if (g_dump_frames && frame_counter >= g_dump_from && frame_counter % (unsigned)g_dump_frames == 0)
        dump_bmp(fb, w, h, frame_counter);
    {
        extern uint8_t *RAM;
        for (int i = 0; i < g_nramdump; i++) if (frame_counter == g_ramdump_at[i]) {
            char rp[600]; snprintf(rp, sizeof rp, "%s/ram_%06u.bin", g_data_dir, g_ramdump_at[i]);
            FILE *rf = fopen(rp, "wb"); if (rf) { fwrite(RAM, 1, 0x01000000, rf); fclose(rf); }
        }
    }
    { extern int g_callprof; if (g_callprof_from && frame_counter >= (unsigned long long)g_callprof_from) g_callprof = 1; }
    if (g_max_frames && frame_counter >= (unsigned long long)g_max_frames) {
        { extern int g_callprof; void callprof_dump(void); if (g_callprof) callprof_dump(); }
        LOG("reached %d fields, exiting", g_max_frames);
        exit(0);
    }
    if (!g_headless && gui_open) {
        run_ingame_overlay(fb, w, h);
        next_frame_time = -1;
    }
    if (g_turbo || g_headless) {
        present_raw(fb, w, h);
        return;
    }
    static double prof_t0 = 0, prof_sleep_sum = 0;
    static int prof_frames = 0;
    double t_before = host_time();
    if (prof_t0 == 0) prof_t0 = t_before;
    if (next_frame_time < 0) next_frame_time = t_before;
    next_frame_time += 1.0 / FIELD_RATE;

    /* Hand the finished 60 Hz field to the dedicated OpenGL presenter thread
       (non-blocking mailbox copy, ~0.04 ms).  The presenter thread blocks on
       glXSwapBuffers at whatever refresh rate Gamescope/display dictates
       (60, 45, 40, 30, 24, 15 FPS) while the 60 Hz CD-i emulation and 44.1 kHz
       audio continue ticking at an exact 16.67 ms period. */
    present_raw(fb, w, h);
    double now = host_time();

    /* Gentle phase-lock (PLL): keep the emulation thread's frame publish time
       centered (~8.3 ms) between compositor vblanks so frame completion never
       collides with glXSwapBuffers wakeup at 30/60 Hz. */
    if (pres_thread_started) {
        double last_vblank = 0.0, vdt = 0.0;
        pthread_mutex_lock(&pres_mtx);
        last_vblank = pres_last_swap_done;
        vdt = pres_vblank_dt;
        pthread_mutex_unlock(&pres_mtx);
        if (last_vblank > 0.0 && (now - last_vblank) < 0.100 && vdt >= 0.015) {
            double period = 1.0 / FIELD_RATE;
            double delta = now - last_vblank;
            int k = (int)(delta / period);
            double phase = delta - (double)k * period;
            if (phase < 0.0) phase += period;
            double err = (0.5 * period) - phase;
            if (err > 0.00025) err = 0.00025;
            else if (err < -0.00025) err = -0.00025;
            next_frame_time += err;
        }
    }

    double sleep_dt = 0;
    if (now < next_frame_time) {
        host_sleep_until(next_frame_time);
        double after_sleep = host_time();
        sleep_dt = after_sleep - now;
        now = after_sleep;
    } else if (now - next_frame_time > 2.0 / FIELD_RATE) {
        next_frame_time = now;
    }

    prof_sleep_sum += sleep_dt;
    prof_frames++;
    if (now - prof_t0 >= 2.0) {
        double elapsed = now - prof_t0;
        double swap_sum = 0.0;
        int presents = 0, hw_vsync = 0;
        if (pres_thread_started) {
            pthread_mutex_lock(&pres_mtx);
            swap_sum = pres_swap_sum;
            presents = pres_swap_count;
            hw_vsync = pres_last_hw_vsync;
            pres_swap_sum = 0.0;
            pres_swap_count = 0;
            pthread_mutex_unlock(&pres_mtx);
        } else {
            presents = prof_frames;
        }
        int pcount = presents > 0 ? presents : 1;
        LOG("perf: %.1f sim_fps / %.1f disp_fps (avg field %.2f ms, glXSwap %.2f ms, sleep %.2f ms, hw_vsync=%d, audio_q=%d)",
            prof_frames / elapsed,
            presents / elapsed,
            elapsed * 1000.0 / prof_frames,
            swap_sum * 1000.0 / pcount,
            prof_sleep_sum * 1000.0 / prof_frames,
            hw_vsync,
            host_audio_queued());
        prof_t0 = now;
        prof_sleep_sum = 0;
        prof_frames = 0;
    }
}

/* ---------------------------------------------------------------- Scripted Input & Poll */
typedef struct { long field; char key; int dur; } ScriptEv;
static ScriptEv script[256];
static int nscript;

void host_set_script(const char *s) {
    while (*s && nscript < 256) {
        ScriptEv *e = &script[nscript];
        e->field = strtol(s, (char **)&s, 10);
        if (*s == ':') s++;
        e->key = *s ? *s++ : '1';
        e->dur = 6;
        if (*s == ':') { s++; e->dur = (int)strtol(s, (char **)&s, 10); }
        nscript++;
        if (*s == ',') s++; else break;
    }
}

void host_poll(void) {
    if (!g_headless) {
        set_cursor_visible(0);
        pump_x11_events();
        gamepad_read_events();
    }
    int speed = keys[K_SHIFT] ? 12 : 6;
    input.dx = (keys[K_RIGHT] ? speed : 0) - (keys[K_LEFT] ? speed : 0);
    input.dy = (keys[K_DOWN] ? speed : 0) - (keys[K_UP] ? speed : 0);
    input.b1 = keys[K_Z] || keys[K_SPACE] || keys[K_A] || keys[K_C] || keys[K_RETURN];
    input.b2 = keys[K_X] || keys[K_CTRL] || keys[K_S] || keys[K_C] || keys[K_RETURN];
    input.b3 = 0;
    if (!g_headless) gamepad_apply_game_input();
    g_turbo = keys[K_TAB] || (js_fd >= 0 && js_axes[5] > 16000);
    if (g_fuzz) {
        static int hold, fdx, fdy, fb1, fb2;
        static uint32_t st;
        if (!st) st = (uint32_t)g_fuzz * 2654435761u + 1;
        if (--hold <= 0) {
            st = st * 1103515245u + 12345u; uint32_t r = st >> 8;
            hold = 2 + (int)(r % 40);
            int dir = (int)((r >> 6) % 9);
            fdx = (dir % 3 - 1) * 6; fdy = (dir / 3 - 1) * 6;
            fb1 = ((r >> 10) % 4) == 0; fb2 = ((r >> 13) % 6) == 0;
        }
        input.dx = fdx; input.dy = fdy; input.b1 = fb1; input.b2 = fb2;
    }
    for (int i = 0; i < nscript; i++) {
        ScriptEv *e = &script[i];
        if ((long long)frame_counter == e->field) {
            if (e->key == 'S') push_hotkey(HOTKEY_SAVE);
            if (e->key == 'N') push_hotkey(HOTKEY_SLOT);
            if (e->key == 'O') push_hotkey(HOTKEY_LOAD);
            if (e->key == 'I') push_hotkey(HOTKEY_INVINCIBLE);
        }
        if ((long long)frame_counter >= e->field && (long long)frame_counter < e->field + e->dur) {
            switch (e->key) {
            case '1': input.b1 = 1; break;
            case '2': input.b2 = 1; break;
            case 'L': input.dx = -6; break;
            case 'R': input.dx = 6; break;
            case 'U': input.dy = -6; break;
            case 'D': input.dy = 6; break;
            }
        }
    }
    if (input.quit) {
        LOG("quit requested");
        exit(0);
    }
}

/* ---------------------------------------------------------------- Audio (PulseAudio / PipeWire / ALSA via dlopen) */
/* Lock-free ring buffer (16384 stereo frames = ~371 ms at 44.1 kHz) fed directly
   by audio_field_tick() and drained in 1-field (735-frame = 16.67 ms) chunks by
   the dedicated audio thread. */
#define AUD_RING_FRAMES 16384
#define AUD_RING_MASK   (AUD_RING_FRAMES - 1)
#define AUD_CHUNK       735
static int16_t aud_ring[AUD_RING_FRAMES * 2];
static volatile uint32_t aud_wr, aud_rd;
static pthread_mutex_t aud_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t aud_cv = PTHREAD_COND_INITIALIZER;
static int aud_running;

/* PulseAudio simple API types (loaded dynamically via libpulse-simple.so.0) */
typedef struct pa_simple pa_simple;
typedef struct {
    int format;         /* PA_SAMPLE_S16LE = 3 */
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;
typedef struct {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
} pa_buffer_attr;

typedef pa_simple *(*pa_simple_new_fn)(const char *, const char *, int, const char *, const char *,
                                       const pa_sample_spec *, const void *, const pa_buffer_attr *, int *);
typedef int (*pa_simple_write_fn)(pa_simple *, const void *, size_t, int *);
typedef void (*pa_simple_free_fn)(pa_simple *);

static pa_simple *pa_dev;
static pa_simple_write_fn p_pa_write;

/* ALSA PCM API types (loaded dynamically via libasound.so.2) */
typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef int (*snd_pcm_open_fn)(snd_pcm_t **, const char *, int, int);
typedef int (*snd_pcm_set_params_fn)(snd_pcm_t *, int, int, unsigned int, unsigned int, int, unsigned int);
typedef snd_pcm_sframes_t (*snd_pcm_writei_fn)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
typedef int (*snd_pcm_recover_fn)(snd_pcm_t *, int, int);

static snd_pcm_t *alsa_dev;
static snd_pcm_writei_fn p_alsa_writei;
static snd_pcm_recover_fn p_alsa_recover;

static void *audio_thread(void *arg) {
    (void)arg;
    int16_t chunk[AUD_CHUNK * 2];
    int prefill = 1;
    while (aud_running) {
        uint32_t avail;
        pthread_mutex_lock(&aud_mtx);
        for (;;) {
            avail = aud_wr - aud_rd;
            /* Wait for 4 fields (~66.7 ms) on initial startup or after an underrun,
               and 1 full field (735 frames = 16.67 ms) during steady playback. */
            uint32_t need = prefill ? (AUD_CHUNK * 4) : AUD_CHUNK;
            if (!aud_running || avail >= need) break;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 25000000L; /* 25 ms timeout */
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&aud_cv, &aud_mtx, &ts) == ETIMEDOUT) {
                avail = aud_wr - aud_rd;
                if (avail < AUD_CHUNK) prefill = 1;
                break;
            }
        }
        pthread_mutex_unlock(&aud_mtx);
        if (!aud_running) break;
        if (avail < (prefill ? (AUD_CHUNK * 4) : AUD_CHUNK)) continue;
        if (prefill) {
            /* Pre-prime PulseAudio / PipeWire's internal playback buffer with 2
               silent fields when starting or resuming from the pause menu so it
               doesn't immediately swallow all 4 fields from aud_ring at once. */
            memset(chunk, 0, (size_t)AUD_CHUNK * 4);
            if (pa_dev && p_pa_write) {
                int err = 0;
                p_pa_write(pa_dev, chunk, (size_t)AUD_CHUNK * 4, &err);
                p_pa_write(pa_dev, chunk, (size_t)AUD_CHUNK * 4, &err);
            }
            prefill = 0;
        }

        uint32_t n = AUD_CHUNK;
        uint32_t r = aud_rd;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t idx = (r + i) & AUD_RING_MASK;
            chunk[i * 2]     = aud_ring[idx * 2];
            chunk[i * 2 + 1] = aud_ring[idx * 2 + 1];
        }
        __atomic_store_n(&aud_rd, r + n, __ATOMIC_RELEASE);

        if (pa_dev && p_pa_write) {
            int err = 0;
            p_pa_write(pa_dev, chunk, (size_t)n * 4, &err);
        } else if (alsa_dev && p_alsa_writei) {
            snd_pcm_sframes_t w = p_alsa_writei(alsa_dev, chunk, n);
            if (w < 0 && p_alsa_recover) p_alsa_recover(alsa_dev, (int)w, 1);
        }
    }
    return NULL;
}

void host_audio_init(int rate) {
    if (g_headless) return;
    /* 1. Try PulseAudio / PipeWire-Pulse */
    void *pa_lib = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    if (pa_lib) {
        pa_simple_new_fn p_new = (pa_simple_new_fn)dlsym(pa_lib, "pa_simple_new");
        p_pa_write = (pa_simple_write_fn)dlsym(pa_lib, "pa_simple_write");
        if (p_new && p_pa_write) {
            pa_sample_spec ss = {3 /* PA_SAMPLE_S16LE */, (uint32_t)rate, 2};
            pa_buffer_attr ba = {
                (uint32_t)(AUD_CHUNK * 4 * 16),   /* maxlength (~266 ms) */
                (uint32_t)(AUD_CHUNK * 4 * 6),    /* tlength   (~100 ms = 6 fields) */
                (uint32_t)(AUD_CHUNK * 4 * 2),    /* prebuf    (~33.3 ms = 2 fields) */
                (uint32_t)(AUD_CHUNK * 4),        /* minreq    (~16.7 ms = 1 field) */
                (uint32_t)-1                      /* fragsize */
            };
            int err = 0;
            pa_dev = p_new(NULL, GAME_TITLE, 1 /* PA_STREAM_PLAYBACK */, NULL, "Game Audio", &ss, NULL, &ba, &err);
            if (pa_dev) LOG("audio: PulseAudio / PipeWire (%d Hz)", rate);
        }
    }
    /* 2. Fallback to ALSA */
    if (!pa_dev) {
        void *alsa_lib = dlopen("libasound.so.2", RTLD_LAZY);
        if (alsa_lib) {
            snd_pcm_open_fn p_open = (snd_pcm_open_fn)dlsym(alsa_lib, "snd_pcm_open");
            snd_pcm_set_params_fn p_params = (snd_pcm_set_params_fn)dlsym(alsa_lib, "snd_pcm_set_params");
            p_alsa_writei = (snd_pcm_writei_fn)dlsym(alsa_lib, "snd_pcm_writei");
            p_alsa_recover = (snd_pcm_recover_fn)dlsym(alsa_lib, "snd_pcm_recover");
            if (p_open && p_params && p_alsa_writei &&
                p_open(&alsa_dev, "default", 0 /* SND_PCM_STREAM_PLAYBACK */, 0) == 0) {
                /* SND_PCM_FORMAT_S16_LE = 2, SND_PCM_ACCESS_RW_INTERLEAVED = 3 */
                if (p_params(alsa_dev, 2, 3, 2, (unsigned)rate, 1, 65000) == 0) {
                    LOG("audio: ALSA (%d Hz)", rate);
                } else {
                    alsa_dev = NULL;
                }
            }
        }
    }
    if (!pa_dev && !alsa_dev) {
        LOG("audio: no PulseAudio/PipeWire or ALSA output available; audio muted");
        return;
    }
    aud_running = 1;
    pthread_t th;
    if (pthread_create(&th, NULL, audio_thread, NULL) == 0) pthread_detach(th);
}

int host_audio_queued(void) {
    uint32_t avail = __atomic_load_n(&aud_wr, __ATOMIC_ACQUIRE) - __atomic_load_n(&aud_rd, __ATOMIC_ACQUIRE);
    return (int)(avail / 735);
}

static FILE *wavf;
static long wav_frames;

void host_wav_open(const char *path) {
    wavf = fopen(path, "wb");
    if (wavf) { uint8_t hdr[44] = {0}; fwrite(hdr, 1, 44, wavf); }
}

static void wav_close(void) {
    if (!wavf) return;
    uint32_t data = (uint32_t)wav_frames * 4;
    uint8_t h[44];
    memcpy(h, "RIFF", 4); *(uint32_t *)(h + 4) = 36 + data; memcpy(h + 8, "WAVEfmt ", 8);
    *(uint32_t *)(h + 16) = 16; *(uint16_t *)(h + 20) = 1; *(uint16_t *)(h + 22) = 2;
    *(uint32_t *)(h + 24) = 44100; *(uint32_t *)(h + 28) = 44100 * 4; *(uint16_t *)(h + 32) = 4;
    *(uint16_t *)(h + 34) = 16; memcpy(h + 36, "data", 4); *(uint32_t *)(h + 40) = data;
    fseek(wavf, 0, SEEK_SET); fwrite(h, 1, 44, wavf); fclose(wavf); wavf = NULL;
}

void host_audio_push(const int16_t *s, int frames) {
    if (wavf) {
        fwrite(s, 4, (size_t)frames, wavf);
        wav_frames += frames;
        static int reg;
        if (!reg) { reg = 1; atexit(wav_close); }
    }
    if (!aud_running || frames <= 0) return;
    uint32_t w = __atomic_load_n(&aud_wr, __ATOMIC_RELAXED);
    uint32_t r = __atomic_load_n(&aud_rd, __ATOMIC_ACQUIRE);
    uint32_t queued = w - r;
    uint32_t free_space = AUD_RING_FRAMES - queued;
    uint32_t n = (uint32_t)frames;
    if (n > free_space) n = free_space;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (w + i) & AUD_RING_MASK;
        aud_ring[idx * 2]     = s[i * 2];
        aud_ring[idx * 2 + 1] = s[i * 2 + 1];
    }
    w += n;
    /* In Steam Deck Gaming Mode, Gamescope's display refresh is ~59.94 Hz while
       the emulated CD-i clock generates 735 * 60 = 44100 samples/sec.  If our
       ring buffer runs below 3 fields during active audio, gently duplicate the
       last sample frame (faster when < 1 field, 2 frames when 1-2 fields) so the
       ring cushion stays at 3-4 fields without ever hitting a PipeWire underrun. */
    if (queued < AUD_CHUNK * 3 && n >= 700 && (AUD_RING_FRAMES - (w - r)) >= 8) {
        int pad_n = (queued < AUD_CHUNK) ? 6 : 2;
        int16_t lastL = s[(n - 1) * 2], lastR = s[(n - 1) * 2 + 1];
        for (int pad = 0; pad < pad_n; pad++) {
            uint32_t idx = (w + (uint32_t)pad) & AUD_RING_MASK;
            aud_ring[idx * 2]     = lastL;
            aud_ring[idx * 2 + 1] = lastR;
        }
        w += (uint32_t)pad_n;
    }
    __atomic_store_n(&aud_wr, w, __ATOMIC_RELEASE);
    pthread_mutex_lock(&aud_mtx);
    pthread_cond_signal(&aud_cv);
    pthread_mutex_unlock(&aud_mtx);
}
#endif
