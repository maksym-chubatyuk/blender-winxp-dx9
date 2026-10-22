# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Install the pure Python packages Blender bundles into the XP build's site-packages, at the
versions pinned in `build_files/build_environment/cmake/versions.cmake`.

There is no pip for the cross-compiled Python: universal (`py3-none-any`) wheels are zip
archives, so they're fetched from PyPI and unpacked directly.

Usage: python3 site_packages.py <site-packages-dir>
"""

import json
import pathlib
import re
import sys
import urllib.request
import zipfile
import io

ROOT = pathlib.Path(__file__).resolve().parents[2]
VERSIONS = ROOT / "build_files" / "build_environment" / "cmake" / "versions.cmake"

# PyPI project name -> `versions.cmake` variable.
# Pure Python only: compiled packages (NumPy, zstandard) need a cross build.
PACKAGES = {
    "idna": "IDNA_VERSION",
    "charset-normalizer": "CHARSET_NORMALIZER_VERSION",
    "urllib3": "URLLIB3_VERSION",
    "certifi": "CERTIFI_VERSION",
    "requests": "REQUESTS_VERSION",
    "attrs": "ATTRS_VERSION",
    "cattrs": "CATTRS_VERSION",
    "fastjsonschema": "FASTJSONSCHEMA_VERSION",
    "typing-extensions": "TYPING_EXTENSIONS_VERSION",
    "tomli-w": "TOMLI_W_VERSION",
}


def pinned_versions() -> dict[str, str]:
    text = VERSIONS.read_text(encoding="utf-8")
    result = {}
    for name, var in PACKAGES.items():
        match = re.search(r"^set\(" + var + r" ([^ )]+)\)", text, re.MULTILINE)
        if not match:
            sys.exit("{:s} not found in {:s}".format(var, str(VERSIONS)))
        result[name] = match.group(1)
    return result


def universal_wheel_url(name: str, version: str) -> str:
    with urllib.request.urlopen("https://pypi.org/pypi/{:s}/{:s}/json".format(name, version)) as fh:
        info = json.load(fh)
    for file in info["urls"]:
        if file["packagetype"] == "bdist_wheel" and file["filename"].endswith("-none-any.whl"):
            return file["url"]
    sys.exit("no universal wheel for {:s}=={:s}".format(name, version))


def main() -> None:
    site_packages = pathlib.Path(sys.argv[1])
    site_packages.mkdir(parents=True, exist_ok=True)
    for name, version in pinned_versions().items():
        url = universal_wheel_url(name, version)
        with urllib.request.urlopen(url) as fh:
            data = fh.read()
        with zipfile.ZipFile(io.BytesIO(data)) as wheel:
            wheel.extractall(site_packages)
        print("{:s}=={:s}".format(name, version))


if __name__ == "__main__":
    main()
