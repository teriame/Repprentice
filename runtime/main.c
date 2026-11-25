/* Entry point: load the disc, set up the HLE OS and run the recompiled game. */
#include "recomp.h"
#include "runtime.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "find_image.inc"
#include "game_config.h"

int log_level = 1;
static FILE *logfp;
extern int g_headless, g_dump_frames, g_max_frames;

void logf_(int lvl, const char *fmt, ...) {
    (void)lvl;
    if (g_log_muted && lvl > 0) return;     /* fields emulated ahead are not logged */
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    if (logfp) { fprintf(logfp, "%s\n", buf); fflush(logfp); }
}

void fatal(const char *fmt, ...) {
    va_list ap;
    char buf[1024];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf_(0, "FATAL: %s", buf);
    logf_(0, "  pc=%06x d=%08x %08x %08x %08x %08x %08x %08x %08x", m68k.pc,
          D(0), D(1), D(2), D(3), D(4), D(5), D(6), D(7));
    logf_(0, "  a=%08x %08x %08x %08x %08x %08x %08x %08x",
          A(0), A(1), A(2), A(3), A(4), A(5), A(6), A(7));
    if (!RAM) goto out;
    /* crude 68k stack backtrace: print code addresses found on the stack */
    for (int i = 0; i < 64; i++) {
        uint32_t v = RL(A(7) + i * 4);
        if (v >= recomp_code_lo && v < recomp_code_hi)
            logf_(0, "  stack+%03x: %06x (%s)", i * 4, v, func_name(v));
    }
    {
        char rp[600];
        snprintf(rp, sizeof rp, "%s/ramdump.bin", g_data_dir);
        FILE *rf = fopen(rp, "wb");
        if (rf) { fwrite(RAM, 1, MEM_SIZE, rf); fclose(rf); }
    }
out:
#ifdef _WIN32
    if (!g_headless) {
        MessageBoxA(NULL, buf, "Recomp fatal error", MB_ICONERROR);
    }
#else
    if (!g_headless) {
        extern void host_fatal_box(const char *msg);
        host_fatal_box(buf);
    }
#endif
    exit(1);
}

/* hooked instructions (game.json "trace_hooks"): logged with --trace */
int game_skip_insn(uint32_t addr) {
    TRACE("hook %06x (%s) d0=%08x d1=%08x a0=%06x a6=%06x", addr, func_name(addr), D(0), D(1), A(0), A(6));
    return 0;
}

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

int frontend_option(const char *opt, const char *val);

