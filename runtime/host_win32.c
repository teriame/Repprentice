/* Win32 host platform: window, Direct3D 11 / GDI presentation, XInput gamepad,
   interactive GUI launcher, in-window file browser (.cue/.bin/.chd), in-game
   Quick Menu overlay (F1 / Select / View), and audio. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "render.h"
#include "cartoon.h"
#include "state.h"
#include "game_config.h"

static int fullscreen;
static int host_inited;
static void save_settings(void);
extern void frontend_osd(const char *msg);
extern int  runtime_extract_embedded_tools(char *out_dir, size_t n);
extern void runtime_cleanup_embedded_tools(const char *tool_dir);

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comdlg32.lib")

static HWND hwnd;
static LARGE_INTEGER qpf, qp0;
static int keys[256];
int g_headless;
static int win_scale = 3;
static int cursor_hidden;

/* GUI & overlay state */
static int gui_open;
static int gui_nav_up, gui_nav_down, gui_nav_left, gui_nav_right, gui_nav_ok, gui_nav_back;
static int mouse_x, mouse_y, mouse_clicked;

static void set_cursor_visible(int vis) {
    if (g_headless) return;
    if (vis && cursor_hidden) {
        ShowCursor(TRUE);
        cursor_hidden = 0;
    } else if (!vis && !cursor_hidden) {
        ShowCursor(FALSE);
        cursor_hidden = 1;
    }
}

static WINDOWPLACEMENT saved_wp = {sizeof(WINDOWPLACEMENT)};

static void toggle_fullscreen(void) {
    if (!hwnd) return;
    DWORD style = GetWindowLongA(hwnd, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(hwnd, &saved_wp) &&
            GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            if (!gui_open) set_cursor_visible(0);
        }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hwnd, &saved_wp);
        SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen = !fullscreen;
    save_settings();
}

/* ---------------------------------------------------------------- video settings */
static int use_gpu = 1, start_fullscreen, win_w, win_h;
static int gpu_ok;

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

static void update_title(void) {
    if (!hwnd) return;
    char t[320];
    const char *ed = g_edition_hd ? " HD" : "";
    const char *keys_hd = g_edition_hd ? "F8 upscaler, " : "";
    if (gpu_ok)
        snprintf(t, sizeof t, GAME_TITLE "%s  [%s, %s]  F1 menu, F2 save, F3 slot, F4 load, F5 invincible, F6 run-ahead, "
                 "F7 interpolation, %sF9 aspect, F10 filter, F11 fullscreen, F12 crop", ed,
                 render_aspect_name(render_settings.aspect), render_filter_name(render_settings.filter), keys_hd);
    else
        snprintf(t, sizeof t, GAME_TITLE "%s  (GDI)", ed);
    SetWindowTextA(hwnd, t);
}

static void settings_changed(void) {
    update_title();
    save_settings();
}

/* command-line overrides: --aspect N, --filter N, --fullscreen, --windowed, --gdi, --size WxH */
void host_video_option(const char *opt, const char *val) {
    if (!strcmp(opt, "--aspect") && val) render_settings.aspect = atoi(val) % RENDER_ASPECTS;
    else if (!strcmp(opt, "--filter") && val) render_settings.filter = atoi(val) % RENDER_FILTERS;
    else if (!strcmp(opt, "--fullscreen")) start_fullscreen = 1;
    else if (!strcmp(opt, "--windowed")) start_fullscreen = 0;
    else if (!strcmp(opt, "--gdi")) use_gpu = 0;
    else if (!strcmp(opt, "--size") && val) sscanf(val, "%dx%d", &win_w, &win_h);
}

/* front end hotkeys (quicksave, invincibility, enhancements), taken by
   frontend.c between fields */
