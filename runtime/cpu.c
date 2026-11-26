/* CPU support: dispatcher, ALU helpers that are too large to inline, and
   user-side 68000 -> C code derivation / dynamic loading when the binary was
   built without pre-derived game code (--stub). */
#include "recomp.h"
#include "runtime.h"
#include "embedded_tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#include <dlfcn.h>
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

M68K m68k;
uint8_t *RAM;
uint32_t jt_expected;

static RecompFn *dispatch;      /* indexed by (pc - recomp_code_lo) / 2 */
static uint32_t dispatch_n;
static const RecompEntry *active_entries;
static int active_num_entries;
static const RecompName *active_names;
static int active_num_names;

/* Extract the embedded Python tools (tools/*.py, runtime/recomp.h, game.json)
   into a temporary directory inside /tmp (Linux) or %TEMP% (Windows) so the
   single .exe or .AppImage is 100% self-contained and never writes outside
   CWD or tmp. */
int runtime_extract_embedded_tools(char *out_dir, size_t n) {
#ifdef _WIN32
    char tmp[MAX_PATH] = ".";
    DWORD tl = GetTempPathA(sizeof tmp, tmp);
    if (tl == 0 || tl >= sizeof tmp) snprintf(tmp, sizeof tmp, ".");
    snprintf(out_dir, n, "%sapprentice_tools_%lu", tmp, (unsigned long)GetCurrentProcessId());
#else
    snprintf(out_dir, n, "/tmp/apprentice_tools_%u", (unsigned)getpid());
#endif
    char sub_tools[700], sub_rt[700];
    snprintf(sub_tools, sizeof sub_tools, "%s/tools", out_dir);
    snprintf(sub_rt, sizeof sub_rt, "%s/runtime", out_dir);
    MKDIR(out_dir);
    MKDIR(sub_tools);
    MKDIR(sub_rt);

    for (int i = 0; embedded_tool_files[i].rel_path != NULL; i++) {
        char fp[800];
        snprintf(fp, sizeof fp, "%s/%s", out_dir, embedded_tool_files[i].rel_path);
        FILE *f = fopen(fp, "wb");
        if (!f) return -1;
        size_t sz = embedded_tool_files[i].size;
        if (sz > 0 && fwrite(embedded_tool_files[i].data, 1, sz, f) != sz) {
            fclose(f);
            return -1;
        }
        fclose(f);
    }
    return 0;
}

void runtime_cleanup_embedded_tools(const char *tool_dir) {
    if (!tool_dir || !tool_dir[0]) return;
    for (int i = 0; embedded_tool_files[i].rel_path != NULL; i++) {
        char fp[800];
        snprintf(fp, sizeof fp, "%s/%s", tool_dir, embedded_tool_files[i].rel_path);
        remove(fp);
    }
    char sub[700];
    snprintf(sub, sizeof sub, "%s/tools", tool_dir);
#ifdef _WIN32
    _rmdir(sub);
    snprintf(sub, sizeof sub, "%s/runtime", tool_dir);
    _rmdir(sub);
    _rmdir(tool_dir);
#else
    rmdir(sub);
    snprintf(sub, sizeof sub, "%s/runtime", tool_dir);
    rmdir(sub);
    rmdir(tool_dir);
#endif
}

/* Try to locate or download the portable Zig C compiler in <saves>/derived/compiler/
   using tools/fetch_compiler.py.  When check_only == 1, only checks if it was
   already downloaded. */
static int find_or_fetch_portable_zig(const char *tool_dir, const char *comp_dir,
                                      int check_only, char *zig_out, size_t n) {
    char script[750], cmd[2000];
    snprintf(script, sizeof script, "%s/tools/fetch_compiler.py", tool_dir);
#ifdef _WIN32
    const char *py_cmd = "python";
    if (system("python --version >nul 2>&1") != 0 && system("py -3 --version >nul 2>&1") == 0)
        py_cmd = "py -3";
    snprintf(cmd, sizeof cmd, "%s \"%s\"%s \"%s\" 2>nul",
             py_cmd, script, check_only ? " --check" : "", comp_dir);
    FILE *p = _popen(cmd, "r");
#else
    snprintf(cmd, sizeof cmd, "python3 '%s'%s '%s' 2>/dev/null",
             script, check_only ? " --check" : "", comp_dir);
    FILE *p = popen(cmd, "r");
#endif
    if (!p) return -1;
    char line[1024] = "";
    if (fgets(line, sizeof line, p)) {
        line[strcspn(line, "\r\n")] = 0;
    }
#ifdef _WIN32
    int rc = _pclose(p);
#else
    int rc = pclose(p);
#endif
    if (rc != 0 || !line[0]) return -1;
    snprintf(zig_out, n, "%s", line);
    return 0;
}

