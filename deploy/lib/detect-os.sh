#!/bin/sh
# detect-os.sh — sourced by install.sh.
#
# Source then call `detect_os` — it parses /etc/os-release, prints "ok" on
# stdout and returns 0 on supported targets, or prints a one-line reason and
# returns non-zero on unsupported.
#
# Supported matrix (must stay in sync with deploy/SUPPORTED_OS.md and the
# ABI ceilings — modern build = glibc 2.17 = CentOS 7; LEGACY build =
# glibc 2.12 = CentOS 6, produced by `make release-legacy`):
#   - Ubuntu 18.04 / 20.04 / 22.04 / 24.04
#   - Debian 10 / 11 / 12 / 13
#   - RHEL / CentOS / Rocky / AlmaLinux / Oracle Linux 6 / 7 / 8 / 9
#     (incl. CentOS Stream 8 + 9; 6 requires the LEGACY glibc-2.12 binary)
#   - Amazon Linux 2, 2023
#   - SUSE Linux Enterprise / openSUSE Leap 12 / 15 (any SP)
#   - Tencent OS 4.x
#
# CentOS / RHEL / Oracle Linux 6 predate /etc/os-release, so when that file
# is absent we fall back to parsing /etc/redhat-release (or /etc/centos-release).
# The os-release path stays primary for every systemd-era target.

# detect_redhat_release_el6 — fallback for pre-systemd RHEL-family hosts that
# have no /etc/os-release. Parses the *-release marker files for a "release 6"
# token. Prints "ok" + returns 0 on EL6, else prints a reason + returns 1.
detect_redhat_release_el6() {
	rel=""
	for f in /etc/redhat-release /etc/centos-release /etc/oracle-release \
	         /etc/system-release; do
		if [ -r "$f" ]; then
			rel=$(cat "$f" 2>/dev/null)
			[ -n "$rel" ] && break
		fi
	done
	if [ -z "$rel" ]; then
		echo "unsupported: /etc/os-release and /etc/redhat-release both missing — pre-systemd?"
		return 1
	fi
	# Extract the major version: the token following the word "release".
	# e.g. "CentOS release 6.10 (Final)" → 6 ; "Red Hat Enterprise Linux
	# Server release 6.9 (Santiago)" → 6.
	major=$(echo "$rel" | sed -n 's/.*release[ \t]*\([0-9][0-9]*\).*/\1/p')
	case "$major" in
		6)
			# RHEL 6 / CentOS 6 / Oracle Linux 6 share the glibc-2.12 baseline.
			# Requires the LEGACY binary (make release-legacy). SysV-init host.
			echo ok
			return 0
			;;
		*)
			echo "unsupported: '$rel' — only EL6 is handled via the redhat-release fallback; see deploy/SUPPORTED_OS.md"
			return 1
			;;
	esac
}

detect_os() {
	if [ ! -r /etc/os-release ]; then
		# Pre-systemd RHEL family (CentOS/RHEL/Oracle Linux 6) has no
		# /etc/os-release — fall back to the *-release marker files.
		detect_redhat_release_el6
		return $?
	fi
	# shellcheck disable=SC1091
	. /etc/os-release
	id="${ID:-unknown}"
	ver="${VERSION_ID:-unknown}"
	major="${ver%%.*}"

	case "$id" in
		ubuntu)
			case "$ver" in
				18.04|20.04|22.04|24.04) echo ok; return 0 ;;
			esac
			;;
		debian)
			case "$major" in
				10|11|12|13) echo ok; return 0 ;;
			esac
			;;
		rhel|centos|rocky|almalinux|ol|oracle)
			case "$major" in
				7|8|9) echo ok; return 0 ;;
			esac
			;;
		amzn)
			case "$ver" in
				2|2023) echo ok; return 0 ;;
			esac
			;;
		sles|opensuse-leap|opensuse)
			case "$major" in
				12|15) echo ok; return 0 ;;
			esac
			;;
		tencentos)
			case "$major" in
				4) echo ok; return 0 ;;
			esac
			;;
	esac
	echo "unsupported: $id-$ver — see deploy/SUPPORTED_OS.md"
	return 1
}
