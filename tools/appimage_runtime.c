/* Type-2 AppImage ELF runtime stub for Linux & SteamOS (Steam Deck).
   Embeds the "AI\x02" AppImage magic in the ELF header (offset 8) and has a
   fixed 65536-byte offset where mksquashfs appends the SquashFS image.

   Supports:
     --appimage-offset             print byte offset of the SquashFS archive
     --appimage-extract [pattern]  extract to ./squashfs-root
     --appimage-extract-and-run    extract to a temporary directory and run
     --appimage-mount              mount/extract and print path until Ctrl+C
     --appimage-version / --appimage-help

   On normal execution, mounts via squashfuse if available and FUSE works, or
   transparently extracts to /tmp/.mount_ApprenXXXXXX via unsquashfs so it runs
   everywhere (SteamOS, Desktop Linux, WSL, containers) even without libfuse2. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#ifndef APPIMAGE_OFFSET
#define APPIMAGE_OFFSET 65536
#endif

static char g_mount_dir[PATH_MAX];
static int g_is_fuse;

static void run_cmd_ignore(const char *cmd) {
    int r = system(cmd);
    (void)r;
}

static void cleanup_mount(void) {
    if (!g_mount_dir[0]) return;
    if (g_is_fuse) {
        char cmd[PATH_MAX * 2 + 128];
        snprintf(cmd, sizeof cmd, "fusermount -u -z '%s' >/dev/null 2>&1 || umount -l '%s' >/dev/null 2>&1",
                 g_mount_dir, g_mount_dir);
        run_cmd_ignore(cmd);
        rmdir(g_mount_dir);
    } else {
        char cmd[PATH_MAX + 64];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_mount_dir);
        run_cmd_ignore(cmd);
    }
    g_mount_dir[0] = 0;
}

static void on_sig(int sig) {
    cleanup_mount();
    signal(sig, SIG_DFL);
    raise(sig);
}

static int has_prog(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = strtok(buf, ":"); p; p = strtok(NULL, ":")) {
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", p, name);
        if (access(full, X_OK) == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0) {
        perror("readlink /proc/self/exe");
        return 1;
    }
    self[n] = 0;

    setenv("APPIMAGE", self, 1);
    setenv("ARGV0", argv[0], 1);

    /* Inside Steam's bwrap container (PRESSURE_VESSEL_RUNTIME / SteamEnv /
       STEAM_GAMEPADUI), FUSE mounting inside a nested user namespace can
       fail or stall; default to direct extraction in /tmp when running
       under Steam or Gamescope. */
    int force_extract_run = (getenv("APPIMAGE_EXTRACT_AND_RUN") != NULL ||
                             getenv("PRESSURE_VESSEL_RUNTIME") != NULL ||
                             getenv("STEAM_GAMEPADUI") != NULL ||
                             getenv("SteamEnv") != NULL);
    int arg_start = 1;

    if (argc > 1) {
        if (!strcmp(argv[1], "--appimage-offset")) {
            printf("%d\n", APPIMAGE_OFFSET);
            return 0;
        }
        if (!strcmp(argv[1], "--appimage-version")) {
            printf("AppImage Type 2 runtime (built-in FUSE + unsquashfs fallback)\n");
            return 0;
        }
        if (!strcmp(argv[1], "--appimage-help")) {
            printf("AppImage options:\n"
                   "  --appimage-offset             Print byte offset of embedded SquashFS image\n"
                   "  --appimage-extract [pattern]  Extract content to squashfs-root/\n"
                   "  --appimage-extract-and-run    Extract to /tmp and run without FUSE\n"
                   "  --appimage-mount              Mount/prepare AppDir and print path\n"
                   "  --appimage-version            Print AppImage runtime version\n");
            return 0;
        }
        if (!strcmp(argv[1], "--appimage-extract")) {
            char cmd[PATH_MAX * 2];
            run_cmd_ignore("rm -rf squashfs-root");
            if (argc > 2)
                snprintf(cmd, sizeof cmd, "unsquashfs -q -o %d -d squashfs-root '%s' '%s'",
                         APPIMAGE_OFFSET, self, argv[2]);
            else
                snprintf(cmd, sizeof cmd, "unsquashfs -q -o %d -d squashfs-root '%s'",
                         APPIMAGE_OFFSET, self);
            return system(cmd) == 0 ? 0 : 1;
        }
        if (!strcmp(argv[1], "--appimage-extract-and-run")) {
            force_extract_run = 1;
            arg_start = 2;
        }
    }

    snprintf(g_mount_dir, sizeof g_mount_dir, "/tmp/.mount_ApprenXXXXXX");
    if (!mkdtemp(g_mount_dir)) {
        perror("mkdtemp");
        return 1;
    }

    int mounted = 0;
    if (!force_extract_run && has_prog("squashfuse") && access("/dev/fuse", R_OK | W_OK) == 0) {
        char cmd[PATH_MAX * 2 + 128];
        snprintf(cmd, sizeof cmd, "squashfuse -o offset=%d '%s' '%s' >/dev/null 2>&1",
                 APPIMAGE_OFFSET, self, g_mount_dir);
        if (system(cmd) == 0) {
            char apprun[PATH_MAX + 32];
            snprintf(apprun, sizeof apprun, "%s/AppRun", g_mount_dir);
            if (access(apprun, X_OK) == 0) {
                mounted = 1;
                g_is_fuse = 1;
            }
        }
    }

    if (!mounted) {
        rmdir(g_mount_dir);
        char cmd[PATH_MAX * 2 + 128];
        snprintf(cmd, sizeof cmd, "unsquashfs -q -f -o %d -d '%s' '%s' >/dev/null 2>&1",
                 APPIMAGE_OFFSET, g_mount_dir, self);
        if (system(cmd) != 0) {
            fprintf(stderr, "AppImage error: could not mount (squashfuse) or unpack (unsquashfs) '%s'\n", self);
            cleanup_mount();
            return 1;
        }
        g_is_fuse = 0;
    }

    if (argc > 1 && !strcmp(argv[1], "--appimage-mount")) {
        printf("%s\n", g_mount_dir);
        fflush(stdout);
        signal(SIGINT, on_sig);
        signal(SIGTERM, on_sig);
        pause();
        cleanup_mount();
        return 0;
    }

    setenv("APPDIR", g_mount_dir, 1);
    {
        char marker[PATH_MAX + 32];
        snprintf(marker, sizeof marker, "%s/.appimage_path", g_mount_dir);
        FILE *mf = fopen(marker, "w");
        if (mf) {
            fprintf(mf, "%s\n", self);
            fclose(mf);
        }
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGHUP, on_sig);

    char apprun[PATH_MAX + 32];
    snprintf(apprun, sizeof apprun, "%s/AppRun", g_mount_dir);

    pid_t pid = fork();
    if (pid == 0) {
        /* Always chdir to the directory containing the .AppImage file itself
           so CWD is the AppImage's folder (never /home/deck/.local/share/Steam
           or /home/deck when launched from Steam or Dolphin). */
        char self_dir[PATH_MAX];
        snprintf(self_dir, sizeof self_dir, "%s", self);
        char *sl = strrchr(self_dir, '/');
        if (sl) {
            *sl = 0;
            if (chdir(self_dir) == 0) {}
        }
        int new_argc = argc - arg_start + 1;
        char **new_argv = (char **)calloc((size_t)new_argc + 1, sizeof(char *));
        new_argv[0] = apprun;
        for (int i = arg_start; i < argc; i++) new_argv[i - arg_start + 1] = argv[i];
        new_argv[new_argc] = NULL;
        execv(apprun, new_argv);
        perror("execv AppRun");
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    cleanup_mount();
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}
