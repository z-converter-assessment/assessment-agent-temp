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
	# manylinux2014(CentOS7) base cmake=2.8.12 -> libarchive(cmake 3.x 필요) 실패.
	# cmake3(EPEL, 3.17)을 설치해 /usr/local/bin/cmake 로 우선 노출.
	if yum install -y -q cmake3 >/dev/null 2>&1 && command -v cmake3 >/dev/null 2>&1; then
		ln -sf "$(command -v cmake3)" /usr/local/bin/cmake
		echo "  + cmake3 -> /usr/local/bin/cmake ($(cmake3 --version | head -1))"
	fi
else
	echo "[build-prep] no supported package manager found (apt-get / yum / dnf)" >&2
	exit 1
fi

echo "[build-prep] OK — vendor-build prerequisites installed"