static int hotq[16], hotn;
static void push_hotkey(int k) { if (hotn < 16) hotq[hotn++] = k; }
int host_take_hotkey(void) {
    if (!hotn) return 0;
    int k = hotq[0];
    memmove(hotq, hotq + 1, (size_t)--hotn * sizeof hotq[0]);
    return k;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE: input.quit = 1; return 0;
    case WM_MOUSEMOVE:
        mouse_x = (int)(short)LOWORD(l);
        mouse_y = (int)(short)HIWORD(l);
        return 0;
    case WM_LBUTTONDOWN:
        mouse_x = (int)(short)LOWORD(l);
        mouse_y = (int)(short)HIWORD(l);
        mouse_clicked = 1;
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        keys[w & 255] = 1;
        if (gui_open) {
            if (w == VK_UP || w == 'W') gui_nav_up = 1;
            else if (w == VK_DOWN || w == 'S') gui_nav_down = 1;
            else if (w == VK_LEFT || w == 'A') gui_nav_left = 1;
            else if (w == VK_RIGHT || w == 'D') gui_nav_right = 1;
            else if (w == VK_RETURN || w == VK_SPACE || w == 'Z') gui_nav_ok = 1;
            else if (w == VK_ESCAPE || w == VK_BACK || w == 'X') gui_nav_back = 1;
            else if (w == VK_F1 && !(l & 0x40000000)) gui_open = 0;
            else if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
                toggle_fullscreen();
                keys[VK_RETURN] = 0;
            }
            return 0;
        }
        if (w == VK_ESCAPE) input.quit = 1;
        if (!(l & 0x40000000)) {   /* not auto-repeat */
            if (w == VK_F1) { gui_open = !gui_open; return 0; }
            if (w == 'P' || w == VK_PAUSE) g_cdi_pause_req = 4;
            if (w == VK_F2) push_hotkey(HOTKEY_SAVE);
            if (w == VK_F3) push_hotkey(HOTKEY_SLOT);
            if (w == VK_F4) push_hotkey(HOTKEY_LOAD);
            if (w == VK_F5) push_hotkey(HOTKEY_INVINCIBLE);
            if (w == VK_F6) push_hotkey(HOTKEY_RUNAHEAD);
            if (w == VK_F7) push_hotkey(HOTKEY_INTERP);
            if (w == VK_F8) push_hotkey(HOTKEY_UPSCALER);
            if (w == VK_F9) { render_settings.aspect = (render_settings.aspect + 1) % RENDER_ASPECTS; settings_changed(); }
            if (w == VK_F10) { render_settings.filter = (render_settings.filter + 1) % RENDER_FILTERS; settings_changed(); }
            if (w == VK_F11 || (w == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) {
                toggle_fullscreen();
                keys[VK_RETURN] = 0;
            }
            if (w == VK_F12) {
                render_settings.crop_borders = !render_settings.crop_borders;
                settings_changed();
                frontend_osd(render_settings.crop_borders ? "Crop overscan borders: on" : "Crop overscan borders: off");
            }
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && cursor_hidden) { SetCursor(NULL); return TRUE; }
        break;
    case WM_SYSCHAR:
        return 0;   /* no menu beep on Alt+Enter */
    case WM_ERASEBKGND:
        return 1;
    case WM_KEYUP: case WM_SYSKEYUP:
        keys[w & 255] = 0;
        return 0;
    case WM_KILLFOCUS:
        memset(keys, 0, sizeof keys);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump_win32_messages(void) {
    if (g_headless) return;
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

int host_init(void) {
    if (host_inited) return 0;
    host_inited = 1;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&qp0);
    timeBeginPeriod(1);
    if (g_headless) return 0;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CDiRecomp";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    SetProcessDPIAware();
    int save_aspect = render_settings.aspect, save_filter = render_settings.filter;
    int cli_fs = start_fullscreen;
    load_settings();
    if (save_aspect) render_settings.aspect = save_aspect;
    if (save_filter) render_settings.filter = save_filter;
    if (cli_fs) start_fullscreen = 1;
    int cw = win_w > 0 ? win_w : 320 * win_scale, chh = win_h > 0 ? win_h : 240 * win_scale;
    if (win_w <= 0) {
        int sh = GetSystemMetrics(SM_CYSCREEN);
        chh = sh * 3 / 4;
        cw = chh * 16 / 9;
    }
    RECT r = {0, 0, cw, chh};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, GAME_TITLE,
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return -1;
    if (use_gpu) gpu_ok = render_init(hwnd) == 0;
    if (start_fullscreen) toggle_fullscreen();
    update_title();
    return 0;
}

double host_time(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - qp0.QuadPart) / (double)qpf.QuadPart;
}

void host_sleep_until(double t) {
    for (;;) {
        double now = host_time();
        if (now >= t) return;
        double d = t - now;
        if (d > 0.002) Sleep((DWORD)((d - 0.001) * 1000));
    }
}

