#!/usr/bin/env python3
"""Download and install official Godot export templates matching this fork.

The editor looks for export templates in a directory named after the engine
version string (for this fork: "4.5.dev") inside the user data directory.
Official template packages are only published for upstream builds, so this
script maps the fork to the closest-era upstream release and installs its
templates under the fork's version string.

Why 4.5-dev3: the fork branched from upstream at 4e6451d62a (2025-04-25),
the same day the 4.5-dev3 snapshot was published. Same-era templates avoid
dev-cycle format and API drift between the editor and the exported runtime,
which 4.5-stable (five months later) would not. If the fork is rebased onto
a newer upstream, update RELEASE_TAG to the snapshot closest to the new base
commit (publish dates: https://github.com/godotengine/godot-builds/releases).

Stock upstream templates are compatible because the AI module is editor-only
(TOOLS_ENABLED); nothing from the fork ships in exported games.

Usage: python misc/scripts/install_export_templates.py [--force]
"""

import argparse
import os
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

RELEASE_TAG = "4.5-dev3"
DOWNLOAD_URL = f"https://github.com/godotengine/godot-builds/releases/download/{RELEASE_TAG}/Godot_v{RELEASE_TAG}_export_templates.tpz"


def version_folder(repo_root):
    v = {}
    exec((repo_root / "version.py").read_text(encoding="utf-8"), v)
    number = f"{v['major']}.{v['minor']}"
    if v["patch"]:
        number += f".{v['patch']}"
    folder = f"{number}.{v['status']}"
    if v["module_config"]:
        folder += f".{v['module_config']}"
    return folder


def templates_base_dir():
    if sys.platform == "win32":
        return Path(os.environ["APPDATA"]) / "Godot" / "export_templates"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "Godot" / "export_templates"
    xdg = os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local" / "share"))
    return Path(xdg) / "godot" / "export_templates"


def download(url, dest_file):
    def report(blocks, block_size, total):
        done = blocks * block_size
        if total > 0 and blocks % 2000 == 0:
            print(f"  {done / 1e6:.0f} / {total / 1e6:.0f} MB", flush=True)

    print(f"Downloading {url}")
    urllib.request.urlretrieve(url, dest_file, reporthook=report)


def extract_tpz(tpz_path, dest_dir):
    # The .tpz is a zip with a single top-level "templates/" directory;
    # the editor expects its contents directly inside the version folder.
    with zipfile.ZipFile(tpz_path) as zf:
        for entry in zf.namelist():
            parts = entry.split("/", 1)
            if len(parts) < 2 or not parts[1] or entry.endswith("/"):
                continue
            target = dest_dir / parts[1]
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(entry) as src, open(target, "wb") as out:
                out.write(src.read())


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--force", action="store_true", help="reinstall even if templates are already present")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parents[2]
    folder = version_folder(repo_root)
    dest_dir = templates_base_dir() / folder

    if dest_dir.exists() and any(dest_dir.iterdir()) and not args.force:
        print(f"Templates already installed at {dest_dir} (use --force to reinstall).")
        return 0

    dest_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tpz_path = Path(tmp) / "templates.tpz"
        download(DOWNLOAD_URL, tpz_path)
        print(f"Extracting to {dest_dir}")
        extract_tpz(tpz_path, dest_dir)

    print(f"Installed {RELEASE_TAG} templates as {folder}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
