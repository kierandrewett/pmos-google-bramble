#!/bin/sh
# Run the pmaports CI checks against this overlay, the way upstream runs them
# locally: "pmbootstrap ci <script>" in a pmaports checkout.
#
# The checkout is build/pmaports: the pinned upstream commit, then a commit
# with the overlay as it was at the base revision, then a commit with the
# working-tree overlay. The upstream scripts see the overlay changes as a merge
# request.
#
# Upstream's check-changed-versions compares versions with upstream pmaports,
# where none of these packages exist, so it is replaced by "versions"
# (scripts/check-versions.py): changed packages must have a higher version
# than at the base revision of this repository.
#
# pmbootstrap runs with its own config and work dir under build/ (some checks
# run "pmbootstrap config ..."), so your normal pmbootstrap setup is not
# touched. Logs go to build/ci-logs/.
#
# Usage: scripts/ci.sh [CHECK...]
#   With no arguments every check runs. Checks: shellcheck grep ec
#   apkbuild-lint dint pytest versions kconfig verify-checksums gitleaks
#
# Environment:
#   PMBOOTSTRAP  path to pmbootstrap.py (default: ../pmbootstrap/pmbootstrap.py,
#                else a clone in build/pmbootstrap of the pinned tag below)
#   PMB_SUDO     sudo command for pmbootstrap (it needs root for its chroots)
#   CI_BASE      revision the versions check compares against (default:
#                merge-base of HEAD and origin/main, else HEAD~1)
#   PMAPORTS_COMMIT, PMAPORTS_URL: see scripts/setup-pmaports.sh
set -u
export PATH="/usr/local/bin:/usr/bin:/bin:$PATH"

repo=$(cd "$(dirname "$0")/.." && pwd)
build="$repo/build"
target="$build/pmaports"
logs="$build/ci-logs"
all_checks="shellcheck grep ec apkbuild-lint dint pytest versions kconfig verify-checksums gitleaks"
checks=${*:-$all_checks}

PMBOOTSTRAP_TAG=3.12.1
GITLEAKS_VERSION=8.28.0
GITLEAKS_SHA256=a65b5253807a68ac0cafa4414031fd740aeb55f54fb7e55f386acb52e6a840eb

for c in $checks; do
	case " $all_checks " in
	*" $c "*) ;;
	*)
		echo "[ci] unknown check: $c" >&2
		exit 2
		;;
	esac
done