#ifdef _WIN32
/* Write a Clang/GCC/Zig response file listing all flags and recomp_*.c files
   in gen_dir (Windows cmd.exe does not expand wildcards like recomp_*.c).
   Paths are written with forward slashes so response-file parsing never treats
   backslashes as escape sequences. */
static void write_rsp_path(FILE *f, const char *p) {
    fputc('"', f);
    for (; *p; p++) fputc(*p == '\\' ? '/' : *p, f);
    fputs("\"\n", f);
}

static int write_win_compile_rsp(const char *rsp_path, const char *tool_dir,
                                 const char *gen_dir, const char *mod_path) {
    FILE *f = fopen(rsp_path, "wb");
    if (!f) return -1;
    fprintf(f, "-shared\n-O2\n-DRECOMP_PLUGIN_DLL\n-Wno-unused-label\n");
    char buf[1024];
    fprintf(f, "-I");
    snprintf(buf, sizeof buf, "%s/runtime", tool_dir);
    write_rsp_path(f, buf);
    fprintf(f, "-I");
    write_rsp_path(f, gen_dir);
    snprintf(buf, sizeof buf, "%s/runtime/alu.c", tool_dir);
    write_rsp_path(f, buf);

    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\recomp_*.c", gen_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int count = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                snprintf(buf, sizeof buf, "%s/%s", gen_dir, fd.cFileName);
                write_rsp_path(f, buf);
                count++;
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    fprintf(f, "-o\n");
    write_rsp_path(f, mod_path);
    fclose(f);
    return count > 0 ? 0 : -1;
}

static void log_compile_error(const char *log_path) {
    FILE *f = fopen(log_path, "rb");
    if (!f) return;
    char line[512];
    int lines = 0;
    while (lines < 8 && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) { LOG("  compiler: %s", line); lines++; }
    }
    fclose(f);
}
#endif

static int compile_with_zig(const char *zig_bin, const char *comp_dir,
                            const char *tool_dir, const char *gen_dir,
                            const char *mod_path) {
    char cache_dir[800];
    snprintf(cache_dir, sizeof cache_dir, "%s/cache", comp_dir);
    MKDIR(comp_dir);
    MKDIR(cache_dir);
    char cmd[3600];
#ifdef _WIN32
    char rsp_path[850], log_path[850];
    snprintf(rsp_path, sizeof rsp_path, "%s/compile.rsp", comp_dir);
    snprintf(log_path, sizeof log_path, "%s/compile.log", comp_dir);
    if (write_win_compile_rsp(rsp_path, tool_dir, gen_dir, mod_path) != 0) {
        LOG("compile_with_zig: no recomp_*.c files found in %s", gen_dir);
        return -1;
    }
    _putenv_s("ZIG_GLOBAL_CACHE_DIR", cache_dir);
    _putenv_s("ZIG_LOCAL_CACHE_DIR", cache_dir);
    /* Outer quotes are required by cmd.exe whenever the executable path is quoted. */
    snprintf(cmd, sizeof cmd, "\"\"%s\" cc @\"%s\" >\"%s\" 2>&1\"", zig_bin, rsp_path, log_path);
    int rc = system(cmd);
    if (rc != 0) log_compile_error(log_path);
    return rc;
#else
    snprintf(cmd, sizeof cmd,
             "ZIG_GLOBAL_CACHE_DIR='%s' ZIG_LOCAL_CACHE_DIR='%s' '%s' cc -shared -fPIC -O2 -Wl,--allow-shlib-undefined -Wno-unused-label -I'%s/runtime' -I'%s' '%s'/recomp_*.c -o '%s.tmp' 2>/dev/null && mv -f '%s.tmp' '%s'",
             cache_dir, cache_dir, zig_bin, tool_dir, gen_dir, gen_dir, mod_path, mod_path, mod_path);
    return system(cmd);
#endif
}

