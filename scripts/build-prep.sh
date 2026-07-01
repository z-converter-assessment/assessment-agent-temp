#!/bin/sh
# build-prep.sh — install vendor-build prerequisites on the native Linux host.
#
# Auto-detects apt-get (Debian / Ubuntu) or yum / dnf (RHEL / CentOS / Rocky /
# AlmaLinux / Oracle Linux / Amazon Linux) and installs the system packages
# needed by `make vendor-build` (mostly: compiler toolchain, cmake, git, and
# Perl + IPC::Cmd which OpenSSL 3.0+ Configure requires).
#
# Two ways the project consumes this:
#   - Native amd64 build host: operator runs `sudo bash scripts/build-prep.sh`
#     once, then `make vendor-fetch && make vendor-build && make USE_VENDORED=1 release`
#   - Containerized build: `scripts/build-linux.sh` calls this from inside
#     the manylinux2014 container automatically (yum branch).

set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "[build-prep] must run as root (try: sudo $0)" >&2
	exit 1
fi

# Package lists — kept tight (only what vendor-build actually needs).
# Some packages (e.g. perl-Pod-Html, perl-Module-Load-Conditional) ship as
# core perl modules on certain distros (no separate package) and as
# stand-alone packages on others. Use a try-skip loop so missing names
# don't abort the whole install — the OpenSSL Configure step would error
# loudly anyway if a TRULY required module is missing.
APT_PKGS='build-essential cmake git pkg-config perl ca-certificates'
RPM_PKGS='gcc make cmake git pkgconfig perl perl-IPC-Cmd perl-Data-Dumper perl-Test-Simple perl-Pod-Html perl-Module-Load-Conditional ca-certificates'

try_install_one() {
	mgr=$1
	pkg=$2
	if $mgr install -y -q "$pkg" >/dev/null 2>&1; then
		echo "  + $pkg"
	else
		echo "  - $pkg (skipped — not available in configured repos)"
	fi
}

if command -v apt-get >/dev/null 2>&1; then
	echo "[build-prep] detected apt — installing per-package (skip missing)"
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	for pkg in $APT_PKGS; do
		try_install_one apt-get "$pkg"
	done
elif command -v dnf >/dev/null 2>&1; then
	echo "[build-prep] detected dnf — installing per-package (skip missing)"
	dnf install -y -q epel-release >/dev/null 2>&1 || true
	for pkg in $RPM_PKGS; do
		try_install_one dnf "$pkg"
	done
elif command -v yum >/dev/null 2>&1; then
	echo "[build-prep] detected yum — installing per-package (skip missing)"
	yum install -y -q epel-release >/dev/null 2>&1 || true
	for pkg in $RPM_PKGS; do
		try_install_one yum "$pkg"
	done
	# cmake 최소 요구: rabbitmq-c v0.15.0 은 CMake 3.22+ 를 요구한다.
	# 최신 manylinux 이미지엔 이미 pipx cmake(4.x)가 /usr/local/bin/cmake 로 있으므로,
	# EPEL cmake3(3.17)로 덮어쓰면 오히려 강등되어 rabbitmq-c 빌드가 깨진다.
	# 기존 cmake 가 3.22+ 이면 손대지 않고, 그보다 낮을 때만 cmake3 로 보강한다.
	cmake_ok=0
	have=$(cmake --version 2>/dev/null | sed -n '1s/.*version \([0-9]*\)\.\([0-9]*\).*/\1 \2/p')
	if [ -n "$have" ]; then
		set -- $have
		if [ "$1" -gt 3 ] || { [ "$1" -eq 3 ] && [ "$2" -ge 22 ]; }; then cmake_ok=1; fi
	fi
	if [ "$cmake_ok" -eq 1 ]; then
		echo "  = cmake $(cmake --version | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+') 이미 충분 (>=3.22) — 유지"
	elif yum install -y -q cmake3 >/dev/null 2>&1 && command -v cmake3 >/dev/null 2>&1; then
		ln -sf "$(command -v cmake3)" /usr/local/bin/cmake
		echo "  + cmake3 -> /usr/local/bin/cmake ($(cmake3 --version | head -1))"
	fi
else
	echo "[build-prep] no supported package manager found (apt-get / yum / dnf)" >&2
	exit 1
fi

echo "[build-prep] OK — vendor-build prerequisites installed"
