#!/bin/sh
# Assessment Agent (Linux) — installer.
#
# Single entry point. Privilege model differs by init system:
#
#   - systemd hosts (CentOS/RHEL 7+, Ubuntu, Debian, SLES, Amazon Linux):
#       USER-LEVEL install. Run as a normal user, NO root/sudo:
#           ./deploy/install.sh
#       Everything lands under the invoking user's home:
#           ~/.local/bin/assessment-agent
#           ${XDG_CONFIG_HOME:-~/.config}/assessment-agent/agent.env{,.local}
#           ${XDG_STATE_HOME:-~/.local/state}/assessment-agent/
#           ${XDG_CONFIG_HOME:-~/.config}/systemd/user/assessment-agent.service
#       Managed via `systemctl --user`. Boot-without-login needs lingering
#       (loginctl enable-linger) — attempted best-effort, never fatal.
#
#   - SysV hosts (CentOS/RHEL/Oracle Linux 6 — no systemd):
#       SYSTEM install, requires root ONCE (no user-session supervisor exists
#       on pre-systemd hosts). Registers /etc/init.d/assessment-agent which
#       runs the binary as the non-privileged `assessment-agent` user — so the
#       24/7 runtime is still unprivileged; only the one-time registration
#       needs root. CentOS 6 needs the LEGACY glibc-2.12 binary
#       (make release-legacy).
#
# Re-runnable (idempotent). Each step short-circuits when already done.
#
# Flags (set via env, not argv — POSIX sh has no getopt without external help):
#   IMAGE_PREP=1     register service but do NOT start. Use before sealing a
#                    golden VM image; then run `assessment-agent prep-image`.
#   SKIP_SHA256=1    skip dist/SHA256SUMS verification (only when intentional).
#
# Server-side dependencies (sh + coreutils are guaranteed on every supported
# OS). systemctl is required ONLY on systemd hosts; useradd/groupadd/chown
# are required ONLY on the SysV (root) path — both are checked inside their
# own branch, not up front.

set -eu

# --- 0. Banner — image-clone caveat is the single biggest gotcha for
#        fleet operators, so it gets first billing.
printf '\n'
printf '=== Assessment Agent installer ===\n'
printf 'NOTE: If this server was cloned from a VM image, run\n'
printf '      `assessment-agent prep-image` on the golden image before\n'
printf '      snapshotting so clones get distinct identities.\n'
printf '\n'

# --- 1. Resolve paths
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
AGENT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)

# INSTALLER_SELF_PATH lets the self-installer subcommand (assessment-agent
# install) point DIST_BIN at /proc/self/exe so install.sh runs against the
# single-binary bundle without needing a dist/ directory on disk.
DIST_BIN="${INSTALLER_SELF_PATH:-$AGENT_ROOT/dist/assessment-agent-linux-x86_64}"
SHA_FILE="$AGENT_ROOT/dist/SHA256SUMS"
ENV_EXAMPLE="$SCRIPT_DIR/systemd/agent.env.example"
SERVICE_FILE="$SCRIPT_DIR/systemd/assessment-agent.service"
SYSV_FILE="$SCRIPT_DIR/sysv/assessment-agent"
SYSV_TARGET=/etc/init.d/assessment-agent

printf '[install] repo root : %s\n' "$AGENT_ROOT"

# --- 2. Init-system detection.
# systemd path requires systemctl AND a live systemd (pid 1). The /run/systemd
# /system dir presence is the canonical "booted under systemd" check; pidof
# systemd is the fallback. Everything else → SysV init (CentOS/RHEL 6).
INIT_SYSTEM=sysv
if command -v systemctl >/dev/null 2>&1; then
	if [ -d /run/systemd/system ] || pidof systemd >/dev/null 2>&1; then
		INIT_SYSTEM=systemd
	fi
fi
printf '[install] init system: %s\n' "$INIT_SYSTEM"

# --- 3. OS gate (lib/detect-os.sh) — shared by both paths.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/lib/detect-os.sh"
status=$(detect_os || true)
if [ "$status" != "ok" ]; then
	echo "[install] $status" >&2
	exit 1