/* When a binary or AppImage is distributed without any derived game code
   (recomp_num_entries == 0), derive the 68000 -> C source files locally on the
   user's machine from their disc image into <saves>/derived/gen/, and if a C
   compiler is available on the user's machine (or downloaded on the fly via
   the interactive compiler selection screen), compile and load
   <saves>/derived/recomp_user.so (or recomp_user.dll on Windows). */
void cpu_derive_user_code(const char *exe_dir, const char *disc_bin_path) {
    active_entries = recomp_entries;
    active_num_entries = recomp_num_entries;
    active_names = recomp_names;
    active_num_names = recomp_num_names;
    if (active_num_entries > 0) return;

    char derived_dir[700], gen_dir[750], comp_dir[750], table_c[800], mod_path[800];
    snprintf(derived_dir, sizeof derived_dir, "%s/derived", g_data_dir);
    snprintf(gen_dir, sizeof gen_dir, "%s/gen", derived_dir);
    snprintf(comp_dir, sizeof comp_dir, "%s/compiler", derived_dir);
    snprintf(table_c, sizeof table_c, "%s/recomp_table.c", gen_dir);
#ifdef _WIN32
    snprintf(mod_path, sizeof mod_path, "%s/recomp_user.dll", derived_dir);
#else
    snprintf(mod_path, sizeof mod_path, "%s/recomp_user.so", derived_dir);
#endif
    MKDIR(derived_dir);

    extern int g_headless;
    extern void host_status_banner(const char *title, const char *subtitle);
    extern int host_choose_compiler(const char *exe_dir);

    char tool_dir[600] = "";
    char gen_py[700] = "";
    FILE *tf = fopen(table_c, "rb");
    int have_table = (tf != NULL);
    if (tf) fclose(tf);

    FILE *mf = fopen(mod_path, "rb");
    int have_mod = (mf != NULL);
    if (mf) fclose(mf);

    if ((!have_table || !have_mod) && runtime_extract_embedded_tools(tool_dir, sizeof tool_dir) == 0) {
        snprintf(gen_py, sizeof gen_py, "%s/tools/gen.py", tool_dir);
    }

    /* Step 1: Derive 68000 -> C source files at the user's machine */
    if (!have_table && gen_py[0] && disc_bin_path && disc_bin_path[0]) {
        LOG("deriving 68000 -> C code from user disc image into %s ...", gen_dir);
        if (!g_headless)
            host_status_banner("DERIVING C CODE FROM YOUR DISC IMAGE...",
                               "TRANSLATING 68000 MODULES TO C IN SAVES/DERIVED/GEN/");
        char cmd[2400];
#ifdef _WIN32
        const char *py_cmd = "python";
        if (system("python --version >nul 2>&1") != 0 && system("py -3 --version >nul 2>&1") == 0)
            py_cmd = "py -3";
        snprintf(cmd, sizeof cmd, "%s \"%s\" --disc \"%s\" --out \"%s\"", py_cmd, gen_py, disc_bin_path, gen_dir);
#else
        snprintf(cmd, sizeof cmd, "python3 '%s' --disc '%s' --out '%s'", gen_py, disc_bin_path, gen_dir);
#endif
        int rc = system(cmd);
        if (rc != 0) LOG("user-side C derivation returned %d", rc);
        tf = fopen(table_c, "rb");
        have_table = (tf != NULL);
        if (tf) fclose(tf);
    }

    /* Step 2: Compile derived C code to a shared module if a C compiler is available
       (or offer to download a portable C compiler into saves/derived/compiler/) */
    if (!have_mod && have_table && tool_dir[0]) {
        int compiled_ok = 0;
        char zig_bin[1024] = "";

        /* 2a. Check if portable Zig compiler was already downloaded into saves/derived/compiler/ */
        if (find_or_fetch_portable_zig(tool_dir, comp_dir, 1, zig_bin, sizeof zig_bin) == 0) {
            LOG("compiling user-derived C code with portable Zig (%s) -> %s ...", zig_bin, mod_path);
            if (!g_headless)
                host_status_banner("COMPILING DERIVED C CODE FOR YOUR MACHINE...",
                                   "BUILDING NATIVE MODULE WITH PORTABLE ZIG CC...");
            if (compile_with_zig(zig_bin, comp_dir, tool_dir, gen_dir, mod_path) == 0) {
                compiled_ok = 1;
            }
        }

        /* 2b. Check for a working system C compiler on PATH */
        if (!compiled_ok) {
#ifdef _WIN32
            const char *cc_bin = NULL;
            int is_sys_zig = 0;
            if (system("where zig >nul 2>&1") == 0) { cc_bin = "zig"; is_sys_zig = 1; }
            else if (system("where gcc >nul 2>&1") == 0) cc_bin = "gcc";
            else if (system("where clang >nul 2>&1") == 0) cc_bin = "clang";
            if (cc_bin) {
                LOG("compiling user-derived C code with system %s -> %s ...", cc_bin, mod_path);
                if (!g_headless)
                    host_status_banner("COMPILING DERIVED C CODE FOR YOUR MACHINE...",
                                       "BUILDING NATIVE MODULE SAVES/DERIVED/RECOMP_USER.DLL");
                int rc;
                if (is_sys_zig) {
                    rc = compile_with_zig(cc_bin, comp_dir, tool_dir, gen_dir, mod_path);
                } else {
                    char rsp_path[850], log_path[850], cmd[2800];
                    MKDIR(comp_dir);
                    snprintf(rsp_path, sizeof rsp_path, "%s/compile.rsp", comp_dir);
                    snprintf(log_path, sizeof log_path, "%s/compile.log", comp_dir);
                    if (write_win_compile_rsp(rsp_path, tool_dir, gen_dir, mod_path) == 0) {
                        snprintf(cmd, sizeof cmd, "\"%s @\"%s\" >\"%s\" 2>&1\"", cc_bin, rsp_path, log_path);
                        rc = system(cmd);
                        if (rc != 0) log_compile_error(log_path);
                    } else {
                        rc = -1;
                    }
                }
                if (rc == 0) compiled_ok = 1;
                else LOG("system compiler %s failed (%d)", cc_bin, rc);
            }
#else
            const char *cc_bin = NULL;
            int is_sys_zig = 0;
            if (system("command -v cc >/dev/null 2>&1") == 0) cc_bin = "cc";
            else if (system("command -v gcc >/dev/null 2>&1") == 0) cc_bin = "gcc";
            else if (system("command -v clang >/dev/null 2>&1") == 0) cc_bin = "clang";
            else if (system("command -v zig >/dev/null 2>&1") == 0) { cc_bin = "zig"; is_sys_zig = 1; }

            if (cc_bin) {
                LOG("compiling user-derived C code with system %s -> %s ...", cc_bin, mod_path);
                if (!g_headless)
                    host_status_banner("COMPILING DERIVED C CODE FOR YOUR MACHINE...",
                                       "BUILDING NATIVE SHARED MODULE SAVES/DERIVED/RECOMP_USER.SO");
                int rc;
                if (is_sys_zig) {
                    rc = compile_with_zig(cc_bin, comp_dir, tool_dir, gen_dir, mod_path);
                } else {
                    char cmd[2600];
                    snprintf(cmd, sizeof cmd,
                             "'%s' -shared -fPIC -O2 -Wno-unused-label -I'%s/runtime' -I'%s' '%s'/recomp_*.c -o '%s.tmp' 2>/dev/null && mv -f '%s.tmp' '%s'",
                             cc_bin, tool_dir, gen_dir, gen_dir, mod_path, mod_path, mod_path);
                    rc = system(cmd);
                }
                if (rc == 0) compiled_ok = 1;
                else LOG("system compiler %s failed (%d)", cc_bin, rc);
            }
#endif
        }

        /* 2c. No working C compiler found on host: show interactive selection screen
               to download a portable C compiler (Zig/Clang) or use the built-in 68000 engine */
        if (!compiled_ok && !g_headless) {
            int want_download = host_choose_compiler(exe_dir);
            if (want_download == 1) {
                LOG("downloading portable Zig C compiler into %s ...", comp_dir);
                host_status_banner("DOWNLOADING PORTABLE C COMPILER (ZIG CC)...",
                                   "DOWNLOADING TO SAVES/DERIVED/COMPILER/ (ONE-TIME, ~45 MB)...");
                if (find_or_fetch_portable_zig(tool_dir, comp_dir, 0, zig_bin, sizeof zig_bin) == 0) {
                    LOG("portable Zig C compiler ready at %s; compiling %s ...", zig_bin, mod_path);
                    host_status_banner("COMPILING DERIVED C CODE FOR YOUR MACHINE...",
                                       "BUILDING NATIVE MODULE WITH PORTABLE ZIG CC (10-20 SECONDS)...");
                    int rc = compile_with_zig(zig_bin, comp_dir, tool_dir, gen_dir, mod_path);
                    if (rc == 0) {
                        compiled_ok = 1;
                    } else {
                        LOG("portable Zig compilation failed (%d); falling back to built-in 68000 engine", rc);
                        host_status_banner("COMPILATION FAILED - USING BUILT-IN 68000 ENGINE",
                                           "STARTING GAME WITH BUILT-IN 68000 ENGINE...");
                        host_sleep_until(host_time() + 1.5);
                    }
                } else {
                    LOG("portable Zig download failed; falling back to built-in 68000 engine");
                    host_status_banner("COMPILER DOWNLOAD FAILED (CHECK INTERNET CONNECTION)",
                                       "STARTING GAME WITH BUILT-IN 68000 ENGINE...");
                    host_sleep_until(host_time() + 1.5);
                }
            } else {
                LOG("user selected built-in 68000 engine (derived C sources are in %s)", gen_dir);
            }
        } else if (!compiled_ok) {
            LOG("no C compiler found on host; derived C sources are in %s (running with built-in 68000 engine)", gen_dir);
        }
    }

    if (tool_dir[0]) runtime_cleanup_embedded_tools(tool_dir);

    /* Step 3: Dynamically load the user-derived native module */
    mf = fopen(mod_path, "rb");
    if (mf) {
        fclose(mf);
#ifdef _WIN32
        HMODULE dl = LoadLibraryA(mod_path);
        if (dl) {
            typedef void (*PluginInitFn)(const RecompHostAPI *);
            PluginInitFn pinit = (PluginInitFn)GetProcAddress(dl, "recomp_plugin_init");
            if (pinit) {
                static const RecompHostAPI api = {
                    &m68k, &RAM, &jt_expected,
                    recomp_bad_entry, m68k_call, m68k_exception,
                    m68k_trap, os9_syscall, jt_check, game_skip_insn
                };
                pinit(&api);
            }
            const RecompEntry *ent = (const RecompEntry *)GetProcAddress(dl, "recomp_entries");
            const int *nent = (const int *)GetProcAddress(dl, "recomp_num_entries");
            const RecompName *nam = (const RecompName *)GetProcAddress(dl, "recomp_names");
            const int *nnam = (const int *)GetProcAddress(dl, "recomp_num_names");
            const uint32_t *clo = (const uint32_t *)GetProcAddress(dl, "recomp_code_lo");
            const uint32_t *chi = (const uint32_t *)GetProcAddress(dl, "recomp_code_hi");
            if (ent && nent && *nent > 0 && clo && chi && *chi > *clo) {
                active_entries = ent;
                active_num_entries = *nent;
                if (nam && nnam) { active_names = nam; active_num_names = *nnam; }
                recomp_code_lo = *clo;
                recomp_code_hi = *chi;
                LOG("loaded %d user-derived recompiled entry points from %s", active_num_entries, mod_path);
            }
        }
#else
        void *dl = dlopen(mod_path, RTLD_NOW | RTLD_LOCAL);
        if (!dl) {
            /* If saves/ is on an SD card mounted with noexec (/run/media/...),
               copy recomp_user.so to /tmp and dlopen from there. */
            char tmp_so[256], cp_cmd[1200];
            snprintf(tmp_so, sizeof tmp_so, "/tmp/apprentice_recomp_%u.so", (unsigned)getuid());
            snprintf(cp_cmd, sizeof cp_cmd, "cp -f '%s' '%s' && chmod 755 '%s'", mod_path, tmp_so, tmp_so);
            if (system(cp_cmd) == 0) dl = dlopen(tmp_so, RTLD_NOW | RTLD_LOCAL);
        }
        if (dl) {
            const RecompEntry *ent = (const RecompEntry *)dlsym(dl, "recomp_entries");
            const int *nent = (const int *)dlsym(dl, "recomp_num_entries");
            const RecompName *nam = (const RecompName *)dlsym(dl, "recomp_names");
            const int *nnam = (const int *)dlsym(dl, "recomp_num_names");
            const uint32_t *clo = (const uint32_t *)dlsym(dl, "recomp_code_lo");
            const uint32_t *chi = (const uint32_t *)dlsym(dl, "recomp_code_hi");
            if (ent && nent && *nent > 0 && clo && chi && *chi > *clo) {
                active_entries = ent;
                active_num_entries = *nent;
                if (nam && nnam) { active_names = nam; active_num_names = *nnam; }
                recomp_code_lo = *clo;
                recomp_code_hi = *chi;
                LOG("loaded %d user-derived recompiled entry points from %s", active_num_entries, mod_path);
            }
        } else {
            LOG("dlopen(%s) failed: %s", mod_path, dlerror());
        }
#endif
    }
}

