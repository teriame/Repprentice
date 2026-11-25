#!/usr/bin/env python3
"""
Register The Apprentice (AppImage or native Linux binary) as a Non-Steam Game
in Steam, ensuring it runs natively on Linux / SteamOS (Steam Deck) without
being misrouted through Proton or failing inside Steam's container runtime.

Inspired by how Recompia (Dittobyte/Recompia) and SteamOS launchers register
native Linux AppImages into Steam:
  1. Updates or creates the entry in ~/.local/share/Steam/userdata/<id>/config/shortcuts.vdf
     (Valve's binary KeyValues format) with `APPIMAGE_EXTRACT_AND_RUN=1` in
     LaunchOptions so FUSE/bwrap namespace restrictions in Game Mode never fail.
  2. Removes any accidentally assigned Proton compatibility mapping for this
     shortcut in ~/.local/share/Steam/config/config.vdf (`CompatToolMapping`)
     so SteamOS always executes the native Linux ELF binary directly.
  3. Installs a desktop icon and writes ~/.local/share/applications/apprentice-recomp.desktop.
  4. If Steam is currently running, also notifies the live Steam client via
     `/tmp/addnonsteamgamefile` + `steam://addnonsteamgame/` (or `steamos-add-to-steam`).

Usage:
  python3 tools/steam_register.py [--exe /path/to/Apprentice.AppImage] [--check] [--auto]
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.parse
import zlib


APP_NAME = "The Apprentice"
TYPE_MAP = 0x00
TYPE_STR = 0x01
TYPE_INT = 0x02
TYPE_END = 0x08


def calc_shortcut_appid(exe_quoted: str, app_name: str) -> int:
    """Calculate the unsigned 32-bit Steam shortcut AppID (top bit set)."""
    key = (exe_quoted + app_name).encode("utf-8")
    crc = zlib.crc32(key) & 0xFFFFFFFF
    return crc | 0x80000000


def parse_binary_vdf(data: bytes) -> dict:
    """Parse Valve's binary KeyValues format (shortcuts.vdf)."""
    idx = 0
    n = len(data)

    def read_cstr() -> str:
        nonlocal idx
        end = data.find(b"\x00", idx)
        if end < 0:
            s = data[idx:].decode("utf-8", errors="replace")
            idx = n
            return s
        s = data[idx:end].decode("utf-8", errors="replace")
        idx = end + 1
        return s

    def read_map() -> dict:
        nonlocal idx
        out = {}
        while idx < n:
            t = data[idx]
            idx += 1
            if t == TYPE_END:
                break
            key = read_cstr()
            if t == TYPE_MAP:
                out[key] = read_map()
            elif t == TYPE_STR:
                out[key] = read_cstr()
            elif t == TYPE_INT:
                if idx + 4 <= n:
                    val = struct.unpack("<I", data[idx:idx + 4])[0]
                    idx += 4
                else:
                    val = 0
                    idx = n
                out[key] = val
            else:
                raise ValueError(f"Unknown binary VDF type 0x{t:02x} at byte {idx - 1}")
        return out

    if not data:
        return {"shortcuts": {}}
    return read_map()


def dump_binary_vdf(root: dict) -> bytes:
    """Serialize a dictionary back into Valve's binary KeyValues format."""
    buf = bytearray()

    def write_cstr(s: str):
        buf.extend(s.encode("utf-8"))
        buf.append(0)

    def write_map(d: dict):
        for k, v in d.items():
            if isinstance(v, dict):
                buf.append(TYPE_MAP)
                write_cstr(str(k))
                write_map(v)
            elif isinstance(v, str):
                buf.append(TYPE_STR)
                write_cstr(str(k))
                write_cstr(v)
            elif isinstance(v, int):
                buf.append(TYPE_INT)
                write_cstr(str(k))
                buf.extend(struct.pack("<I", v & 0xFFFFFFFF))
        buf.append(TYPE_END)

    write_map(root)
    return bytes(buf)


def find_steam_roots() -> list:
    """Locate Steam root directories (native SteamOS / Linux & Flatpak Steam)."""
    home = os.path.expanduser("~")
    candidates = [
        os.path.join(home, ".local/share/Steam"),
        os.path.join(home, ".steam/steam"),
        os.path.join(home, ".steam/root"),
        os.path.join(home, ".var/app/com.valvesoftware.Steam/.local/share/Steam"),
    ]
    roots = []
    seen = set()
    for c in candidates:
        if os.path.isdir(c):
            real = os.path.realpath(c)
            if real not in seen and os.path.isdir(os.path.join(real, "userdata")):
                seen.add(real)
                roots.append(real)
    return roots


