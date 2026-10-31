#!/usr/bin/env python3
"""Stage the Roadster release layout.

    ./build.sh             # (in WSL) builds out/Roadster
    python3 tools/mkpkg.py

Produces out/release/ with Roadster.readme, the Roadster drawer
(Roadster + its tool icon, Install_Roadster + its icon, LICENSE) and
Roadster.info (the plain drawer icon) beside the drawer, so the drawer shows
on Workbench; the installer swaps it for the system's own drawer icon. No
other drawer icons. Archive it with LhA to keep the protection bits
(Roadster needs +e).
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REL = os.path.join(ROOT, "out", "release")

# (source relative to the repo root, path inside out/release)
FILES = [
    ("Roadster.readme",            "Roadster.readme"),
    ("out/Roadster",               "Roadster/Roadster"),
    ("icons/Roadster.info",        "Roadster/Roadster.info"),
    ("Install_Roadster",           "Roadster/Install_Roadster"),
    ("icons/Install_Roadster.info", "Roadster/Install_Roadster.info"),
    ("LICENSE",                    "Roadster/LICENSE"),
    ("icons/Roadster-drawer.info", "Roadster.info"),
]


def main():
    missing = [s for s, _ in FILES if not os.path.isfile(os.path.join(ROOT, s))]
    if missing:
        sys.exit("missing (run build.sh first?): " + ", ".join(missing))
    if os.path.isdir(REL):
        shutil.rmtree(REL)
    for src, dst in FILES:
        d = os.path.join(REL, dst)
        os.makedirs(os.path.dirname(d), exist_ok=True)
        shutil.copyfile(os.path.join(ROOT, src), d)
    for dirpath, _, names in os.walk(REL):
        for n in sorted(names):
            p = os.path.join(dirpath, n)
            print("%7d  %s" % (os.path.getsize(p), os.path.relpath(p, REL)))


if __name__ == "__main__":
    main()