void cpu_init(void) {
    if (!active_entries) {
        active_entries = recomp_entries;
        active_num_entries = recomp_num_entries;
        active_names = recomp_names;
        active_num_names = recomp_num_names;
    }
    if (active_num_entries <= 0 || recomp_code_hi <= recomp_code_lo) {
        dispatch_n = 0;
        dispatch = NULL;
        return;
    }
    dispatch_n = (recomp_code_hi - recomp_code_lo) / 2 + 1;
    dispatch = (RecompFn *)calloc(dispatch_n, sizeof(RecompFn));
    for (int i = 0; i < active_num_entries; i++) {
        uint32_t a = active_entries[i].addr;
        if (a >= recomp_code_lo && a < recomp_code_hi)
            dispatch[(a - recomp_code_lo) / 2] = active_entries[i].fn;
    }
}

const char *func_name(uint32_t pc) {
    /* nearest symbol (MacsBug or .stb) at or below pc */
    const char *best = "?";
    uint32_t ba = 0;
    for (int i = 0; i < active_num_names; i++) {
        if (active_names[i].addr <= pc && active_names[i].addr >= ba) {
            ba = active_names[i].addr;
            best = active_names[i].name;
        }
    }
    return best;
}

/* Is there translated code for this exact address?  Addresses inside a
   recompiled module that the analyzer did not discover have none: they run in
   the interpreter, which hands back to translated code at the next known
   entry point. */