/* Present a raw ARGB buffer without frame-counter increment */
static void present_raw(const uint32_t *fb, int w, int h) {
    if (g_headless || !hwnd) return;
    if (gpu_ok && render_present(fb, w, h) != 0) {
        LOG("GPU present failed; falling back to GDI");
        gpu_ok = 0;
        update_title();
    }
    if (!gpu_ok) {
        HDC dc = GetDC(hwnd);
        RECT r;
        GetClientRect(hwnd, &r);
        BITMAPINFO bi = {0};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);
        int cw = r.right > 0 ? r.right : 1, ch = r.bottom > 0 ? r.bottom : 1;
        int dw = cw, dh = cw * 3 / 4;
        if (dh > ch) { dh = ch; dw = ch * 4 / 3; }
        int dx = (cw - dw) / 2, dy = (ch - dh) / 2;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (dy > 0) { RECT a = {0, 0, cw, dy}, b = {0, dy + dh, cw, ch}; FillRect(dc, &a, black); FillRect(dc, &b, black); }
        if (dx > 0) { RECT a = {0, 0, dx, ch}, b = {dx + dw, 0, cw, ch}; FillRect(dc, &a, black); FillRect(dc, &b, black); }
        StretchDIBits(dc, dx, dy, dw, dh, 0, 0, w, h, fb, &bi, DIB_RGB_COLORS, SRCCOPY);
        ReleaseDC(hwnd, dc);
    }
}

/* ---------------------------------------------------------------- XInput (loaded dynamically) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XIGamepad;
typedef struct { DWORD dwPacketNumber; XIGamepad Gamepad; } XIState;
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XIState *);
static XInputGetStateFn xinput_get;
static int xinput_tried;
static WORD prev_xi_btns;
static int prev_xi_hat_x, prev_xi_hat_y;
static int xi_rt_held;

static void xinput_load(void) {
    xinput_tried = 1;
    const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !xinput_get; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (m) xinput_get = (XInputGetStateFn)GetProcAddress(m, "XInputGetState");
    }
}

static void gamepad_poll_events(int apply_game_input) {
    if (!xinput_tried) xinput_load();
    if (!xinput_get) return;
    for (DWORD pad = 0; pad < 4; pad++) {
        XIState st;
        if (xinput_get(pad, &st) != 0) continue;
        XIGamepad *g = &st.Gamepad;
        WORD b = g->wButtons;
        WORD pressed = b & ~prev_xi_btns;
        prev_xi_btns = b;
        xi_rt_held = (g->bRightTrigger > 128);

        /* Start / Menu / Burger (0x0010) toggles in-game Quick Menu */
        if (pressed & 0x0010) gui_open = !gui_open;

        int hx = 0, hy = 0;
        if (b & 0x0001) hy = -1;
        else if (b & 0x0002) hy = 1;
        else if (g->sThumbLY > 16000) hy = -1;
        else if (g->sThumbLY < -16000) hy = 1;

        if (b & 0x0004) hx = -1;
        else if (b & 0x0008) hx = 1;
        else if (g->sThumbLX < -16000) hx = -1;
        else if (g->sThumbLX > 16000) hx = 1;

        if (gui_open) {
            if (hy < 0 && prev_xi_hat_y >= 0) gui_nav_up = 1;
            if (hy > 0 && prev_xi_hat_y <= 0) gui_nav_down = 1;
            if (hx < 0 && prev_xi_hat_x >= 0) gui_nav_left = 1;
            if (hx > 0 && prev_xi_hat_x <= 0) gui_nav_right = 1;
            if (pressed & 0x1000) gui_nav_ok = 1;                /* A */
            if (pressed & (0x2000 | 0x0020)) gui_nav_back = 1;   /* B or Back/Select */
        } else {
            /* Back / View / Select (0x0020) triggers original CD-i in-game pause screen */
            if (pressed & 0x0020) g_cdi_pause_req = 4;
            if (pressed & 0x0100) push_hotkey(HOTKEY_SAVE);     /* LB / L1 */
            if (pressed & 0x0200) push_hotkey(HOTKEY_LOAD);     /* RB / R1 */
            if (apply_game_input) {
                int lx = g->sThumbLX, ly = g->sThumbLY;
                const int dead = 8000;
                if (lx > dead || lx < -dead) input.dx += (lx > 0 ? lx - dead : lx + dead) * 10 / (32767 - dead);
                if (ly > dead || ly < -dead) input.dy -= (ly > 0 ? ly - dead : ly + dead) * 10 / (32767 - dead);
                if (b & 0x0001) input.dy -= 6;
                if (b & 0x0002) input.dy += 6;
                if (b & 0x0004) input.dx -= 6;
                if (b & 0x0008) input.dx += 6;
                if (b & 0x1000) input.b1 = 1;
                if (b & 0x2000) input.b2 = 1;
                if (b & 0x4000) { input.b1 = 1; input.b2 = 1; }
                if (b & 0x8000) input.b2 = 1;
            }
        }
        prev_xi_hat_x = hx;
        prev_xi_hat_y = hy;
        break;
    }
}