fi
if [ -r /etc/os-release ]; then
	# shellcheck disable=SC1091
	. /etc/os-release
	printf '[install] OS         : %s-%s — supported\n' "${ID:-?}" "${VERSION_ID:-?}"
fi

# --- 4. Binary present (shared).
if [ ! -f "$DIST_BIN" ]; then
	echo "[install] binary missing: $DIST_BIN" >&2
	if [ "$INIT_SYSTEM" = "sysv" ]; then
		echo "[install] SysV/EL6 host detected — run 'make release-legacy' on a" >&2
		echo "[install]   manylinux2010 build host (glibc 2.12) and point DIST_BIN" >&2
		echo "[install]   at the legacy artifact (see docs/centos6-bringup.md)" >&2
	else
		echo "[install] run 'make release' on a manylinux2014 build host first" >&2
	fi
	exit 1
fi

# --- 5. SHA256 verify (shared). This is the build-host integrity gate the
#        prod box trusts.
verify_sha256() {
	if [ "${SKIP_SHA256:-0}" = "1" ]; then
		echo "[install] SKIP_SHA256=1 — skipping integrity check"
	elif [ -f "$SHA_FILE" ]; then
		(cd "$AGENT_ROOT/dist" && sha256sum -c SHA256SUMS) >/dev/null 2>&1 || {
			echo "[install] SHA256 mismatch — binary may be corrupt" >&2
			exit 1
		}
		echo "[install] SHA256 OK"
	else
		echo "[install] WARNING: $SHA_FILE missing — proceeding without integrity check" >&2
	fi
}

require_cmds() {
	for cmd in "$@"; do
		if ! command -v "$cmd" >/dev/null 2>&1; then
			echo "[install] missing required command: $cmd" >&2
			echo "[install]   apt:  apt-get install coreutils" >&2
			echo "[install]   yum:  yum install coreutils" >&2
			exit 1
		fi
	done
}

