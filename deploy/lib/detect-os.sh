#!/bin/sh
# detect-os.sh — sourced by install.sh.
#
# Source then call `detect_os` — it parses /etc/os-release, prints "ok" on
# stdout and returns 0 on supported targets, or prints a one-line reason and
# returns non-zero on unsupported.
#
# Supported matrix (must stay in sync with deploy/SUPPORTED_OS.md). The agent
# is a single musl fully-static binary (no glibc dependency), so support is
# bounded only by the kernel (>= 2.6.32), not by glibc version:
#   - Ubuntu 18.04 / 20.04 / 22.04 / 24.04
#   - Debian 10 / 11 / 12 / 13
#   - RHEL / CentOS / Rocky / AlmaLinux / Oracle Linux 6 / 7 / 8 / 9
#     (incl. CentOS Stream 8 + 9; EL6 is a SysV-init host, kernel 2.6.32)
#   - Amazon Linux 2, 2023
#   - SUSE Linux Enterprise / openSUSE Leap 11 / 12 / 15 (any SP)
#     (SLES 11 is a SysV-init host, kernel 3.0.x)
#   - Tencent OS 4.x
#
# CentOS / RHEL / Oracle Linux 6 and SLES 11 predate /etc/os-release, so when
# that file is absent we fall back to /etc/redhat-release or /etc/SuSE-release.
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
			# RHEL 6 / CentOS 6 / Oracle Linux 6 — SysV-init host, kernel
			# 2.6.32. Covered by the musl static binary (verified on 2.6.32).
			echo ok
			return 0
			;;
		*)
			echo "unsupported: '$rel' — only EL6 is handled via the redhat-release fallback; see deploy/SUPPORTED_OS.md"
			return 1
			;;
	esac
}

# detect_suse_release_sles11 — fallback for SLES 11, which predates
# /etc/os-release and ships only /etc/SuSE-release (VERSION = 11). Prints "ok"
# + returns 0 on SLES 11, else a reason + returns 1.
detect_suse_release_sles11() {
	rel=$(cat /etc/SuSE-release 2>/dev/null)
	ver=$(echo "$rel" | sed -n 's/^VERSION[ \t]*=[ \t]*\([0-9][0-9]*\).*/\1/p')
	case "$ver" in
		11)
			# SLES 11 — SysV-init host, kernel 3.0.x, glibc ~2.11. Covered by
			# the musl static binary (glibc version is irrelevant).
			echo ok
			return 0
			;;
		*)
			echo "unsupported: '$(echo "$rel" | head -1)' — only SLES 11 is handled via the SuSE-release fallback; see deploy/SUPPORTED_OS.md"
			return 1
			;;
	esac
}

detect_os() {
	if [ ! -r /etc/os-release ]; then
		# Pre-os-release hosts fall back to *-release marker files: SLES 11
		# (/etc/SuSE-release) or EL6 (/etc/redhat-release). Both are SysV-init.
		if [ -r /etc/SuSE-release ]; then
			detect_suse_release_sles11
			return $?
		fi
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
				11|12|15) echo ok; return 0 ;;
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
