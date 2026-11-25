#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <commdlg.h>
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#endif

static char exe_dir[512] = ".";

static void init_exe_dir(void) {
#ifdef _WIN32
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, p, sizeof p);
    if (n > 0 && n < sizeof p) {
        char *s = strrchr(p, '\\');
        if (s) {
            *s = 0;
            snprintf(exe_dir, sizeof exe_dir, "%s", p);
            _chdir(exe_dir);
            return;
        }
    }
    if (_getcwd(p, sizeof p)) snprintf(exe_dir, sizeof exe_dir, "%s", p);
#else
    char p[PATH_MAX];
    /* 1. APPIMAGE env var holds the real path to the .AppImage on disk */
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0]) {
        if (!realpath(appimg, p)) snprintf(p, sizeof p, "%s", appimg);
        char *s = strrchr(p, '/');
        if (s && strncmp(p, "/tmp/", 5) != 0) {
            *s = 0;
            snprintf(exe_dir, sizeof exe_dir, "%s", p);
            if (chdir(exe_dir) == 0) {}
            return;
        }
    }
    /* 2. $APPDIR/.appimage_path marker written by our AppImage runtime stub */
    const char *appdir = getenv("APPDIR");
    if (appdir && appdir[0]) {
        char marker[PATH_MAX];
        snprintf(marker, sizeof marker, "%s/.appimage_path", appdir);
        FILE *mf = fopen(marker, "r");
        if (mf) {
            char line[PATH_MAX] = "";
            if (fgets(line, sizeof line, mf)) {
                line[strcspn(line, "\r\n")] = 0;
                if (!realpath(line, p)) snprintf(p, sizeof p, "%s", line);
                char *s = strrchr(p, '/');
                if (s && strncmp(p, "/tmp/", 5) != 0) {
                    *s = 0;
                    fclose(mf);
                    snprintf(exe_dir, sizeof exe_dir, "%s", p);
                    if (chdir(exe_dir) == 0) {}
                    return;
                }
            }
            fclose(mf);
        }
    }
    /* 3. /proc/self/exe (when running a native binary outside /tmp) */
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n > 0) {
        p[n] = 0;
        if (strncmp(p, "/tmp/", 5) != 0) {
            char *s = strrchr(p, '/');
            if (s) {
                *s = 0;
                snprintf(exe_dir, sizeof exe_dir, "%s", p);
                if (chdir(exe_dir) == 0) {}
                return;
            }
        }
    }
    /* 4. Current working directory fallback (never allow Steam's internal folder) */
    if (getcwd(p, sizeof p) && strncmp(p, "/tmp/", 5) != 0 &&
        !strstr(p, "/.local/share/Steam") && access(p, W_OK) == 0) {
        snprintf(exe_dir, sizeof exe_dir, "%s", p);
        return;
    }
#endif
}

/* <exe_name>.ini next to the executable: settings the installer writes and the
   user may edit (rom = the disc image or its folder, update_check = 0/1).
   Like everything else the program writes, it stays in the program's folder. */
static void ini_path(char *out, size_t n) { snprintf(out, n, "%s/%s.ini", exe_dir, EXE_NAME); }

static int ini_get(const char *key, char *out, size_t n) {
    char p[600], line[1024];
    ini_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t kl = strlen(key);
    int found = -1;
    while (fgets(line, sizeof line, f)) {
        if (_strnicmp(line, key, kl) || line[kl] != '=') continue;
        char *v = line + kl + 1;
        v[strcspn(v, "\r\n")] = 0;
        snprintf(out, n, "%s", v);
        found = 0;
    }
    fclose(f);
    return found;
}