# ======================================================================
#  systemd path — USER-LEVEL (no root)
# ======================================================================
install_user_systemd() {
	require_cmds systemctl install chmod awk grep sed tr mktemp sha256sum

	# `systemctl --user` needs a running user manager reachable over the
	# user D-Bus. Over a bare SSH exec (no login session) XDG_RUNTIME_DIR is
	# often unset and the call fails with "Failed to connect to bus". Wire a
	# sane default so the common high-latency-SSH deploy path works.
	uid=$(id -u)
	if [ "$uid" = "0" ]; then
		echo "[install] running as root on a systemd host — this installer is" >&2
		echo "[install]   user-level by design. Re-run as the unprivileged user" >&2
		echo "[install]   that should own the agent (no sudo)." >&2
		exit 1
	fi
	: "${XDG_RUNTIME_DIR:=/run/user/$uid}"
	export XDG_RUNTIME_DIR

	CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/assessment-agent"
	STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/assessment-agent"
	BIN_DIR="$HOME/.local/bin"
	UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
	BIN_TARGET="$BIN_DIR/assessment-agent"
	UNIT_TARGET="$UNIT_DIR/assessment-agent.service"
	ENV_FILE="$CFG_DIR/agent.env"
	ENV_LOCAL="$CFG_DIR/agent.env.local"

	verify_sha256

	# Directories (0700 for config/state — they hold broker secrets + worker
	# results; 0755 for ~/.local/bin which is conventionally on PATH).
	install -d -m 0700 "$CFG_DIR" "$STATE_DIR"
	install -d -m 0755 "$BIN_DIR" "$UNIT_DIR"

	# Stop a running instance so the binary can be replaced (upgrade path).
	if systemctl --user is-active --quiet assessment-agent.service 2>/dev/null; then
		echo "[install] stopping running user service for upgrade..."
		systemctl --user stop assessment-agent.service || true
	fi

	# Binary
	install -m 0755 "$DIST_BIN" "$BIN_TARGET"
	echo "[install] binary     : $BIN_TARGET"

	# User unit — %h/%E/%S specifiers inside the unit resolve to this user's
	# home / config / state, so no path substitution is needed here.
	install -m 0644 "$SERVICE_FILE" "$UNIT_TARGET"

	# env file (seed from example on first run, then env-setup fills empties)
	if [ ! -f "$ENV_FILE" ]; then
		install -m 0640 "$ENV_EXAMPLE" "$ENV_FILE"
		echo "[install] seeded env : $ENV_FILE (from agent.env.example)"
	fi
	sh "$SCRIPT_DIR/lib/env-setup.sh" "$ENV_EXAMPLE" "$ENV_FILE" "$ENV_LOCAL"

	# agent.env.example ships WORKER_STATE_DIR=/var/lib/agent-worker — the SysV
	# system path. A user-level agent cannot write there, so worker_init() fails
	# and the worker silently stays off (collector-only). Rewrite it to this
	# user's own writable state dir so the value the installer lays down is
	# correct on its own, not only when the unit's %S override happens to apply.
	# (WORKER_TMP_DIR stays /tmp — every user can write there.)
	tmp_env=$(mktemp)
	sed "s|^[[:space:]]*WORKER_STATE_DIR=.*|WORKER_STATE_DIR=$STATE_DIR|" "$ENV_FILE" > "$tmp_env"
	install -m 0640 "$tmp_env" "$ENV_FILE"
	rm -f "$tmp_env"
	echo "[install] worker state: $STATE_DIR"

	systemctl --user daemon-reload

	# Boot-without-login: enable lingering. On most distros a user may enable
	# their own linger without root (polkit org.freedesktop.login1.set-self-
	# linger defaults to allow); where policy denies it, warn but continue —
	# the agent still starts on the next interactive login.
	if command -v loginctl >/dev/null 2>&1; then
		if loginctl enable-linger "$(id -un)" 2>/dev/null; then
			echo "[install] lingering enabled — agent will start at boot"
		else
			echo "[install] WARNING: could not enable lingering (no root / polkit denied)." >&2
			echo "[install]   The agent starts on your next login. For boot-without-login," >&2
			echo "[install]   ask an admin to run: loginctl enable-linger $(id -un)" >&2
		fi
	fi

	# image-prep mode: enable but do not start.
	if [ "${IMAGE_PREP:-0}" = "1" ]; then
		systemctl --user enable assessment-agent.service >/dev/null 2>&1 || true
		printf '\n'
		printf '[install] IMAGE_PREP=1 — service enabled but NOT started.\n'
		printf '[install] before sealing this VM into an image, run:\n'
		printf '[install]     assessment-agent prep-image\n'
		printf '\n'
		exit 0
	fi

	systemctl --user enable --now assessment-agent.service

	sleep 5
	if systemctl --user is-active --quiet assessment-agent.service; then
		printf '\n'
		echo "[install] OK — assessment-agent is active (user service)"
		echo "[install] logs:       journalctl --user -u assessment-agent -f"
		echo "[install] stop:       systemctl --user stop assessment-agent"
		echo "[install] uninstall:  assessment-agent uninstall"
		case ":$PATH:" in
			*":$BIN_DIR:"*) : ;;
			*) echo "[install] NOTE: $BIN_DIR is not on your PATH — add it to run 'assessment-agent' directly" ;;
		esac
	else
		echo "[install] WARNING: user service is not active. Last 30 log lines:" >&2
		journalctl --user -u assessment-agent.service -n 30 --no-pager 2>/dev/null || true
		exit 1
	fi
}

