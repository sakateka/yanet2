#!/usr/bin/env bash
# One-command entry point for the pdump BEFORE/AFTER capture benchmark on a
# bare Linux box.
#
# Installs Nix with the official multi-user installer if it is missing
# (asks first, needs sudo), enters the devShell pinned in
# modules/pdump/tests/ab/devshell and runs pdump-ab.sh there. Flakes are
# enabled through command-line flags only; nix.conf is never edited.

set -euo pipefail

NIX_INSTALL_URL=https://nixos.org/nix/install
NIX_DAEMON_PROFILE=${NIX_DAEMON_PROFILE:-/nix/var/nix/profiles/default/etc/profile.d/nix-daemon.sh}
NIX_FLAGS=(--extra-experimental-features 'nix-command flakes')

usage() {
	cat <<'EOF'
Usage: modules/pdump/tests/ab/pdump-ab-nix.sh [--yes] [--dry-run] [pdump-ab.sh options]

Runs modules/pdump/tests/ab/pdump-ab.sh inside the pinned Nix devShell in
modules/pdump/tests/ab/devshell. If Nix is not installed, installs it first
with the official multi-user installer (needs sudo, asks for confirmation).

  --yes      install Nix without asking (the installer runs with --yes too)
  --dry-run  print the Nix installer and devShell commands instead of
             running them
  -h, --help this help and pdump-ab.sh's help

Every other argument (e.g. --cpus, --quick) is passed to pdump-ab.sh.
EOF
}

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
DEVSHELL="$ROOT/modules/pdump/tests/ab/devshell"
RUN="$ROOT/modules/pdump/tests/ab/pdump-ab.sh"

YES=0
DRY_RUN=0
RUN_ARGS=()
while (($#)); do
	case "$1" in
	--yes | -y) YES=1 ;;
	--dry-run) DRY_RUN=1 ;;
	-h | --help)
		usage
		echo
		"$RUN" --help
		exit 0
		;;
	*) RUN_ARGS+=("$1") ;;
	esac
	shift
done

err() {
	echo "pdump-ab-nix: $*" >&2
}

# Make an installed but not yet sourced Nix visible (a shell started before
# the install, or one that does not read /etc/profile.d).
load_nix_profile() {
	if [[ -r $NIX_DAEMON_PROFILE ]]; then
		# The profile script reads variables that may be unset, and skips
		# itself when this guard is inherited from a shell that sourced it.
		unset __ETC_PROFILE_NIX_SOURCED
		set +u
		# shellcheck source=/dev/null
		. "$NIX_DAEMON_PROFILE"
		set -u
	fi
}

install_nix() {
	local installer_args=(--daemon)
	((YES)) && installer_args+=(--yes)
	if [[ $(uname -s) != Linux ]]; then
		err "Nix is not installed and this script only installs it on Linux"
		exit 1
	fi
	cat <<EOF
Nix is not installed. The benchmark runs in a pinned Nix devShell, so this
script can install Nix with the official multi-user installer:

    sh <(curl --proto '=https' --tlsv1.2 -L $NIX_INSTALL_URL) --daemon

The installer uses sudo to create /nix, the nixbld build users and the
nix-daemon service, and adds Nix to the shell profiles in /etc. It does not
touch anything else; see https://nixos.org/download/ for details. Flakes are
enabled for this run only, through command-line flags.
EOF
	if ((DRY_RUN)); then
		echo
		echo "dry run, would run: sh <(curl --proto '=https' --tlsv1.2 -sSfL $NIX_INSTALL_URL) ${installer_args[*]}"
		return
	fi
	if ((EUID != 0)) && ! command -v sudo >/dev/null; then
		err "the Nix installer needs sudo, which is not available; install Nix as root, then rerun"
		exit 1
	fi
	if ! command -v curl >/dev/null; then
		err "curl is needed to download the Nix installer; install curl, then rerun"
		exit 1
	fi
	if ((!YES)); then
		if [[ ! -t 0 ]]; then
			err "no terminal to confirm the Nix install; rerun with --yes to install without asking"
			exit 1
		fi
		local answer
		read -r -p "Install Nix now? [y/N] " answer
		case "$answer" in
		[yY] | [yY][eE][sS]) ;;
		*)
			err "Nix install declined; install Nix yourself or rerun with --yes"
			exit 1
			;;
		esac
	fi
	# Download first so a network failure is reported as such rather than
	# as an empty script run by sh.
	local installer rc=0
	installer=$(mktemp)
	if ! curl --proto '=https' --tlsv1.2 -sSfL -o "$installer" "$NIX_INSTALL_URL"; then
		rm -f "$installer"
		err "cannot download the Nix installer from $NIX_INSTALL_URL"
		exit 1
	fi
	sh "$installer" "${installer_args[@]}" || rc=$?
	rm -f "$installer"
	if ((rc != 0)); then
		err "the Nix installer failed with exit code $rc (see its output above)"
		exit 1
	fi
	load_nix_profile
	if ! command -v nix >/dev/null; then
		err "Nix was installed, but nix is not on PATH after sourcing $NIX_DAEMON_PROFILE; open a new login shell and rerun"
		exit 1
	fi
}

if ! command -v nix >/dev/null; then
	load_nix_profile
fi
if ! command -v nix >/dev/null; then
	install_nix
fi

# Start from a clean environment so no host compiler, library or
# pkg-config path leaks into the build; keep only identity, locale, proxy
# settings and the benchmark's own knobs.
KEEP=(HOME USER LOGNAME TERM LANG LC_ALL TZ
	http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY no_proxy NO_PROXY
	GOPROXY GOPRIVATE GONOSUMDB GOMAXPROCS)
while IFS= read -r var; do
	KEEP+=("$var")
done < <(compgen -v PDUMP_BENCH_ || true)
KEEP_FLAGS=()
for var in "${KEEP[@]}"; do
	KEEP_FLAGS+=(--keep "$var")
done

DEVELOP=(nix "${NIX_FLAGS[@]}" develop --ignore-environment "${KEEP_FLAGS[@]}"
	"path:$DEVSHELL" -c "$RUN" "${RUN_ARGS[@]}")

if ((DRY_RUN)); then
	echo "dry run, would run in $ROOT:"
	printf ' %q' "${DEVELOP[@]}"
	echo
	exit 0
fi

if ! nix "${NIX_FLAGS[@]}" --version >/dev/null 2>&1; then
	err "'nix --version' fails; check the Nix install (flakes need Nix 2.4 or newer)"
	exit 1
fi
echo "pdump-ab-nix: $(nix --version); entering the devShell (the first run downloads the toolchain)"
cd "$ROOT"
exec "${DEVELOP[@]}"