def is_steam_running() -> bool:
    try:
        r = subprocess.run(["pgrep", "-x", "steam"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return r.returncode == 0
    except Exception:
        return False


def clear_proton_compat_mapping(steam_root: str, appids: list) -> bool:
    """
    If Steam's config.vdf has a CompatToolMapping forcing Proton on our
    native Linux shortcut AppID, remove that block so Steam runs it natively.
    """
    cfg_path = os.path.join(steam_root, "config", "config.vdf")
    if not os.path.isfile(cfg_path):
        return False
    try:
        with open(cfg_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except Exception:
        return False

    modified = False
    for aid in appids:
        # Check both unsigned 32-bit, signed 32-bit, and 64-bit representations
        signed_aid = aid - 0x100000000 if aid >= 0x80000000 else aid
        for key in (str(aid), str(signed_aid)):
            pattern = re.compile(
                r'\s*"' + re.escape(key) + r'"\s*\{\s*[^}]*"name"\s*"[^"]*proton[^"]*"[^}]*\}',
                re.IGNORECASE | re.DOTALL,
            )
            new_text, count = pattern.subn("", text)
            if count > 0:
                text = new_text
                modified = True

    if modified:
        try:
            shutil.copy2(cfg_path, cfg_path + ".bak")
            with open(cfg_path, "w", encoding="utf-8") as f:
                f.write(text)
        except Exception:
            return False
    return modified


def install_icon_and_desktop(exe_path: str, start_dir: str) -> tuple:
    """
    Install the application icon and .desktop entry into ~/.local/share/
    so both Steam Desktop Mode and Gaming Mode display the proper icon and metadata.
    """
    home = os.path.expanduser("~")
    icon_dir = os.path.join(home, ".local/share/icons/hicolor/scalable/apps")
    apps_dir = os.path.join(home, ".local/share/applications")
    os.makedirs(icon_dir, exist_ok=True)
    os.makedirs(apps_dir, exist_ok=True)

    icon_path = os.path.join(icon_dir, "apprentice-recomp.svg")
    svg_content = """<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color="#1c2d4f"/>
      <stop offset="100%" stop-color="#0d1424"/>
    </linearGradient>
    <linearGradient id="hat" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%" stop-color="#4a78d0"/>
      <stop offset="100%" stop-color="#224088"/>
    </linearGradient>
  </defs>
  <rect width="256" height="256" rx="40" fill="url(#bg)" stroke="#ffe070" stroke-width="6"/>
  <path d="M128 32 L188 168 L68 168 Z" fill="url(#hat)" stroke="#ffe070" stroke-width="5" stroke-linejoin="round"/>
  <ellipse cx="128" cy="170" rx="84" ry="20" fill="url(#hat)" stroke="#ffe070" stroke-width="5"/>
  <polygon points="128,75 133,88 147,88 136,96 140,109 128,101 116,109 120,96 109,88 123,88" fill="#ffe070"/>
  <circle cx="105" cy="135" r="5" fill="#ffe070"/>
  <circle cx="152" cy="130" r="6" fill="#ffe070"/>
  <rect x="32" y="198" width="192" height="34" rx="8" fill="#141e30" stroke="#ffe070" stroke-width="3"/>
  <text x="128" y="221" font-family="sans-serif" font-size="20" font-weight="bold" fill="#ffe070" text-anchor="middle">APPRENTICE</text>
</svg>
"""
    try:
        with open(icon_path, "w", encoding="utf-8") as f:
            f.write(svg_content)
    except Exception:
        pass

    is_appimage = exe_path.lower().endswith(".appimage")
    env_prefix = "env APPIMAGE_EXTRACT_AND_RUN=1 " if is_appimage else ""
    desktop_path = os.path.join(apps_dir, "apprentice-recomp.desktop")
    desktop_content = f"""[Desktop Entry]
Type=Application
Name={APP_NAME}
GenericName=CD-i Game
Comment=The Apprentice (1994, Philips CD-i) Native Linux & SteamOS Edition
Exec={env_prefix}"{exe_path}"
Path={start_dir}
Icon={icon_path}
Terminal=false
Categories=Game;ActionGame;ArcadeGame;
StartupWMClass=apprentice
X-Steam-Library-Capsule={icon_path}
"""
    try:
        with open(desktop_path, "w", encoding="utf-8") as f:
            f.write(desktop_content)
        os.chmod(desktop_path, 0o755)
    except Exception:
        pass

    return icon_path, desktop_path


def check_is_registered(exe_path: str) -> bool:
    """Return True if exe_path is already registered in any Steam user's shortcuts.vdf."""
    exe_norm = os.path.realpath(exe_path)
    for root in find_steam_roots():
        ud = os.path.join(root, "userdata")
        if not os.path.isdir(ud):
            continue
        for uid in os.listdir(ud):
            if uid == "0" or not uid.isdigit():
                continue
            vdf_path = os.path.join(ud, uid, "config", "shortcuts.vdf")
            if not os.path.isfile(vdf_path):
                continue
            try:
                with open(vdf_path, "rb") as f:
                    data = parse_binary_vdf(f.read())
                shortcuts = data.get("shortcuts", data.get("Shortcuts", {}))
                for _, entry in shortcuts.items():
                    if not isinstance(entry, dict):
                        continue
                    e_exe = str(entry.get("Exe", entry.get("exe", ""))).strip('"')
                    e_name = str(entry.get("AppName", entry.get("appname", "")))
                    if e_name == APP_NAME or (e_exe and os.path.realpath(e_exe) == exe_norm):
                        return True
            except Exception:
                continue
    return False


def register_in_steam(exe_path: str, notify_live_steam: bool = True) -> tuple:
    """
    Register or update The Apprentice in all Steam userdata shortcuts.vdf files
    and clear any Proton compatibility override.
    Returns (success: bool, message: str).
    """
    exe_path = os.path.abspath(exe_path)
    try:
        os.chmod(exe_path, os.stat(exe_path).st_mode | 0o111)
    except Exception:
        pass

    start_dir = os.path.dirname(exe_path)
    icon_path, desktop_path = install_icon_and_desktop(exe_path, start_dir)

    exe_quoted = f'"{exe_path}"'
    start_quoted = f'"{start_dir}"'
    is_appimage = exe_path.lower().endswith(".appimage")
    launch_opts = "APPIMAGE_EXTRACT_AND_RUN=1 %command%" if is_appimage else ""
    appid = calc_shortcut_appid(exe_quoted, APP_NAME)

    roots = find_steam_roots()
    updated_users = 0
    cleared_proton = False
    was_already_registered = check_is_registered(exe_path)

    for root in roots:
        if clear_proton_compat_mapping(root, [appid]):
            cleared_proton = True
        ud = os.path.join(root, "userdata")
        if not os.path.isdir(ud):
            continue
        for uid in os.listdir(ud):
            if uid == "0" or not uid.isdigit():
                continue
            cfg_dir = os.path.join(ud, uid, "config")
            os.makedirs(cfg_dir, exist_ok=True)
            vdf_path = os.path.join(cfg_dir, "shortcuts.vdf")

            root_dict = {"shortcuts": {}}
            if os.path.isfile(vdf_path):
                try:
                    with open(vdf_path, "rb") as f:
                        raw = f.read()
                    if raw:
                        root_dict = parse_binary_vdf(raw)
                    shutil.copy2(vdf_path, vdf_path + ".bak")
                except Exception:
                    root_dict = {"shortcuts": {}}

            # Find or create the "shortcuts" map
            sc_key = "shortcuts"
            for k in root_dict:
                if k.lower() == "shortcuts":
                    sc_key = k
                    break
            if sc_key not in root_dict or not isinstance(root_dict[sc_key], dict):
                root_dict[sc_key] = {}
            shortcuts = root_dict[sc_key]

            # Look for an existing entry for The Apprentice
            target_idx = None
            old_appids = [appid]
            for idx, entry in shortcuts.items():
                if not isinstance(entry, dict):
                    continue
                e_name = str(entry.get("AppName", entry.get("appname", "")))
                e_exe = str(entry.get("Exe", entry.get("exe", ""))).strip('"')
                if e_name == APP_NAME or "apprentice" in os.path.basename(e_exe).lower():
                    target_idx = idx
                    old_id = entry.get("appid")
                    if isinstance(old_id, int):
                        old_appids.append(old_id)
                    break

            if len(old_appids) > 1 and clear_proton_compat_mapping(root, old_appids):
                cleared_proton = True

            if target_idx is None:
                # Next numeric index as string ("0", "1", ...)
                existing_nums = [int(k) for k in shortcuts.keys() if str(k).isdigit()]
                next_num = max(existing_nums) + 1 if existing_nums else 0
                target_idx = str(next_num)

            shortcuts[target_idx] = {
                "appid": appid,
                "AppName": APP_NAME,
                "Exe": exe_quoted,
                "StartDir": start_quoted,
                "icon": icon_path,
                "ShortcutPath": "",
                "LaunchOptions": launch_opts,
                "IsHidden": 0,
                "AllowDesktopConfig": 1,
                "AllowOverlay": 1,
                "OpenVR": 0,
                "Devkit": 0,
                "DevkitGameID": "",
                "DevkitOverrideAppID": 0,
                "LastPlayTime": 0,
                "FlatpakAppID": "",
                "tags": {"0": "Favorite"},
            }

            try:
                out_bytes = dump_binary_vdf(root_dict)
                tmp_path = vdf_path + ".tmp"
                with open(tmp_path, "wb") as f:
                    f.write(out_bytes)
                os.replace(tmp_path, vdf_path)
                updated_users += 1
            except Exception as e:
                return False, f"Failed writing {vdf_path}: {e}"

    steam_live = is_steam_running()
    live_notified = False
    # If Steam is currently running in Desktop Mode and this is a brand-new
    # registration (or no shortcuts.vdf could be written directly), also notify
    # the live Steam client via steamos-add-to-steam / steam://addnonsteamgame
    # so the running Steam session does not overwrite shortcuts.vdf on exit.
    if notify_live_steam and steam_live and os.environ.get("STEAM_GAMEPADUI") != "1":
        if updated_users == 0 or not was_already_registered:
            try:
                if shutil.which("steamos-add-to-steam"):
                    subprocess.Popen(
                        ["steamos-add-to-steam", desktop_path],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    live_notified = True
                elif shutil.which("steam"):
                    with open("/tmp/addnonsteamgamefile", "w", encoding="utf-8") as f:
                        f.write(desktop_path)
                    enc = urllib.parse.quote(desktop_path, safe="")
                    subprocess.Popen(
                        ["steam", f"steam://addnonsteamgame/{enc}"],
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                    live_notified = True
            except Exception:
                pass

    if updated_users > 0:
        msg = f"REGISTERED IN STEAM ({updated_users} PROFILE{'S' if updated_users != 1 else ''})"
        if cleared_proton:
            msg += " - REMOVED PROTON OVERRIDE (NATIVE LINUX)"
        elif steam_live:
            msg += " - RESTART STEAM OR SWITCH TO GAMING MODE"
        else:
            msg += " - READY FOR GAMING MODE!"
        return True, msg

    if live_notified:
        return True, "SENT TO RUNNING STEAM CLIENT (NATIVE LINUX DESKTOP ENTRY)"

    return False, "NO STEAM INSTALLATION FOUND IN ~/.local/share/Steam"


def main():
    ap = argparse.ArgumentParser(description="Register The Apprentice as a native Linux Non-Steam Game.")
    ap.add_argument("--exe", help="Path to Apprentice.AppImage or apprentice_hd binary", default="")
    ap.add_argument("--check", action="store_true", help="Exit 0 if already registered in Steam, 1 otherwise")
    ap.add_argument("--auto", action="store_true", help="Register automatically only if not already registered")
    args = ap.parse_args()

    exe = args.exe
    if not exe:
        exe = os.environ.get("APPIMAGE", "")
    if not exe:
        repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        for cand in ("dist/Apprentice-1.0.0-x86_64.AppImage", "apprentice_hd", "apprentice"):
            p = os.path.join(repo_dir, cand)
            if os.path.isfile(p):
                exe = p
                break
    if not exe or not os.path.exists(exe):
        print("ERROR: Could not locate Apprentice executable or AppImage", file=sys.stderr)
        sys.exit(1)

    exe = os.path.abspath(exe)
    if args.check:
        sys.exit(0 if check_is_registered(exe) else 1)

    if args.auto and check_is_registered(exe):
        print(f"Already registered in Steam: {exe}")
        sys.exit(0)

    ok, msg = register_in_steam(exe)
    print(msg)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