# ======================================================================
#  SysV path — SYSTEM install, root required ONCE (runtime stays
#  unprivileged via the init script's RUNAS=assessment-agent user)
# ======================================================================
install_sysv_root() {
	CFG_DIR=/etc/assessment-agent
	STATE_DIR=/var/lib/agent-worker
	ENV_FILE="$CFG_DIR/agent.env"
	ENV_LOCAL="$CFG_DIR/agent.env.local"

	if [ "$(id -u)" -ne 0 ]; then
		echo "[install] SysV/EL6 host: registering an /etc/init.d service needs" >&2
		echo "[install]   root ONCE. The agent itself then runs as the" >&2
		echo "[install]   unprivileged 'assessment-agent' user. Re-run with sudo." >&2
		exit 1
	fi

	require_cmds sha256sum useradd groupadd id install chmod chown awk grep sed tr getent mktemp
	verify_sha256

	# User/group (system, non-login) — the init script runs the agent as this.
	if ! getent group assessment-agent >/dev/null 2>&1; then
		groupadd --system assessment-agent
		echo "[install] created group assessment-agent"
	fi
	if ! id assessment-agent >/dev/null 2>&1; then
		useradd --system --gid assessment-agent --home "$STATE_DIR" \
		        --shell /sbin/nologin assessment-agent
		echo "[install] created user assessment-agent"
	fi

	install -d -o root             -g root             -m 0755 "$CFG_DIR"
	install -d -o assessment-agent -g assessment-agent -m 0700 "$STATE_DIR"

	# Stop running service (upgrade path).
	if [ -x "$SYSV_TARGET" ] && "$SYSV_TARGET" status >/dev/null 2>&1; then
		echo "[install] stopping running service for upgrade..."
		"$SYSV_TARGET" stop || true
	fi

	install -o root -g root -m 0755 "$DIST_BIN" /usr/local/bin/assessment-agent
	echo "[install] binary     : /usr/local/bin/assessment-agent"

	if [ ! -f "$SYSV_FILE" ]; then
		echo "[install] SysV init script missing: $SYSV_FILE" >&2
		exit 1
	fi
	install -o root -g root -m 0755 "$SYSV_FILE" "$SYSV_TARGET"
	echo "[install] init script: $SYSV_TARGET"

	if [ ! -f "$ENV_FILE" ]; then
		install -o root -g assessment-agent -m 0640 "$ENV_EXAMPLE" "$ENV_FILE"
		echo "[install] seeded env : $ENV_FILE (from agent.env.example)"
	fi
	sh "$SCRIPT_DIR/lib/env-setup.sh" "$ENV_EXAMPLE" "$ENV_FILE" "$ENV_LOCAL"

	# Register at boot — chkconfig (RHEL family) or update-rc.d (Debian).
	if command -v chkconfig >/dev/null 2>&1; then
		chkconfig --add assessment-agent >/dev/null 2>&1 || true
		chkconfig assessment-agent on >/dev/null 2>&1 || true
		echo "[install] registered with chkconfig (runlevels 2345)"
	elif command -v update-rc.d >/dev/null 2>&1; then
		update-rc.d assessment-agent defaults >/dev/null 2>&1 || true
		echo "[install] registered with update-rc.d"
	else
		echo "[install] WARNING: neither chkconfig nor update-rc.d found —" >&2
		echo "[install]   service installed to $SYSV_TARGET but not enabled at boot" >&2
	fi

	if [ "${IMAGE_PREP:-0}" = "1" ]; then
		printf '\n'
		printf '[install] IMAGE_PREP=1 — service registered but NOT started.\n'
		printf '[install] before sealing this VM into an image, run:\n'
		printf '[install]     assessment-agent prep-image\n'
		printf '\n'
		exit 0
	fi

	if command -v service >/dev/null 2>&1; then
		service assessment-agent start || true
	else
		"$SYSV_TARGET" start || true
	fi

	sleep 5
	if "$SYSV_TARGET" status >/dev/null 2>&1; then
		printf '\n'
		echo "[install] OK — assessment-agent is running (SysV)"
		echo "[install] logs:       tail -f /var/log/assessment-agent.log"
		echo "[install] stop:       service assessment-agent stop"
		echo "[install] uninstall:  assessment-agent uninstall"
	else
		echo "[install] WARNING: service is not running. Last 30 log lines:" >&2
		tail -n 30 /var/log/assessment-agent.log 2>/dev/null || true
		exit 1
	fi
}

# --- 6. Dispatch
if [ "$INIT_SYSTEM" = "systemd" ]; then
	install_user_systemd
else
	install_sysv_root
fi