int recomp_has_entry(uint32_t pc) {
    if (!dispatch || pc < recomp_code_lo || pc >= recomp_code_hi || (pc & 1)) return 0;
    return dispatch[(pc - recomp_code_lo) / 2] != NULL;
}

static void note_undiscovered(uint32_t pc) {
    if (!dispatch) return;
    static uint32_t seen[256];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == pc) return;
    if (nseen < 256) seen[nseen++] = pc;
    LOG("note: code at %06x (%s) was not found by the analyzer; it runs in the interpreter "
        "(add it to extra_entries in game.json)", pc, func_name(pc));
}

/* Resolve pc to a host function; follows "jmp abs.l" stubs in the a5/a6
   jump tables that live in RAM. */
static RecompFn lookup(uint32_t *ppc) {
    if (!dispatch) return NULL;
    uint32_t pc = *ppc;
    for (int hops = 0; hops < 4; hops++) {
        if (pc >= recomp_code_lo && pc < recomp_code_hi && !(pc & 1)) {
            RecompFn f = dispatch[(pc - recomp_code_lo) / 2];
            if (f) { *ppc = pc; return f; }
            break;
        }
        if (RW(pc) == 0x4EF9) {          /* jmp abs.l (jump table stub): count it as executed */
            pc = RL(pc + 2);
            m68k.cyc++;
            continue;
        }
        break;
    }
    *ppc = pc;
    return NULL;
}

