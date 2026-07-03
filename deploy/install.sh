#!/bin/sh
# Assessment Agent (Linux) — installer.
#
# Single entry point. Root is required and the agent runs AS root by design:
# the collector reads every process' /proc/<pid>/exe, /proc/<pid>/fd and comm,
# which is only possible as root. Running unprivileged leaves listen_ports[].pid
# /comm and services[].exe null for daemons owned by other users (sshd/nginx/
# mysql/…) — i.e. most of the interesting inventory. So both the install and the
# 24/7 runtime are root. The layout is the same on both init systems:
#
#   /usr/local/bin/assessment-agent
#   /etc/assessment-agent/agent.env{,.local}
#   /var/lib/agent-worker/                       (worker state)
#
#   - systemd hosts (CentOS/RHEL 7+, Ubuntu, Debian, SLES, Amazon Linux):
#       /etc/systemd/system/assessment-agent.service, managed via `systemctl`
#       (User=root, WantedBy=multi-user.target — starts at boot).
#   - SysV hosts (CentOS/RHEL/Oracle Linux 6 — no systemd):
#       /etc/init.d/assessment-agent, runs the binary as root. The same musl
#       static binary covers EL6 (kernel 2.6.32) — no separate legacy build.
#
# Re-runnable (idempotent). Each step short-circuits when already done.
#
# Flags (set via env, not argv — POSIX sh has no getopt without external help):
#   IMAGE_PREP=1     register service but do NOT start. Use before sealing a
#                    golden VM image; then run `assessment-agent prep-image`.
#   SKIP_SHA256=1    skip dist/SHA256SUMS verification (only when intentional).
#
# Server-side dependencies (sh + coreutils are guaranteed on every supported
# OS). systemctl is required ONLY on systemd hosts; chkconfig/update-rc.d ONLY
# on the SysV path — checked inside their own branch, not up front.

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
	echo "[install] build it first: scripts/build-linux.sh (musl static, single" >&2
	echo "[install]   binary covering all supported x86_64 Linux incl. EL6)" >&2
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
#  systemd path — SYSTEM install, root (agent runs as root)
# ======================================================================
install_system_systemd() {
	require_cmds systemctl install chmod awk grep sed tr mktemp sha256sum

	if [ "$(id -u)" -ne 0 ]; then
		echo "[install] systemd host: installing a system service that runs the" >&2
		echo "[install]   agent as root needs root. Re-run with sudo." >&2
		exit 1
	fi

	CFG_DIR=/etc/assessment-agent
	STATE_DIR=/var/lib/agent-worker
	BIN_TARGET=/usr/local/bin/assessment-agent
	UNIT_TARGET=/etc/systemd/system/assessment-agent.service
	ENV_FILE="$CFG_DIR/agent.env"
	ENV_LOCAL="$CFG_DIR/agent.env.local"

	verify_sha256

	# Directories. Config 0755 (world-readable dir, secrets are in the 0600
	# agent.env.local); worker state 0700 root-only.
	install -d -o root -g root -m 0755 "$CFG_DIR"
	install -d -o root -g root -m 0700 "$STATE_DIR"

	# Stop a running instance so the binary can be replaced (upgrade path).
	if systemctl is-active --quiet assessment-agent.service 2>/dev/null; then
		echo "[install] stopping running service for upgrade..."
		systemctl stop assessment-agent.service || true
	fi

	install -o root -g root -m 0755 "$DIST_BIN" "$BIN_TARGET"
	echo "[install] binary     : $BIN_TARGET"

	# System unit — absolute paths inside the unit (/etc, /var/lib), no path
	# substitution needed here.
	install -o root -g root -m 0644 "$SERVICE_FILE" "$UNIT_TARGET"

	# env file (seed from example on first run, then env-setup fills empties).
	# agent.env.example ships WORKER_STATE_DIR=/var/lib/agent-worker, which is
	# exactly where the system service writes — no rewrite needed.
	if [ ! -f "$ENV_FILE" ]; then
		install -o root -g root -m 0640 "$ENV_EXAMPLE" "$ENV_FILE"
		echo "[install] seeded env : $ENV_FILE (from agent.env.example)"
	fi
	sh "$SCRIPT_DIR/lib/env-setup.sh" "$ENV_EXAMPLE" "$ENV_FILE" "$ENV_LOCAL"

	systemctl daemon-reload

	# image-prep mode: enable but do not start.
	if [ "${IMAGE_PREP:-0}" = "1" ]; then
		systemctl enable assessment-agent.service >/dev/null 2>&1 || true
		printf '\n'
		printf '[install] IMAGE_PREP=1 — service enabled but NOT started.\n'
		printf '[install] before sealing this VM into an image, run:\n'
		printf '[install]     assessment-agent prep-image\n'
		printf '\n'
		exit 0
	fi

	systemctl enable --now assessment-agent.service

	sleep 5
	if systemctl is-active --quiet assessment-agent.service; then
		printf '\n'
		echo "[install] OK — assessment-agent is active (system service, root)"
		echo "[install] logs:       journalctl -u assessment-agent -f"
		echo "[install] stop:       systemctl stop assessment-agent"
		echo "[install] uninstall:  assessment-agent uninstall"
	else
		echo "[install] WARNING: service is not active. Last 30 log lines:" >&2
		journalctl -u assessment-agent.service -n 30 --no-pager 2>/dev/null || true
		exit 1
	fi
}

# ======================================================================
#  SysV path — SYSTEM install, root (agent runs as root)
# ======================================================================
install_sysv_root() {
	CFG_DIR=/etc/assessment-agent
	STATE_DIR=/var/lib/agent-worker
	ENV_FILE="$CFG_DIR/agent.env"
	ENV_LOCAL="$CFG_DIR/agent.env.local"

	if [ "$(id -u)" -ne 0 ]; then
		echo "[install] SysV/EL6 host: installing an /etc/init.d service that runs" >&2
		echo "[install]   the agent as root needs root. Re-run with sudo." >&2
		exit 1
	fi

	require_cmds sha256sum id install chmod awk grep sed tr mktemp
	verify_sha256

	install -d -o root -g root -m 0755 "$CFG_DIR"
	install -d -o root -g root -m 0700 "$STATE_DIR"

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
		install -o root -g root -m 0640 "$ENV_EXAMPLE" "$ENV_FILE"
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
	install_system_systemd
else
	install_sysv_root
fi
