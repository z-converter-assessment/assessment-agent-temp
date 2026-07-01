#!/bin/sh
# image-prep.sh
#
# Run this on the GOLDEN VM IMAGE before snapshotting / sealing.
#
# Identity in this system is keyed on `composite_id` = sha256(machine_id +
# MAC addresses), and the engine upserts on composite_id (machine_id is
# display-only). Cloned VMs almost always get fresh MACs, so their
# composite_id diverges automatically — clone collisions are largely handled
# without this script. It remains useful belt-and-suspenders for the rare
# identical-MAC clone (virtual-NIC-only hosts) and general image hygiene.
#
# USER-LEVEL by design: this runs as the unprivileged agent user. The system
# hygiene steps below (/etc/machine-id, random-seed, cloud-init cache) require
# root; each is attempted best-effort and SKIPPED with a note when not
# writable — never fatal. If you want the full generalize, run the image build
# as root (image builders normally have root) or use cloud-init's standard
# image-cleanup.

set -eu

printf '\n=== Assessment Agent — image preparation ===\n'
printf 'Run this once on the GOLDEN TEMPLATE before snapshotting.\n\n'

am_root=0
[ "$(id -u)" -eq 0 ] && am_root=1

# --- 1. Stop the agent if running (user service; best-effort).
uid=$(id -u)
: "${XDG_RUNTIME_DIR:=/run/user/$uid}"
export XDG_RUNTIME_DIR
if command -v systemctl >/dev/null 2>&1; then
	if systemctl --user is-active --quiet assessment-agent.service 2>/dev/null; then
		echo "[image-prep] stopping user assessment-agent.service..."
		systemctl --user stop assessment-agent.service || true
	fi
	# SysV/root install variant.
	if [ "$am_root" -eq 1 ] && systemctl is-active --quiet assessment-agent.service 2>/dev/null; then
		systemctl stop assessment-agent.service || true
	fi
fi

# --- 2. Clear /etc/machine-id (root only — best-effort).
#
# systemd-machine-id-setup regenerates an empty (zero-byte, not deleted) file
# on next boot. The dbus copy is symlinked so the two stay in sync.
if [ "$am_root" -eq 1 ] && [ -w /etc/machine-id ]; then
	echo "[image-prep] clearing /etc/machine-id (regenerated on next boot)"
	: > /etc/machine-id
	if [ -e /var/lib/dbus/machine-id ] && [ ! -L /var/lib/dbus/machine-id ]; then
		rm -f /var/lib/dbus/machine-id
		ln -s /etc/machine-id /var/lib/dbus/machine-id
	fi
else
	echo "[image-prep] SKIP /etc/machine-id reset (needs root)."
	echo "[image-prep]   Not fatal — composite_id diverges via per-clone MACs."
fi

# --- 3. Clear systemd random seed (root only — regenerated on boot).
if [ "$am_root" -eq 1 ] && [ -e /var/lib/systemd/random-seed ]; then
	rm -f /var/lib/systemd/random-seed
fi

# --- 4. Clear cloud-init instance cache (root only) so each clone fetches its
#        own metadata on first boot.
if [ "$am_root" -eq 1 ] && [ -d /var/lib/cloud ]; then
	rm -rf /var/lib/cloud/instance \
	       /var/lib/cloud/instances/* \
	       /var/lib/cloud/data/* 2>/dev/null || true
	echo "[image-prep] cleared cloud-init cache"
fi

# --- 5. We deliberately KEEP the agent config (agent.env / agent.env.local).
#        Broker credentials are per-tenant, not per-machine. Delete manually
#        before this script if you want per-machine credentials.
echo "[image-prep] keeping agent.env{,.local} (per-tenant secrets)"

printf '\n[image-prep] done — VM is ready to snapshot.\n'
if [ "$am_root" -eq 0 ]; then
	printf '[image-prep] NOTE: ran unprivileged — system-wide hygiene steps were\n'
	printf '[image-prep]       skipped. composite_id (MAC-based) still keeps clones\n'
	printf '[image-prep]       distinct; run as root only if you need a full generalize.\n'
fi
