#!/usr/bin/env python3
"""Check that every package changed since a base revision has a higher version.

Upstream's check-changed-versions compares against upstream pmaports, where
none of this overlay's packages exist, so it only sees new packages. This
check applies the same rule to the overlay's own history instead: a package
whose files changed since BASE must have a higher pkgver/pkgrel than at BASE.
New packages are fine at any version.

Usage: scripts/check-versions.py BASE
Needs pmbootstrap's "pmb" module on PYTHONPATH (for apk version comparison).
"""

import subprocess
import sys
from pathlib import Path

import pmb.parse.version

REPO = Path(__file__).resolve().parent.parent


def git(*args: str) -> str:
    return subprocess.run(
        ["git", "-C", str(REPO), *args], check=True, capture_output=True, text=True
    ).stdout


def version(apkbuild: str) -> str:
    """pkgver-rpkgrel of an APKBUILD given as text (APKBUILDs are shell)."""
    out = subprocess.run(
        ["sh", "-c", 'eval "$1" >/dev/null 2>&1; echo "$pkgver-r$pkgrel"', "sh", apkbuild],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    base = sys.argv[1]

    # Same escape hatch as upstream, for changes that do not affect the built
    # package (comments, maintainer lines).
    if "[ci:skip-vercheck]" in git("log", "--pretty=format:%B", f"{base}..HEAD"):
        print("WARNING: not checking package versions ([ci:skip-vercheck])")
        return 0

    changed = git("diff", "--name-only", base, "--", "pmaports").splitlines()
    changed += git("ls-files", "--others", "--exclude-standard", "--", "pmaports").splitlines()
    pkgdirs = set()
    for path in changed:
        parts = Path(path).parent
        while parts != Path("pmaports") and parts != Path("."):
            if (REPO / parts / "APKBUILD").exists() or git("ls-tree", base, f"{parts}/APKBUILD"):
                pkgdirs.add(parts)
                break
            parts = parts.parent

    error = False
    for pkgdir in sorted(pkgdirs):
        name = pkgdir.relative_to("pmaports")
        head_file = REPO / pkgdir / "APKBUILD"
        if not head_file.exists():
            print(f"- {name}: removed")
            continue
        head = version(head_file.read_text())
        try:
            old = version(git("show", f"{base}:{pkgdir}/APKBUILD"))
        except subprocess.CalledProcessError:
            print(f"- {name}: {head} (new package)")
            continue
        result = pmb.parse.version.compare(head, old)
        ok = result == 1
        error |= not ok
        sign = {1: ">", 0: "==", -1: "<"}[result]
        print(f"- {name}: {head} {sign} {old} ({base[:12]}){'' if ok else ' [ERROR]'}")

    if error:
        print("ERROR: changed packages need a higher pkgver or pkgrel than at the base revision.")
        return 1
    print("all changed packages have a higher version")
    return 0


if __name__ == "__main__":
    sys.exit(main())