void fatal(const char *fmt, ...);
uint32_t interp_run(uint32_t pc);

uint32_t recomp_bad_entry(uint32_t pc, uint32_t func) {
    /* a label of another function's body, or an address in the middle of a
       function: let the dispatcher (or the interpreter) take it */
    (void)func;
    if (recomp_has_entry(pc)) return m68k_call(pc);
    note_undiscovered(pc);
    return interp_run(pc);
}

uint32_t interp_run(uint32_t pc);

int g_cdi_pause_req;

uint32_t cpu_hook_cdi_pause(uint32_t target, uint32_t ret_pc) {
    /* When the user presses Select / View (⧉) in-game, trigger the original
       CD-i built-in pause screen via the level module's pause-check entry
       116(a5) (or confirm/unpause via 112(a5) if already paused) without
       requiring a 50-frame Button 1+2 hold that makes Marvin jump. */
    if (g_cdi_pause_req > 0 && !g_audio_muted && target == A(5) + 116) {
        if (RW(target) == 0x6000) {
            uint32_t fn = target + 2 + (int16_t)RW(target + 2);
            if (RW(fn) == 0x343A && RW(fn + 4) == 0x263A && RW(fn + 8) == 0x41FA) {
                uint32_t addr_d3 = fn + 6 + (int16_t)RW(fn + 6);
                uint32_t addr_a0 = fn + 10 + (int16_t)RW(fn + 10);
                if (ret_pc != 0x109fd6u) {
                    D(2) = (D(2) & 0xFFFF0000u) | 0x8000u;
                    D(3) = 0x00008003u;
                    WW(addr_a0 + 12, 0x0031);
                    g_cdi_pause_req = 0;
                    return fn + 8;
                } else {
                    WL(addr_d3, 0x00008001u);
                    g_cdi_pause_req = 0;
                }
            }
        }
    }
    return target;
}