/* ---------------------------------------------------------------- Built-in Software GUI Renderer */
#define GUI_W 768
#define GUI_H 480
static uint32_t *gui_fb;

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

static void gui_mouse_coords(int *gx, int *gy) {
    RECT cr = {0, 0, 1280, 800};
    if (hwnd) GetClientRect(hwnd, &cr);
    int cw = cr.right > 0 ? cr.right : 1;
    int ch = cr.bottom > 0 ? cr.bottom : 1;
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

extern int  frontend_get_runahead(void);
extern void frontend_set_runahead(int v);
extern int  frontend_get_interp(void);
extern void frontend_set_interp(int v);
extern int  frontend_get_upscaler(void);
extern void frontend_set_upscaler(int v);
extern int  frontend_get_slot(void);
extern void frontend_set_slot(int v);

static int try_system_file_dialog(const char *start_dir, char *out, size_t n) {
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = "CD-i Disc Images (*.cue;*.bin;*.chd)\0*.cue;*.bin;*.chd\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file;
    ofn.lpstrInitialDir = start_dir;
    ofn.lpstrTitle = "Select " GAME_TITLE " disc image (.cue, .bin, or .chd)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameA(&ofn) || !file[0]) return -1;
    snprintf(out, n, "%s", file);
    return 0;
}

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
    const char *sl = strrchr(chd_path, '/'), *bs = strrchr(chd_path, '\\');
    if (bs > sl) sl = bs;
    const char *base_name = sl ? sl + 1 : chd_path;
    char stem[256];
    snprintf(stem, sizeof stem, "%s", base_name);
    size_t L = strlen(stem);
    if (L > 4) stem[L - 4] = 0;
    char out_prefix[1024];
    snprintf(out_prefix, sizeof out_prefix, "%s/%s", g_data_dir, stem);

    const char *py_cmd = "python";
    if (system("python --version >nul 2>&1") != 0 && system("py -3 --version >nul 2>&1") == 0)
        py_cmd = "py -3";
    char cmd[2600];
    snprintf(cmd, sizeof cmd, "%s \"%s\" \"%s\" \"%s\"", py_cmd, script, chd_path, out_prefix);
    int r = system(cmd);
    runtime_cleanup_embedded_tools(tool_dir);
    if (r != 0) return -1;
    snprintf(cue_out, n, "%s.cue", out_prefix);
    FILE *f = fopen(cue_out, "rb");
    if (!f) return -1;
    fclose(f);
    return 0;
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
    return _stricmp(fa->name, fb->name);
}

static int load_dir_entries(const char *dir, const char *exe_dir, FileEntry *list, int max_entries) {
    int n = 0;
    snprintf(list[n].name, sizeof list[n].name, "[.. PARENT DIRECTORY]");
    snprintf(list[n].full, sizeof list[n].full, "%s\\..", dir);
    list[n].is_dir = 1; n++;

    char rom_dir[600];
    snprintf(rom_dir, sizeof rom_dir, "%s\\rom", exe_dir);
    DWORD attr = GetFileAttributesA(rom_dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) && n < max_entries) {
        snprintf(list[n].name, sizeof list[n].name, "[GAME ROM FOLDER]");
        snprintf(list[n].full, sizeof list[n].full, "%s", rom_dir);
        list[n].is_dir = 1; n++;
    }
    snprintf(list[n].name, sizeof list[n].name, "[OPEN WINDOWS FILE DIALOG...]");
    snprintf(list[n].full, sizeof list[n].full, "::SYS_DIALOG::");
    list[n].is_dir = 2; n++;

    int fixed_top = n;
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.') continue;
            char p[1024];
            snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (n < max_entries) {
                    snprintf(list[n].name, sizeof list[n].name, "[DIR] %s", fd.cFileName);
                    snprintf(list[n].full, sizeof list[n].full, "%s", p);
                    list[n].is_dir = 1;
                    n++;
                }
            } else {
                size_t L = strlen(fd.cFileName);
                if (L > 4 && (!_stricmp(fd.cFileName + L - 4, ".cue") ||
                              !_stricmp(fd.cFileName + L - 4, ".bin") ||
                              !_stricmp(fd.cFileName + L - 4, ".chd"))) {
                    if (n < max_entries) {
                        snprintf(list[n].name, sizeof list[n].name, "      %s", fd.cFileName);
                        snprintf(list[n].full, sizeof list[n].full, "%s", p);
                        list[n].is_dir = 0;
                        n++;
                    }
                }
            }
        } while (FindNextFileA(h, &fd) && n < max_entries);
        FindClose(h);
    }
    if (n > fixed_top) {
        qsort(list + fixed_top, (size_t)(n - fixed_top), sizeof(FileEntry), cmp_file_entry);
    }
    return n;
}