mkdir -p "$build/bin" "$logs" || exit 1
rm -f "$logs"/*.log

# pmbootstrap: the upstream scripts call "pmbootstrap" from PATH and import
# its "pmb" module.
PMBOOTSTRAP=${PMBOOTSTRAP:-"$repo/../pmbootstrap/pmbootstrap.py"}
if [ ! -f "$PMBOOTSTRAP" ]; then
	PMBOOTSTRAP="$build/pmbootstrap/pmbootstrap.py"
	if [ ! -f "$PMBOOTSTRAP" ]; then
		git clone -q --depth 1 --branch "$PMBOOTSTRAP_TAG" \
			https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git "$build/pmbootstrap" || exit 1
	fi
fi
PMBOOTSTRAP=$(cd "$(dirname "$PMBOOTSTRAP")" && pwd)/pmbootstrap.py
cat > "$build/bin/pmbootstrap" << EOF
#!/bin/sh
exec python3 "$PMBOOTSTRAP" "\$@"
EOF
chmod +x "$build/bin/pmbootstrap"
PYTHONPATH="$(dirname "$PMBOOTSTRAP")${PYTHONPATH:+:$PYTHONPATH}"
export PYTHONPATH

# pytest for the pytest check, in a venv so the host stays untouched.
if ! command -v pytest > /dev/null && [ ! -x "$build/venv/bin/pytest" ]; then
	python3 -m venv "$build/venv" && "$build/venv/bin/pip" -q install pytest || exit 1
fi
PATH="$build/bin:$build/venv/bin:$PATH"
export PATH

# Isolated pmbootstrap config (found through XDG_CONFIG_HOME) and work dir.
export XDG_CONFIG_HOME="$build/ci-config"
mkdir -p "$XDG_CONFIG_HOME" "$build/ci-work" || exit 1
cat > "$XDG_CONFIG_HOME/pmbootstrap_v3.cfg" << EOF
[pmbootstrap]
aports = $target
work = $build/ci-work
device = google-bramble
ui = none
service_manager = openrc
is_default_channel = False

[providers]

[mirrors]
EOF

# A fresh work dir needs the version marker that "pmbootstrap init" writes.
if [ ! -e "$build/ci-work/version" ]; then
	python3 -c 'import pmb.config; print(pmb.config.work_version)' > "$build/ci-work/version" || exit 1
fi

# Base revision: the overlay's own history, not upstream.
base_rev=${CI_BASE:-}
if [ -z "$base_rev" ]; then
	if git -C "$repo" rev-parse --verify -q origin/main > /dev/null; then
		base_rev=$(git -C "$repo" merge-base HEAD origin/main)
	else
		base_rev=$(git -C "$repo" rev-parse --verify -q HEAD~1 || true)
	fi
fi

# Recreate the checkout on every run: the commits below are throwaway.
pkgdirs=$(cd "$repo/pmaports" && find . -name APKBUILD -exec dirname {} \; | sed 's|^\./||' | sort)
rm -rf "$target"
sh "$repo/scripts/setup-pmaports.sh" "$target" > "$logs/setup.log" 2>&1 || {
	cat "$logs/setup.log"
	exit 1
}
cd "$target" || exit 1
upstream=$(git rev-parse HEAD)
commit() {
	git -c user.name=ci -c user.email=ci@localhost -c commit.gpgsign=false \
		commit -q --allow-empty -m "$1"
}
# setup-pmaports.sh copied the working-tree packages. Replace them with the
# base revision first, so the overlay commit is a real diff.
base_pkgdirs=$pkgdirs
if [ -n "$base_rev" ] && git -C "$repo" cat-file -e "$base_rev:pmaports" 2> /dev/null; then
	base_pkgdirs=$(git -C "$repo" ls-tree -r --name-only "$base_rev:pmaports" |
		grep '/APKBUILD$' | xargs -n1 dirname | sort -u)
	# shellcheck disable=SC2086
	rm -rf $pkgdirs $base_pkgdirs
	git -C "$repo" archive "$base_rev:pmaports" | tar -x -C "$target" || exit 1
	# shellcheck disable=SC2086
	git add -A -- $base_pkgdirs
	commit "overlay at base $(git -C "$repo" rev-parse --short "$base_rev")" || exit 1
fi
CI_MERGE_REQUEST_DIFF_BASE_SHA=$(git rev-parse HEAD)
export CI_MERGE_REQUEST_DIFF_BASE_SHA
# shellcheck disable=SC2086
rm -rf $pkgdirs
(cd "$repo/pmaports" && tar -cf - $pkgdirs) | tar -x -C "$target" || exit 1
# shellcheck disable=SC2086
git add -A -- $pkgdirs $base_pkgdirs
commit "bramble overlay for CI" || exit 1
echo "[ci] checkout $(git rev-parse --short HEAD), base $(git rev-parse --short "$CI_MERGE_REQUEST_DIFF_BASE_SHA"), upstream $(git rev-parse --short "$upstream")"

# Waivers. Each one only applies when it is the only complaint left.
#
# grep: device-google-bramble/initfs.files maps the modprobe.d file to
# /etc/modprobe.d in the initramfs. That must stay: busybox modprobe in the
# initramfs only reads /etc/modprobe.d.
grep_waiver='device/downstream/device-google-bramble/initfs.files:/usr/lib/modprobe.d/device-google-bramble.conf:/etc/modprobe.d/device-google-bramble.conf'
# dint: two deviceinfo keys the schema no longer lists, which the installed
# tools still read. deviceinfo_kernel_cmdline: boot-deploy uses it instead of
# /usr/lib/kernel-cmdline.d, which would add "quiet splash" and plymouth
# options to the command line. deviceinfo_initfs_extra_compression: mkinitfs
# reads it; initramfs-extra must stay gzip, the only format stage 1 unpacks.
dint_waivers="deviceinfo_kernel_cmdline deviceinfo_initfs_extra_compression"

strip_colour() {
	esc=$(printf '\033')
	sed "s/$esc\[[0-9;]*[mK]//g" "$1"
}

gitleaks_check() {
	tools="$build/tools"
	bin="$tools/gitleaks-$GITLEAKS_VERSION"
	if [ ! -x "$bin" ]; then
		mkdir -p "$tools" || return 1
		tgz="$tools/gitleaks.tar.gz"
		curl -fsSL -o "$tgz" "https://github.com/gitleaks/gitleaks/releases/download/v$GITLEAKS_VERSION/gitleaks_${GITLEAKS_VERSION}_linux_x64.tar.gz" || return 1
		echo "$GITLEAKS_SHA256  $tgz" | sha256sum -c - || return 1
		tar -xzf "$tgz" -C "$tools" gitleaks && mv "$tools/gitleaks" "$bin" && rm -f "$tgz" || return 1
	fi
	cfg="$tools/gitleaks-tree.toml"
	cat > "$cfg" << 'EOF'
[extend]
useDefault = true

[allowlist]
description = "build output and git metadata"
paths = ['''^build/''', '''^\.git/''']
EOF
	rc=0
	echo "[gitleaks] git history"
	(cd "$repo" && "$bin" git --no-banner --redact --exit-code 1 .) || rc=1
	echo "[gitleaks] working tree (build/ excluded)"
	(cd "$repo" && "$bin" dir --no-banner --redact --exit-code 1 -c "$cfg" .) || rc=1
	return "$rc"
}

run_check() {
	name=$1
	log="$logs/$name.log"
	echo "[ci] $name"
	case "$name" in
	gitleaks) gitleaks_check > "$log" 2>&1 ;;
	versions)
		if [ -n "$base_rev" ]; then
			python3 "$repo/scripts/check-versions.py" "$base_rev" > "$log" 2>&1
		else
			echo "no base revision: nothing to compare" > "$log"
		fi
		;;
	# grep.sh only needs git and grep: run it directly.
	grep) (cd "$target" && sh .ci/grep.sh) > "$log" 2>&1 ;;
	*)
		# "native" scripts run on the host anyway; run them directly so they
		# get CI_MERGE_REQUEST_DIFF_BASE_SHA (pmbootstrap ci drops it). The
		# others run in a pmbootstrap chroot that installs their tools.
		if grep -q '^# Options:.*native' "$target/.ci/$name.sh"; then
			(cd "$target" && sh ".ci/$name.sh") > "$log" 2>&1
		else
			(cd "$target" && pmbootstrap ci "$name") > "$log" 2>&1
		fi
		;;
	esac
	rc=$?

	if [ "$rc" -ne 0 ] && [ "$name" = grep ]; then
		bad=$(strip_colour "$log" | grep -vF -e "$grep_waiver" -e 'following files:' | grep -cE '^[^+[:space:]]')
		if [ "$bad" = 0 ] && strip_colour "$log" | grep -qF "$grep_waiver"; then
			echo "[ci] waived: modprobe.d mapping in initfs.files (see scripts/ci.sh)" >> "$log"
			rc=0
		fi
	fi
	if [ "$rc" -ne 0 ] && [ "$name" = dint ]; then
		errors=$(strip_colour "$log" | sed -n 's/^\[Error!!!\] \([a-z_]*\):.*/\1/p')
		left=$(for e in $errors; do
			case " $dint_waivers " in *" $e "*) ;; *) echo "$e" ;; esac
		done)
		if [ -n "$errors" ] && [ -z "$left" ]; then
			echo "[ci] waived: $errors (see scripts/ci.sh)" >> "$log"
			rc=0
		fi
	fi

	if [ "$rc" -eq 0 ]; then
		echo "[ci] $name: ok"
	else
		echo "[ci] $name: FAILED (rc=$rc), see $log"
	fi
	return "$rc"
}

failed=""
for c in $checks; do
	run_check "$c" || failed="$failed $c"
done

if [ -n "$failed" ]; then
	echo "[ci] failed:$failed"
	exit 1
fi
echo "[ci] all checks passed"