uint32_t m68k_call(uint32_t target) {
    extern int g_interp_all;
    uint32_t pc = cpu_hook_cdi_pause(target, RL(A(7)));
    if (g_interp_all) return interp_run(pc);
    RecompFn f = lookup(&pc);
    if (!f) {
        if (pc >= recomp_code_lo && pc < recomp_code_hi) note_undiscovered(pc);
        return interp_run(pc);
    }
    return f(pc);
}

void jt_check(int reg, uint32_t addr) {
    fatal("A%d changed (%08x, expected %08x) at %06x", reg, A(reg), jt_expected, addr);
}

uint32_t m68k_exception(int vec, uint32_t addr) {
    fatal("CPU exception %d at %06x (%s)", vec, addr, func_name(addr));
    return 0;
}

/* ---------------------------------------------------------------- run loop */
void cpu_run_until(volatile int *flag) {
    extern int g_interp_all;
    while (!*flag) {
        uint32_t pc = m68k.pc;
        RecompFn f = g_interp_all ? NULL : lookup(&pc);
        uint32_t r;
        if (f) r = f(pc);
        else {
            if (!g_interp_all && pc >= recomp_code_lo && pc < recomp_code_hi) note_undiscovered(pc);
            r = interp_run(pc);
        }
        /* PC_UNWIND: a system call already put the resume address in m68k.pc */
        if (r != PC_UNWIND) m68k.pc = r;
        /* the scheduler runs only where code yielded (backward branches), so
           translated and interpreted execution see events at the same points */
        if (m68k.yield) {
            m68k.yield = 0;
            sched_service();
        }
    }
}

void cpu_run(void) {
    os9_run();      /* the OS-9 scheduler: runs every process until it gives up the CPU */
}