static int run_file_browser(const char *exe_dir, char *out, size_t out_n) {
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);
    FileEntry *entries = (FileEntry *)calloc(512, sizeof(FileEntry));
    char cur_dir[MAX_PATH];
    if (!_fullpath(cur_dir, exe_dir, sizeof cur_dir)) snprintf(cur_dir, sizeof cur_dir, "%s", exe_dir);
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
        pump_win32_messages();
        gamepad_poll_events(0);

        if (gui_nav_up) { sel = (sel - 1 + count) % (count > 0 ? count : 1); gui_nav_up = 0; }
        if (gui_nav_down) { sel = (sel + 1) % (count > 0 ? count : 1); gui_nav_down = 0; }
        if (gui_nav_left) { sel = sel >= vis_rows ? sel - vis_rows : 0; gui_nav_left = 0; }
        if (gui_nav_right) { sel = sel + vis_rows < count ? sel + vis_rows : (count > 0 ? count - 1 : 0); gui_nav_right = 0; }

        int gx, gy;
        gui_mouse_coords(&gx, &gy);
        if (mouse_clicked) {
            mouse_clicked = 0;
            if (gx >= GUI_W - 130 && gx <= GUI_W - 24 && gy >= 16 && gy <= 46) {
                gui_nav_back = 1;
            }
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
                    if (L > 4 && !_stricmp(picked + L - 4, ".chd")) {
                        if (convert_chd_image(exe_dir, picked, out, out_n) == 0) { result = 0; break; }
                        snprintf(status_msg, sizeof status_msg, "CHD CONVERSION FAILED (REQUIRES PYTHON 3)");
                    } else {
                        snprintf(out, out_n, "%s", picked);
                        result = 0;
                        break;
                    }
                }
            } else if (e->is_dir == 1) {
                char resolved[MAX_PATH];
                if (_fullpath(resolved, e->full, sizeof resolved)) snprintf(cur_dir, sizeof cur_dir, "%s", resolved);
                else snprintf(cur_dir, sizeof cur_dir, "%s", e->full);
                count = load_dir_entries(cur_dir, exe_dir, entries, 512);
                sel = 0; scroll = 0;
            } else {
                size_t L = strlen(e->full);
                if (L > 4 && !_stricmp(e->full + L - 4, ".chd")) {
                    if (convert_chd_image(exe_dir, e->full, out, out_n) == 0) { result = 0; break; }
                    snprintf(status_msg, sizeof status_msg, "CHD CONVERSION FAILED (REQUIRES PYTHON 3)");
                } else {
                    snprintf(out, out_n, "%s", e->full);
                    result = 0;
                    break;
                }
            }
        }

        if (sel < scroll) scroll = sel;
        if (sel >= scroll + vis_rows) scroll = sel - vis_rows + 1;

        gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
        gui_fill_rect(0, 0, GUI_W, 56, 0xFF182438u);
        gui_draw_text(24, 14, 2, 0xFFFFE070u, "SELECT DISC IMAGE (.CUE / .BIN / .CHD)");
        gui_draw_text(24, 38, 1, 0xFF98B0D0u, cur_dir);

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
    if (hwnd) return run_file_browser(exe_dir, out, n);
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
        if (!_strnicmp(line, key, kl) && line[kl] == '=') {
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
            if (!_strnicmp(line, key, kl) && line[kl] == '=') continue;
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
    return u == CARTOON_HDPLUS ? "CARTOON HD+ (GPU XBR + INKED)" :
           u == CARTOON_HD     ? "CARTOON HD (GPU XBR)" :
                                 "ORIGINAL (384X240)";
}