int main(int argc, char **argv) {
    const char *img_arg = NULL;
    unsigned pokes[16][2];
    int npokes = 0, no_update = 0, force_gui = 0, no_gui = 0, add_to_steam = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--no-update")) no_update = 1;
        else if (!strcmp(argv[i], "--gui") || !strcmp(argv[i], "--launcher")) force_gui = 1;
        else if (!strcmp(argv[i], "--no-gui") || !strcmp(argv[i], "--no-launcher") || !strcmp(argv[i], "--play")) no_gui = 1;
        else if (!strcmp(argv[i], "--add-to-steam") || !strcmp(argv[i], "--register-steam")) add_to_steam = 1;
        else if (!strcmp(argv[i], "--portable")) { /* always portable: accepted for shortcuts */ }
        else if (!strcmp(argv[i], "--trace")) log_level = 2;
        else if (!strcmp(argv[i], "--trace3")) log_level = 3;
        else if (!strcmp(argv[i], "--dump") && i + 1 < argc) g_dump_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_max_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--aspect") || !strcmp(argv[i], "--filter") || !strcmp(argv[i], "--size")) {
            extern void host_video_option(const char *, const char *);
            if (i + 1 < argc) { host_video_option(argv[i], argv[i + 1]); i++; }
        }
        else if (!strcmp(argv[i], "--fullscreen") || !strcmp(argv[i], "--windowed") || !strcmp(argv[i], "--gdi")) {
            extern void host_video_option(const char *, const char *);
            host_video_option(argv[i], NULL);
        }
        else if (frontend_option(argv[i], i + 1 < argc ? argv[i + 1] : NULL)) {
            if (frontend_option(argv[i], i + 1 < argc ? argv[i + 1] : NULL) == 2) i++;
        }
        else if (!strcmp(argv[i], "--interp")) { extern int g_interp_all; g_interp_all = 1; }
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < argc) { extern int g_fuzz; g_fuzz = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) { extern void host_wav_open(const char *); host_wav_open(argv[++i]); }
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) { extern void host_set_script(const char *); host_set_script(argv[++i]); }
        else if (!strcmp(argv[i], "--dump-from") && i + 1 < argc) { extern unsigned long long g_dump_from; g_dump_from = strtoull(argv[++i], NULL, 10); }
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc) { extern uint32_t g_watch; g_watch = (uint32_t)strtoul(argv[++i], NULL, 16); }
        else if (!strcmp(argv[i], "--ramdump-at") && i + 1 < argc) {
            extern unsigned g_ramdump_at[16]; extern int g_nramdump;
            for (char *q = argv[++i]; *q && g_nramdump < 16;) { g_ramdump_at[g_nramdump++] = (unsigned)strtoul(q, &q, 10); if (*q == ',') q++; }
        }
        else if (!strcmp(argv[i], "--callprof") && i + 1 < argc) { extern int g_callprof_from; g_callprof_from = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--poke") && i + 1 < argc) {
            /* debug: ADDR:BYTE written after the main program is loaded (several allowed) */
            unsigned a, v;
            if (sscanf(argv[++i], "%x:%x", &a, &v) == 2 && npokes < 16) { pokes[npokes][0] = a; pokes[npokes][1] = v; npokes++; }
        }
        else img_arg = argv[i];
    }
    init_exe_dir();
    setup_data_dir();
    {
        char lp[600];
        snprintf(lp, sizeof lp, "%s/recomp.log", g_data_dir);
        logfp = fopen(lp, "w");
        if (!logfp) {
            /* Fallback if CWD is read-only: use /tmp (Linux) or %TEMP% (Windows) */
#ifdef _WIN32
            char tmp[MAX_PATH] = ".";
            GetTempPathA(sizeof tmp, tmp);
            snprintf(g_data_dir, sizeof g_data_dir, "%sapprentice_saves", tmp);
            _mkdir(g_data_dir);
#else
            snprintf(g_data_dir, sizeof g_data_dir, "/tmp/apprentice_saves");
            mkdir(g_data_dir, 0755);
#endif
            snprintf(lp, sizeof lp, "%s/recomp.log", g_data_dir);
            logfp = fopen(lp, "w");
        }
    }
    LOG("%s %s (exe_dir=%s, saves=%s)", GAME_TITLE, APP_VERSION, exe_dir, g_data_dir);
#ifndef _WIN32
    if (add_to_steam) {
        extern int host_register_in_steam(const char *exe_dir, int check_only, int auto_only, char *status_out, size_t status_n);
        char st_msg[256] = "";
        int rc = host_register_in_steam(exe_dir, 0, 0, st_msg, sizeof st_msg);
        if (st_msg[0]) printf("%s\n", st_msg);
        return rc == 0 ? 0 : 1;
    }
#else
    (void)add_to_steam;