/* replace or add key=value, keeping the other lines */
static void ini_set(const char *key, const char *value) {
    char p[600], line[1024], buf[16384] = "";
    ini_path(p, sizeof p);
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

/* saves (NVRAM files), the log and debug dumps live in <exe dir>/saves */
static void setup_data_dir(void) {
    snprintf(g_data_dir, sizeof g_data_dir, "%s/saves", exe_dir);
#ifdef _WIN32
    _mkdir(g_data_dir);
#else
    mkdir(g_data_dir, 0755);
#endif
}

/* Resolve a .cue to the .bin it references. */
static void cue_to_bin(const char *cue, char *out, size_t n) {
    snprintf(out, n, "%s", cue);
    FILE *f = fopen(cue, "r");
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *q1 = strchr(line, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!strncmp(line, "FILE", 4) && q1 && q2) {
            *q2 = 0;
            char dir[512];
            snprintf(dir, sizeof dir, "%s", cue);
            char *sl = strrchr(dir, '/'), *bs = strrchr(dir, '\\');
            if (bs > sl) sl = bs;
            if (sl) { sl[1] = 0; snprintf(out, n, "%s%s", dir, q1 + 1); }
            else snprintf(out, n, "%s", q1 + 1);
            break;
        }
    }
    if (f) fclose(f);
}

/* Is this a raw 2352-byte-sector CD-i image? (disc label "CD-I " at sector 16) */
static int is_cdi_image(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8] = {0};
    int ok = fseek(f, 16L * 2352 + 24 + 1, SEEK_SET) == 0 && fread(buf, 1, 5, f) == 5 && !memcmp(buf, "CD-I ", 5);
    fclose(f);
    return ok;
}

static int scan_dir_for_image(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[600];
    snprintf(pat, sizeof pat, "%s\\*.bin", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        char p[800];
        snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
        if (is_cdi_image(p)) { FindClose(h); snprintf(out, n, "%s", p); return 0; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t L = strlen(de->d_name);
        if (L > 4 && !_stricmp(de->d_name + L - 4, ".bin")) {
            char p[800];
            snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
            if (is_cdi_image(p)) { closedir(d); snprintf(out, n, "%s", p); return 0; }
        }
    }
    closedir(d);
#endif
    return -1;
}

static int find_image(const char *arg, char *out, size_t n) {
    if (arg) {
        size_t L = strlen(arg);
        if (L > 4 && !_stricmp(arg + L - 4, ".cue")) cue_to_bin(arg, out, n);
        else snprintf(out, n, "%s", arg);
        return is_cdi_image(out) ? 0 : -1;
    }
    /* the location given at installation or picked at first start */
    char cfg[600];
    if (!ini_get("rom", cfg, sizeof cfg) && cfg[0]) {
        size_t L = strlen(cfg);
        if (L > 4 && !_stricmp(cfg + L - 4, ".cue")) { cue_to_bin(cfg, out, n); if (is_cdi_image(out)) return 0; }
        else if (L > 4 && !_stricmp(cfg + L - 4, ".bin")) { snprintf(out, n, "%s", cfg); if (is_cdi_image(out)) return 0; }
        else if (!scan_dir_for_image(cfg, out, n)) return 0;
    }
    /* look for saves/*.bin, rom/*.bin, or *.bin strictly inside exe_dir */
    if (!scan_dir_for_image(g_data_dir, out, n)) return 0;
    char romdir[700], savedir[700];
    snprintf(romdir, sizeof romdir, "%s/rom", exe_dir);
    if (!scan_dir_for_image(romdir, out, n)) return 0;
    snprintf(savedir, sizeof savedir, "%s/saves", exe_dir);
    if (!scan_dir_for_image(savedir, out, n)) return 0;
    if (!scan_dir_for_image(exe_dir, out, n)) return 0;
    return -1;
}


/* no disc found: ask where it is, and remember it in the .ini */
static int pick_image(char *out, size_t n) {
    extern int host_pick_image(const char *exe_dir, char *out, size_t n);
    char picked[1024] = "";
    while (host_pick_image(exe_dir, picked, sizeof picked) == 0 && picked[0]) {
        size_t L = strlen(picked);
        if (L > 4 && !_stricmp(picked + L - 4, ".cue")) cue_to_bin(picked, out, n);
        else snprintf(out, n, "%s", picked);
        if (is_cdi_image(out)) { ini_set("rom", picked); return 0; }
    }
    return -1;
}