int host_run_launcher(const char *exe_dir, char *img_out, size_t n, int has_img) {
    if (g_headless) return -1;
    if (!host_inited) host_init();
    if (!hwnd) return -1;
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);

    char show_lv[16] = "0";
    ini_get_str(exe_dir, "show_launcher", show_lv, sizeof show_lv);
    int show_on_start = (show_lv[0] == '1');

    enum {
        LM_PLAY,
        LM_DISC,
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
        pump_win32_messages();
        gamepad_poll_events(0);

        if (gui_nav_up) { sel = (sel - 1 + LM_COUNT) % LM_COUNT; gui_nav_up = 0; }
        if (gui_nav_down) { sel = (sel + 1) % LM_COUNT; gui_nav_down = 0; }

        int gx, gy;
        gui_mouse_coords(&gx, &gy);
        if (mouse_clicked) {
            mouse_clicked = 0;
            for (int i = 0; i < LM_COUNT; i++) {
                int ry = 80 + i * 28;
                if (gx >= 36 && gx <= GUI_W - 36 && gy >= ry && gy < ry + 25) {
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

        gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
        gui_fill_rect(0, 0, GUI_W, 56, 0xFF182438u);
        gui_draw_text(28, 10, 2, 0xFFFFE070u, GAME_TITLE " - CD-I STATIC RECOMPILATION");
        gui_draw_text(28, 34, 1, 0xFF88B0E8u, "VERSION " APP_VERSION "  |  SELF-CONTAINED PORTABLE EDITION");
        gui_draw_text(36, 64, 1, has_img ? 0xFF70F090u : 0xFFFFB060u, status_msg);

        const char *disc_base = "NOT FOUND (CLICK TO BROWSE...)";
        if (has_img && img_out[0]) {
            const char *sl = strrchr(img_out, '/'), *bs = strrchr(img_out, '\\');
            if (bs > sl) sl = bs;
            disc_base = sl ? sl + 1 : img_out;
        }
        char labels[LM_COUNT][64];
        char values[LM_COUNT][80];
        snprintf(labels[LM_PLAY], 64, ">> START GAME");
        snprintf(values[LM_PLAY], 80, "%s", has_img ? "READY" : "SELECT DISC IMAGE FIRST");
        snprintf(labels[LM_DISC], 64, "DISC IMAGE (.CUE/.BIN/.CHD)");
        snprintf(values[LM_DISC], 80, "%.52s", disc_base);
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
            int ry = 80 + i * 28;
            int is_sel = (i == sel);
            uint32_t bg = is_sel ? 0xFF284A78u : (i == LM_PLAY ? 0xFF1A3228u : 0xFF141E30u);
            gui_fill_rect(36, ry, GUI_W - 72, 25, bg);
            if (is_sel) gui_border_rect(36, ry, GUI_W - 72, 25, 1, 0xFFFFE070u);
            uint32_t lcol = is_sel ? 0xFFFFE070u : (i == LM_PLAY ? 0xFF80FFB0u : 0xFFD8E2F0u);
            gui_draw_text(50, ry + 9, 1, lcol, labels[i]);
            gui_draw_text(350, ry + 9, 1, 0xFFFFFFFFu, values[i]);
        }

        gui_fill_rect(0, GUI_H - 36, GUI_W, 36, 0xFF182438u);
        gui_draw_text(28, GUI_H - 22, 1, 0xFFB8C8E0u,
                      "QUICK MENU: [START / MENU] OR [F1]  |  CD-I PAUSE: [SELECT / VIEW] OR [P]");

        present_raw(gui_fb, GUI_W, GUI_H);
        host_sleep_until(host_time() + 1.0 / 60.0);
    }
    gui_open = 0;
    set_cursor_visible(0);
    return has_img ? 0 : -1;
}

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
        pump_win32_messages();
        gamepad_poll_events(0);

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
        snprintf(labels[OM_SAVE], 48, "QUICKSAVE TO CURRENT SLOT (F2 / LB)");
        snprintf(values[OM_SAVE], 64, "SLOT %d", slot);
        snprintf(labels[OM_LOAD], 48, "QUICKLOAD FROM CURRENT SLOT (F4 / RB)");
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
    if (g_headless || !hwnd) return;
    if (!gui_fb) gui_fb = (uint32_t *)calloc(GUI_W * GUI_H, 4);
    gui_fill_rect(0, 0, GUI_W, GUI_H, 0xFF0E1420u);
    gui_fill_rect(60, 180, GUI_W - 120, 120, 0xFF141E30u);
    gui_border_rect(60, 180, GUI_W - 120, 120, 2, 0xFF50A0FFu);
    gui_draw_text(84, 212, 2, 0xFFFFE070u, title ? title : "");
    gui_draw_text(84, 248, 1, 0xFFD0D8E8u, subtitle ? subtitle : "");
    present_raw(gui_fb, GUI_W, GUI_H);
}

