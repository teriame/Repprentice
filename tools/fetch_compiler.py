#!/usr/bin/env python3
"""
Downloads a portable, self-contained C compiler (Zig/Clang) on the fly into
<saves>/derived/compiler/ (strictly inside CWD, with /tmp execution fallback if
saves/ is on a noexec/exFAT SD card) when no system C compiler is installed on
the user's machine (e.g. SteamOS / Steam Deck or stock Windows).
"""
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

ZIG_VERSION = "0.13.0"

URLS = {
    ("linux", "x86_64"): [
        f"https://ziglang.org/download/{ZIG_VERSION}/zig-linux-x86_64-{ZIG_VERSION}.tar.xz",
        f"https://pkg.machengine.org/zig/zig-linux-x86_64-{ZIG_VERSION}.tar.xz",
    ],
    ("linux", "aarch64"): [
        f"https://ziglang.org/download/{ZIG_VERSION}/zig-linux-aarch64-{ZIG_VERSION}.tar.xz",
        f"https://pkg.machengine.org/zig/zig-linux-aarch64-{ZIG_VERSION}.tar.xz",
    ],
    ("windows", "x86_64"): [
        f"https://ziglang.org/download/{ZIG_VERSION}/zig-windows-x86_64-{ZIG_VERSION}.zip",
        f"https://pkg.machengine.org/zig/zig-windows-x86_64-{ZIG_VERSION}.zip",
    ],
}


def find_existing_zig(dest_dir: str) -> str:
    exe_name = "zig.exe" if os.name == "nt" else "zig"
    if not os.path.isdir(dest_dir):
        return ""
    for root, _, files in os.walk(dest_dir):
        if exe_name in files:
            p = os.path.join(root, exe_name)
            if os.path.isfile(p):
                return p
    return ""


def can_execute(zig_bin: str) -> bool:
    if not zig_bin or not os.path.isfile(zig_bin):
        return False
    if os.name != "nt":
        try:
            os.chmod(zig_bin, 0o755)
        except OSError:
            pass
    try:
        rc = subprocess.call(
            [zig_bin, "version"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return rc == 0
    except Exception:
        return False


def ensure_runnable(zig_bin: str) -> str:
    if can_execute(zig_bin):
        return zig_bin
    if os.name == "nt":
        return ""
    # If saves/ is on a noexec or exFAT SD card (/run/media/...), copy the
    # extracted compiler tree to /tmp/apprentice_zig_<uid> and run from there.
    zig_root = os.path.dirname(zig_bin)
    tmp_root = os.path.join(tempfile.gettempdir(), f"apprentice_zig_{os.getuid()}")
    tmp_bin = os.path.join(tmp_root, "zig")
    if can_execute(tmp_bin):
        return tmp_bin
    try:
        if os.path.exists(tmp_root):
            shutil.rmtree(tmp_root, ignore_errors=True)
        shutil.copytree(zig_root, tmp_root)
        if can_execute(tmp_bin):
            return tmp_bin
    except Exception:
        pass
    return ""


def download_file(url: str, out_path: str) -> bool:
    if shutil.which("curl"):
        rc = subprocess.call(["curl", "-fL", "--connect-timeout", "15", "-o", out_path, url])
        if rc == 0 and os.path.isfile(out_path) and os.path.getsize(out_path) > 1000000:
            return True
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Apprentice-Recomp/1.0"})
        with urllib.request.urlopen(req, timeout=30) as resp, open(out_path, "wb") as out_f:
            shutil.copyfileobj(resp, out_f)
        return os.path.isfile(out_path) and os.path.getsize(out_path) > 1000000
    except Exception as e:
        print(f"Download error ({url}): {e}", file=sys.stderr)
        return False


def main() -> int:
    check_only = "--check" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--check"]
    if not args:
        print("Usage: fetch_compiler.py [--check] <dest_dir>", file=sys.stderr)
        return 1

    dest_dir = os.path.abspath(args[0])
    existing = find_existing_zig(dest_dir)
    if existing:
        runnable = ensure_runnable(existing)
        if runnable:
            print(runnable)
            return 0

    if check_only:
        return 1

    os.makedirs(dest_dir, exist_ok=True)
    sys_name = "windows" if os.name == "nt" else "linux"
    mach = platform.machine().lower()
    if mach in ("amd64", "x64"):
        mach = "x86_64"
    elif mach in ("arm64",):
        mach = "aarch64"

    urls = URLS.get((sys_name, mach))
    if not urls:
        print(f"Unsupported platform for portable compiler: {sys_name} {mach}", file=sys.stderr)
        return 1

    is_zip = urls[0].endswith(".zip")
    archive_path = os.path.join(dest_dir, "zig_pkg.zip" if is_zip else "zig_pkg.tar.xz")

    ok = False
    for url in urls:
        if os.path.exists(archive_path):
            try:
                os.remove(archive_path)
            except OSError:
                pass
        if download_file(url, archive_path):
            ok = True
            break

    if not ok:
        if os.path.exists(archive_path):
            try:
                os.remove(archive_path)
            except OSError:
                pass
        return 1

    try:
        if is_zip:
            with zipfile.ZipFile(archive_path, "r") as zf:
                zf.extractall(dest_dir)
        else:
            with tarfile.open(archive_path, "r:xz") as tf:
                tf.extractall(dest_dir)
    except Exception as e:
        print(f"Extraction failed: {e}", file=sys.stderr)
        return 1
    finally:
        if os.path.exists(archive_path):
            try:
                os.remove(archive_path)
            except OSError:
                pass

    zig_bin = find_existing_zig(dest_dir)
    runnable = ensure_runnable(zig_bin)
    if not runnable:
        return 1

    print(runnable)
    return 0


if __name__ == "__main__":
    sys.exit(main())
