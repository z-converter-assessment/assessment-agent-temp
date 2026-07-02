#!/bin/sh
# Assessment Agent (Linux) — uninstaller.
#
# Invoked by the self-installer subcommand (root required, mirrors install):
#     sudo assessment-agent uninstall            # stop + remove service
#     sudo assessment-agent uninstall --purge    # also wipe config + worker state
#
# Mirrors install.sh's privilege model — root on both init systems:
#   - systemd host → remove /etc/systemd/system/assessment-agent.service
#   - SysV/EL6 host → remove /etc/init.d/assessment-agent
#
# By default config + worker state are preserved so a re-install keeps
# configuration. Set PURGE=1 to wipe them.

set -eu

# --- Init-system detection (same logic as install.sh).
INIT_SYSTEM=sysv
if command -v systemctl >/dev/null 2>&1; then
	if [ -d /run/systemd/system ] || pidof systemd >/dev/null 2>&1; then
		INIT_SYSTEM=systemd
	fi
fi

uninstall_system_systemd() {
	CFG_DIR=/etc/assessment-agent
	STATE_DIR=/var/lib/agent-worker
	BIN_TARGET=/usr/local/bin/assessment-agent
	UNIT_TARGET=/etc/systemd/system/assessment-agent.service

	if [ "$(id -u)" -ne 0 ]; then
		echo "[uninstall] systemd host: removing the system service needs root." >&2
		echo "[uninstall]   Re-run with sudo." >&2
		exit 1
	fi

	if systemctl is-active --quiet assessment-agent.service 2>/dev/null; then
		echo "[uninstall] stopping service..."
		systemctl stop assessment-agent.service || true
	fi
	if systemctl is-enabled --quiet assessment-agent.service 2>/dev/null; then
		systemctl disable assessment-agent.service >/dev/null 2>&1 || true
	fi

	if [ -f "$UNIT_TARGET" ]; then
		rm -f "$UNIT_TARGET"
		echo "[uninstall] removed $UNIT_TARGET"
	fi
	systemctl daemon-reload 2>/dev/null || true

	if [ -f "$BIN_TARGET" ]; then
		rm -f "$BIN_TARGET"
		echo "[uninstall] removed $BIN_TARGET"
	fi

	if [ "${PURGE:-0}" = "1" ]; then
		rm -rf "$CFG_DIR" "$STATE_DIR"
		echo "[uninstall] purged $CFG_DIR and $STATE_DIR"
	else
		echo "[uninstall] preserved $CFG_DIR and $STATE_DIR (PURGE=1 to wipe)"
	fi

	echo "[uninstall] done."
}

uninstall_sysv_root() {
	CFG_DIR=/etc/assessment-agent
	STATE_DIR=/var/lib/agent-worker
	BIN_TARGET=/usr/local/bin/assessment-agent
	SYSV_TARGET=/etc/init.d/assessment-agent

	if [ "$(id -u)" -ne 0 ]; then
		echo "[uninstall] SysV/EL6 host: removing the /etc/init.d service needs root." >&2
		echo "[uninstall]   Re-run with sudo." >&2
		exit 1
	fi

	if [ -x "$SYSV_TARGET" ] && "$SYSV_TARGET" status >/dev/null 2>&1; then
		echo "[uninstall] stopping service..."
		"$SYSV_TARGET" stop || true
	fi

	if command -v chkconfig >/dev/null 2>&1; then
		chkconfig --del assessment-agent >/dev/null 2>&1 || true
	elif command -v update-rc.d >/dev/null 2>&1; then
		update-rc.d -f assessment-agent remove >/dev/null 2>&1 || true
	fi

	if [ -f "$SYSV_TARGET" ]; then
		rm -f "$SYSV_TARGET"
		echo "[uninstall] removed $SYSV_TARGET"
	fi
	if [ -f "$BIN_TARGET" ]; then
		rm -f "$BIN_TARGET"
		echo "[uninstall] removed $BIN_TARGET"
	fi

	if [ "${PURGE:-0}" = "1" ]; then
		rm -rf "$CFG_DIR" "$STATE_DIR"
		echo "[uninstall] purged $CFG_DIR and $STATE_DIR"
	else
		echo "[uninstall] preserved $CFG_DIR and $STATE_DIR (PURGE=1 to wipe)"
	fi

	echo "[uninstall] done."
}

if [ "$INIT_SYSTEM" = "systemd" ]; then
	uninstall_system_systemd
else
	uninstall_sysv_root
fi