/* Interactive Compiler Selection Screen when no system C compiler is installed
   on Windows.  Returns:
     1 = Download portable Zig/Clang C compiler into saves/derived/compiler/ & compile
     0 = Use built-in 68000 engine */
int host_choose_compiler(const char *exe_dir) {
    if (g_headless) return 0;
    char mode[32] = "";
    ini_get_str(exe_dir, "compiler_mode", mode, sizeof mode);
    if (!_stricmp(mode, "interp") || !_stricmp(mode, "builtin")) return 0;
    if (!_stricmp(mode, "download") || !_stricmp(mode, "zig")) return 1;

    if (!host_inited) host_init();
    if (!hwnd) return 0;
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
        pump_win32_messages();
        gamepad_poll_events(0);

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
                      "  AND COMPILE NATIVE X64 CODE (RECOMP_USER.DLL) ON THE FLY (~45 MB, ONE-TIME).");
        gui_draw_text(64, 174, 1, 0xFFB8C8E0u,
                      "* OPTION 2/3: RUN IMMEDIATELY WITH THE BUILT-IN 68000 ENGINE (NO DOWNLOAD NEEDED).");

        const char *titles[CM_COUNT] = {
            ">> DOWNLOAD PORTABLE C COMPILER & BUILD NATIVE CODE (RECOMMENDED)",
            ">> USE BUILT-IN 68000 ENGINE (PLAY IMMEDIATELY, ASK NEXT TIME)",
            ">> ALWAYS USE BUILT-IN 68000 ENGINE (PLAY IMMEDIATELY, DON'T ASK AGAIN)"
        };
        const char *subs[CM_COUNT] = {
            "DOWNLOADS PORTABLE ZIG CC TO SAVES/DERIVED/COMPILER/ & BUILDS RECOMP_USER.DLL",
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

/* ---------------------------------------------------------------- Frame Dump & Presentation */
int g_turbo;
int g_fuzz;
static double next_frame_time = -1;
static unsigned long long frame_counter;
int g_dump_frames;
int g_max_frames;
int g_callprof_from;
unsigned long long g_dump_from;
unsigned g_ramdump_at[16]; int g_nramdump;

static void dump_bmp(const uint32_t *fb, int w, int h, unsigned long long n) {
    char name[600];
    snprintf(name, sizeof name, "%s/frame_%06llu.bmp", g_data_dir, n);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    BITMAPFILEHEADER bf = {0};
    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
    bf.bfType = 0x4D42; bf.bfOffBits = sizeof bf + sizeof bi; bf.bfSize = bf.bfOffBits + w * h * 4;
    fwrite(&bf, sizeof bf, 1, f); fwrite(&bi, sizeof bi, 1, f);
    fwrite(fb, 4, (size_t)w * h, f);
    fclose(f);
}

void host_present(const uint32_t *fb, int w, int h) {
    frame_counter++;
    if (g_dump_frames && frame_counter >= g_dump_from && frame_counter % (unsigned)g_dump_frames == 0)
        dump_bmp(fb, w, h, frame_counter);
    {
        extern unsigned g_ramdump_at[16]; extern int g_nramdump; extern uint8_t *RAM;
        for (int i = 0; i < g_nramdump; i++) if (frame_counter == g_ramdump_at[i]) {
            char rp[600]; snprintf(rp, sizeof rp, "%s/ram_%06u.bin", g_data_dir, g_ramdump_at[i]);
            FILE *rf = fopen(rp, "wb"); if (rf) { fwrite(RAM, 1, 0x01000000, rf); fclose(rf); }
        }
    }
    { extern int g_callprof, g_callprof_from; if (g_callprof_from && frame_counter >= (unsigned long long)g_callprof_from) g_callprof = 1; }
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

    static int consec_skips = 0;
    double t_before = host_time();
    if (next_frame_time < 0) next_frame_time = t_before;
    next_frame_time += 1.0 / FIELD_RATE;

    /* Adaptive frame-skip if an external frame limiter (30/40/45 FPS cap)
       blocked Present() past this field's deadline, keeping 60 Hz emulation
       and 44.1 kHz audio at 100% speed. */
    int skip_present = (t_before > next_frame_time && consec_skips < 3);
    if (skip_present) {
        consec_skips++;
    } else {
        consec_skips = 0;
        present_raw(fb, w, h);
    }
    double now = host_time();
    if (now < next_frame_time) {
        host_sleep_until(next_frame_time);
    } else if (now - next_frame_time > 2.0 / FIELD_RATE) {
        next_frame_time = now - 1.25 / FIELD_RATE;
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
        pump_win32_messages();
    }
    int speed = keys[VK_SHIFT] ? 12 : 6;
    input.dx = (keys[VK_RIGHT] ? speed : 0) - (keys[VK_LEFT] ? speed : 0);
    input.dy = (keys[VK_DOWN] ? speed : 0) - (keys[VK_UP] ? speed : 0);
    input.b1 = keys['Z'] || keys[VK_SPACE] || keys['A'] || keys['C'] || keys[VK_RETURN];
    input.b2 = keys['X'] || keys[VK_CONTROL] || keys['S'] || keys['C'] || keys[VK_RETURN];
    input.b3 = 0;
    if (!g_headless) gamepad_poll_events(1);
    g_turbo = keys[VK_TAB] || xi_rt_held;
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

/* ---------------------------------------------------------------- audio */
#define NBUF 12
#define BUF_FRAMES 1470
#define WIN_PREFILL_BUFS 3
static HWAVEOUT wo;
static WAVEHDR hdrs[NBUF];
static int16_t bufs[NBUF][BUF_FRAMES * 2];
static int cur_buf, cur_fill, wo_paused;

void host_audio_init(int rate) {
    if (g_headless) return;
    WAVEFORMATEX wf = {0};
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = rate * 4;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        LOG("waveOutOpen failed; audio disabled");
        wo = NULL;
        return;
    }
    for (int i = 0; i < NBUF; i++) {
        hdrs[i].lpData = (LPSTR)bufs[i];
        hdrs[i].dwBufferLength = BUF_FRAMES * 4;
        hdrs[i].dwFlags = WHDR_DONE;
    }
    /* Pause waveOut until WIN_PREFILL_BUFS (~100 ms) are queued so the
       playback stream starts with a full cushion and never underruns on
       minor frame-time jitter or 59.94 Hz vsync. */
    waveOutPause(wo);
    wo_paused = 1;
}

int host_audio_queued(void) {
    int n = 0;
    for (int i = 0; i < NBUF; i++) if (!(hdrs[i].dwFlags & WHDR_DONE)) n++;
    return n;
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
    if (!wo) return;
    while (frames > 0) {
        WAVEHDR *h = &hdrs[cur_buf];
        if (!(h->dwFlags & WHDR_DONE)) return;   /* all buffers busy: drop */
        int k = BUF_FRAMES - cur_fill;
        if (k > frames) k = frames;
        memcpy(bufs[cur_buf] + cur_fill * 2, s, (size_t)k * 4);
        cur_fill += k; s += k * 2; frames -= k;
        if (cur_fill == BUF_FRAMES) {
            if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(wo, h, sizeof *h);
            h->dwFlags = 0;
            h->dwBufferLength = BUF_FRAMES * 4;
            waveOutPrepareHeader(wo, h, sizeof *h);
            waveOutWrite(wo, h, sizeof *h);
            cur_buf = (cur_buf + 1) % NBUF;
            cur_fill = 0;
            if (wo_paused && host_audio_queued() >= WIN_PREFILL_BUFS) {
                waveOutRestart(wo);
                wo_paused = 0;
            }
        }
    }
    /* If all buffers drained (e.g. after a long pause/menu), re-arm the
       prefill cushion before restarting playback. */
    if (!wo_paused && host_audio_queued() == 0) {
        waveOutPause(wo);
        wo_paused = 1;
    }
}