#endif
    if (!g_headless && !no_update) {
        /* <exe_name>.ini: update_check=1 ask (default), auto, 0 off */
        char v[16] = "1";
        ini_get("update_check", v, sizeof v);
        if (v[0] != '0') {
            extern void update_check_start(const char *exe_dir, int automatic);
            update_check_start(exe_dir, !_stricmp(v, "auto"));
        }
    }
    char img[1024] = "";
    int found = (find_image(img_arg, img, sizeof img) == 0);
    if (!g_headless && !no_gui) {
        /* Open the interactive GUI launcher on both Windows and Linux when
           requested (--gui / --launcher), when no disc image is found yet, or
           when show_launcher=1 is set in apprentice.ini. */
        char lv[16] = "0";
        ini_get("show_launcher", lv, sizeof lv);
        if (force_gui || !found || lv[0] == '1') {
            extern int host_run_launcher(const char *exe_dir, char *img_out, size_t n, int has_img);
            if (host_run_launcher(exe_dir, img, sizeof img, found) == 0 && img[0]) {
                size_t L = strlen(img);
                char bin[1024];
                if (L > 4 && !_stricmp(img + L - 4, ".cue")) cue_to_bin(img, bin, sizeof bin);
                else snprintf(bin, sizeof bin, "%s", img);
                if (is_cdi_image(bin)) {
                    if (!img_arg) ini_set("rom", img);
                    snprintf(img, sizeof img, "%s", bin);
                    found = 1;
                }
            }
        }
    }
    if (!found && (g_headless || img_arg || pick_image(img, sizeof img))) {
        fatal("Could not find the " GAME_TITLE " disc image.\n\n"
              "Put the .bin/.cue in a 'rom' folder next to the program, or pass the path to the .cue or .bin as an argument.\n"
              "(A .chd image can be selected directly in the launcher GUI or converted to .bin/.cue in saves/).");
    }
    LOG("disc image: %s", img);
#ifndef _WIN32
    /* When Steam launches an AppImage with APPIMAGE_EXTRACT_AND_RUN=1, Steam's
       own extractor may unpack to /tmp/appimage_extracted_* and start AppRun
       with CWD=/tmp/appimage_extracted_* and without APPIMAGE set.  In that
       case, relocate exe_dir and saves/ next to the disc image (and chdir
       there) so saves/ and recomp.log are persisted in the game folder instead
       of vanishing when /tmp/appimage_extracted_* is cleaned up. */
    if (!strncmp(g_data_dir, "/tmp/", 5) || !strcmp(exe_dir, ".") || access(g_data_dir, W_OK) != 0) {
        char ddir[600];
        snprintf(ddir, sizeof ddir, "%s", img);
        char *sl = strrchr(ddir, '/');
        if (sl) {
            *sl = 0;
            char *last = strrchr(ddir, '/');
            if (last && (!strcasecmp(last + 1, "rom") || !strcasecmp(last + 1, "saves"))) *last = 0;
            if (strncmp(ddir, "/tmp/", 5) != 0 && access(ddir, W_OK) == 0) {
                snprintf(exe_dir, sizeof exe_dir, "%s", ddir);
                if (chdir(exe_dir) == 0) {}
                setup_data_dir();
                ini_set("rom", img);
                char lp[600];
                snprintf(lp, sizeof lp, "%s/recomp.log", g_data_dir);
                FILE *nlog = fopen(lp, "w");
                if (nlog) {
                    if (logfp) fclose(logfp);
                    logfp = nlog;
                    LOG("%s %s (exe_dir=%s, saves=%s)", GAME_TITLE, APP_VERSION, exe_dir, g_data_dir);
                    LOG("disc image: %s", img);
                }
            }
        }
    }
#endif
    if (disc_open(img)) fatal("failed to open disc image %s", img);

    RAM = (uint8_t *)calloc(MEM_SIZE + 16, 1);
    host_init();
    cpu_derive_user_code(exe_dir, img);
    cpu_init();
    os9_init();
    video_init();
    audio_init();
    cdfm_init();
    sched_init();

    const DiscFile *df = disc_find(MAIN_MODULE);
    if (!df) fatal("program module '%s' not found on the disc (is this the right game?)", MAIN_MODULE);
    uint8_t *mod; uint32_t len;
    disc_read_file(df, &mod, &len);
    os9_load_main_module(mod, len);
    free(mod);
    for (int i = 0; i < npokes; i++) {
        LOG("poke %06x = %02x", pokes[i][0], pokes[i][1]);
        RAM[pokes[i][0] & MEM_MASK] = (uint8_t)pokes[i][1];
    }
    sched_init();

    cpu_run();
    return 0;
}
