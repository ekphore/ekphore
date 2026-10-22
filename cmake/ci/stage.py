#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later

"""Collect normal CMake plugin bundles and the standalone executable."""
import argparse
from pathlib import Path
import plistlib
import shutil
import sys


def find_bundle(build, name):
    matches = [p for p in build.rglob(name) if "_deps" not in p.parts]
    # on Windows the VST3 binary has the same name as its enclosing bundle
    candidates = set(matches)
    matches = [p for p in matches if not any(parent in candidates for parent in p.parents)]
    if len(matches) != 1:
        raise RuntimeError(f"Expected one {name}, found {matches}")
    return matches[0]


def copy(source, destination):
    if source.is_dir():
        shutil.copytree(source, destination, symlinks=True)
    else:
        shutil.copy2(source, destination)


def stage(build, output, formats, dependency_prefix=None, standalone=False):
    if output.exists() and any(output.iterdir()):
        raise RuntimeError(f"Output directory must be empty: {output}")
    names = {"clap": "ekphore-clap.clap", "lv2": "ekphore.lv2",
             "vst3": "ekphore.vst3", "au": "ekphore.component"}
    bundles = [(find_bundle(build, names[kind]),
                output / f"ekphore.{kind if kind != 'au' else 'component'}")
               for kind in formats]
    daemon = "nksd.exe" if any(build.rglob("nksd.exe")) else "nksd"
    daemon_source = find_bundle(build, daemon)
    macos = sys.platform == 'darwin'
    if standalone:
        executable = "ekphore-ui.exe" if daemon.endswith(".exe") else "ekphore-ui"
        destination = (output / 'Ekphore.app/Contents/MacOS' / executable
                       if macos else output / executable)
        bundles.append((find_bundle(build, executable), destination))
    root = Path(__file__).resolve().parents[2]
    output.mkdir(parents=True, exist_ok=True)
    for source, destination in bundles:
        destination.parent.mkdir(parents=True, exist_ok=True)
        copy(source, destination)
        if macos and destination.is_dir():
            helpers = destination / 'Contents/Helpers'
            helpers.mkdir(parents=True, exist_ok=True)
            copy(daemon_source, helpers / 'nksd')
    if macos and standalone:
        contents = output / 'Ekphore.app/Contents'
        (contents / 'Helpers').mkdir()
        copy(daemon_source, contents / 'Helpers/nksd')
        (contents / 'Resources').mkdir()
        shutil.copy2(root / 'resources/icons/ekphore.icns', contents / 'Resources/ekphore.icns')
        with (contents / 'Info.plist').open('wb') as stream:
            plistlib.dump({
                'CFBundleDevelopmentRegion': 'en',
                'CFBundleDisplayName': 'Ekphore',
                'CFBundleExecutable': 'ekphore-ui',
                'CFBundleIconFile': 'ekphore',
                'CFBundleIdentifier': 'io.github.ekphore',
                'CFBundleName': 'Ekphore',
                'CFBundlePackageType': 'APPL',
                'CFBundleShortVersionString': '0.1.0',
                'CFBundleVersion': '1',
                'NSHighResolutionCapable': True,
            }, stream)
    elif not macos:
        copy(daemon_source, output / daemon)
    if standalone and not macos and daemon == "nksd":
        # freedesktop layout, so the archive's share/ can go straight into ~/.local/share
        share = output / 'share'
        (share / 'applications').mkdir(parents=True)
        shutil.copy2(root / 'resources/linux/io.github.ekphore.desktop', share / 'applications')
        for size, name in (('256x256', 'ekphore.png'), ('scalable', 'ekphore.svg')):
            icons = share / 'icons/hicolor' / size / 'apps'
            icons.mkdir(parents=True)
            shutil.copy2(root / 'resources/icons' / name,
                         icons / ('io.github.ekphore' + Path(name).suffix))
    shutil.copy2(root / "LICENSE", output / "LICENSE")
    licenses = output / "licenses"
    for directory in (build / "_deps").glob("*-src"):
        for source in directory.rglob("*"):
            if not source.is_file() or not source.name.upper().startswith(
                    ("LICENSE", "COPYING", "COPYRIGHT", "NOTICE", "PATENTS")):
                continue
            destination = licenses / directory.name / source.relative_to(directory)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
    for name in ("native-grain-fftw/source/COPYING", "fftw/fftw-3.3.9/COPYING"):
        fftw = build / name
        if fftw.exists():
            (licenses / "fftw").mkdir(parents=True, exist_ok=True)
            shutil.copy2(fftw, licenses / "fftw/COPYING")
    (licenses / "opensans").mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / "assets/fonts/OFL.txt", licenses / "opensans/OFL.txt")
    # the logo's ep mark is drawn from Bravura glyphs
    (licenses / "bravura").mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / "resources/icons/BRAVURA-OFL.txt", licenses / "bravura/OFL.txt")
    if dependency_prefix:
        for source in (dependency_prefix / "share").glob("*/copyright"):
            destination = licenses / "vcpkg" / source.parent.name / "copyright"
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--formats", nargs="+", required=True,
                        choices=("clap", "lv2", "vst3", "au"))
    parser.add_argument("--dependency-prefix", type=Path)
    parser.add_argument("--standalone", action="store_true")
    args = parser.parse_args()
    stage(args.build.resolve(), args.output.resolve(), args.formats,
          args.dependency_prefix, args.standalone)
