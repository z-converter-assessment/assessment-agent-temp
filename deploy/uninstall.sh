#!/bin/sh
# Assessment Agent (Linux) — uninstaller.
#
# Invoked by the self-installer subcommand:
#     assessment-agent uninstall            # systemd host: no root needed
#     assessment-agent uninstall --purge    # also wipe config + worker state
#     sudo assessment-agent uninstall       # SysV/EL6 host: root (mirrors install)
#
# Mirrors install.sh's privilege model:
#   - systemd host → USER-LEVEL teardown (systemctl --user, no root)
#   - SysV/EL6 host → SYSTEM teardown, root (the service lives in /etc/init.d)
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

uninstall_user_systemd() {
	uid=$(id -u)
	: "${XDG_RUNTIME_DIR:=/run/user/$uid}"
	export XDG_RUNTIME_DIR

	CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/assessment-agent"
	STATE_DIR="${XDG_STATE_HOME:-$HOME/.local/state}/assessment-agent"
	BIN_TARGET="$HOME/.local/bin/assessment-agent"
	UNIT_TARGET="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/assessment-agent.service"

	if systemctl --user is-active --quiet assessment-agent.service 2>/dev/null; then
		echo "[uninstall] stopping user service..."
		systemctl --user stop assessment-agent.service || true
	fi
	if systemctl --user is-enabled --quiet assessment-agent.service 2>/dev/null; then
		systemctl --user disable assessment-agent.service >/dev/null 2>&1 || true
	fi

	if [ -f "$UNIT_TARGET" ]; then
		rm -f "$UNIT_TARGET"
		echo "[uninstall] removed $UNIT_TARGET"
	fi
	systemctl --user daemon-reload 2>/dev/null || true

	if [ -f "$BIN_TARGET" ]; then
		rm -f "$BIN_TARGET"
		echo "[uninstall] removed $BIN_TARGET"
	fi

	if [ "${PURGE:-0}" = "1" ]; then
		rm -rf "$CFG_DIR" "$STATE_DIR"
		echo "[uninstall] purged $CFG_DIR and $STATE_DIR"
		# Drop lingering so we leave no trace of a boot-time agent.
		if command -v loginctl >/dev/null 2>&1; then
			loginctl disable-linger "$(id -un)" 2>/dev/null || true
		fi
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
		if id assessment-agent >/dev/null 2>&1; then
			userdel assessment-agent 2>/dev/null || true
		fi
		if getent group assessment-agent >/dev/null 2>&1; then
			groupdel assessment-agent 2>/dev/null || true
		fi
		echo "[uninstall] removed assessment-agent user/group"
	else
		echo "[uninstall] preserved $CFG_DIR and $STATE_DIR (PURGE=1 to wipe)"
	fi

	echo "[uninstall] done."
}

if [ "$INIT_SYSTEM" = "systemd" ]; then
	uninstall_user_systemd
else
	uninstall_sysv_root
fi
