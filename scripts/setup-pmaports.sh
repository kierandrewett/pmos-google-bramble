#!/bin/sh
# Create a pmaports checkout with the google-bramble packages from this
# repository added, ready for "pmbootstrap init".
#
# Usage: scripts/setup-pmaports.sh [TARGET_DIR]
#   TARGET_DIR defaults to ./build/pmaports (inside this repository, ignored
#   by git). It must not be ./pmaports, which holds the packages to copy.
#
# Environment:
#   PMAPORTS_URL     pmaports git URL (default: upstream on GitLab)
#   PMAPORTS_COMMIT  commit to check out (default: the one this port was
#                    tested against). Set to "main" to try the latest.
set -eu
export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"

repo=$(cd "$(dirname "$0")/.." && pwd)
target=${1:-"$repo/build/pmaports"}
url=${PMAPORTS_URL:-https://gitlab.postmarketos.org/postmarketOS/pmaports.git}

case "$target" in
"$repo/pmaports" | "$repo/pmaports/" | pmaports | pmaports/ | ./pmaports | ./pmaports/)
	echo "[setup] TARGET_DIR must not be $repo/pmaports: that holds the packages to copy" >&2
	exit 1
	;;
esac
commit=${PMAPORTS_COMMIT:-9e19dd8c837d946a86505fdf879882a1d8857d42}

if [ -e "$target/.git" ]; then
	echo "[setup] using existing checkout: $target"
else
	echo "[setup] cloning $url into $target"
	git clone --filter=blob:none "$url" "$target"
fi

cd "$target"

# Package directories this repository provides, relative to pmaports.
pkgdirs=$(cd "$repo/pmaports" && find . -name APKBUILD -exec dirname {} \; | sed 's|^\./||' | sort)
manifest="$(git rev-parse --absolute-git-dir)/bramble-sync.sha256"

in_pkgdirs() {
	for d in $pkgdirs; do
		case "$1" in "$d"/*) return 0 ;; esac
	done
	return 1
}

# Only files written by an earlier run of this script may differ from the
# checkout, and only if nobody edited them since: they are replaced below.
# Anything else is someone's work, so stop instead of overwriting it.
if [ -e "$manifest" ] && ! sha256sum -c --quiet "$manifest" > /dev/null 2>&1; then
	echo "[setup] files copied by an earlier run were changed in $target:" >&2
	sha256sum -c --quiet "$manifest" 2>&1 | sed 's/^/  /' >&2
	echo "[setup] make the change in $repo/pmaports instead, or commit/stash it" >&2
	exit 1
fi
others=$(git status --porcelain --untracked-files=all | cut -c4- | while read -r p; do
	if in_pkgdirs "$p" && { [ ! -e "$p" ] || grep -qF "  ./$p" "$manifest" 2> /dev/null; }; then
		continue
	fi
	echo "$p"
done)
if [ -n "$others" ]; then
	echo "[setup] $target has local changes; commit or stash them first:" >&2
	echo "$others" | sed 's/^/  /' >&2
	exit 1
fi
if git rev-parse --verify --quiet google-bramble > /dev/null &&
	[ "$(git rev-parse google-bramble)" != "$(git rev-parse HEAD)" ]; then
	echo "[setup] branch google-bramble exists but is not checked out; delete or rename it first" >&2
	exit 1
fi

# Put the package directories back to the checkout's state, then move to
# the pinned commit.
for d in $pkgdirs; do
	git checkout --quiet HEAD -- "$d" 2> /dev/null || true
	git clean --quiet -fdx -- "$d"
done
git fetch --quiet origin
pin=$(git rev-parse --verify --quiet "$commit^{commit}" ||
	git rev-parse --verify --quiet "origin/$commit^{commit}") || {
	echo "[setup] commit $commit not found in $target" >&2
	exit 1
}
if [ "$(git symbolic-ref --quiet --short HEAD)" = google-bramble ] &&
	[ -n "$(git rev-list "$pin..HEAD")" ]; then
	echo "[setup] branch google-bramble has commits on top of $commit; move them to another branch first" >&2
	exit 1
fi
git checkout --quiet -B google-bramble "$pin"
echo "[setup] pmaports at $(git rev-parse --short HEAD) on branch google-bramble"

# Replace each package directory with the one from this repository, so
# files removed here are removed there too. Files are copied, not linked,
# so pmbootstrap sees a normal checkout.
for d in $pkgdirs; do
	rm -rf "$d"
	mkdir -p "$(dirname "$d")"
	cp -R "$repo/pmaports/$d" "$d"
done
(cd "$target" && for d in $pkgdirs; do find "./$d" -type f; done | sort | xargs sha256sum) > "$manifest"
echo "[setup] packages from $repo/pmaports:"
echo "$pkgdirs" | sed 's|^|  |'

cat <<EOF

[setup] done. Next:
  pmbootstrap --aports "$target" init
See docs/install.md for the answers to give.
EOF
