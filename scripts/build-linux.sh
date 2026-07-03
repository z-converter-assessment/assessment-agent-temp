#!/bin/sh
# build-linux.sh — musl fully-static release build inside an Alpine container.
#
# Produces a SINGLE binary — dist/assessment-agent-linux-x86_64 — that is
# statically linked against musl libc (and all vendored deps). It carries no
# dynamic libc, so it is glibc-version independent and runs on ANY x86_64 Linux
# with kernel >= 2.6.32. That covers every supported image in one artifact
# (verified on EL6 / kernel 2.6.32 and SLES 11 / kernel 3.0.13), replacing the
# former glibc modern+legacy split.
#
# Host requirement: **Docker only**. Everything (toolchain, vendor build, link)
# happens inside the Alpine container; outputs are chown'd back to the caller.
#
# Usage (from repo root):
#   ./scripts/build-linux.sh
#
# Knobs (env):
#   BUILD_IMAGE    musl build container (default alpine:3.19)
#   AGENT_VERSION  payload agent_version (CI injects the release tag)
#
# NOTE on Apple Silicon: the image runs linux/amd64. ARM hosts emulate it
# (slow); prefer a native amd64 build host for release artifacts.

set -eu

cd "$(dirname "$0")/.."

if ! command -v docker >/dev/null 2>&1; then
	echo "[build-linux] docker not found. Install Docker Desktop / Docker Engine first." >&2
	exit 1
fi
if ! docker info >/dev/null 2>&1; then
	echo "[build-linux] docker daemon not running. Start it and retry." >&2
	exit 1
fi

UID_HOST=$(id -u)
GID_HOST=$(id -g)

BUILD_IMAGE="${BUILD_IMAGE:-alpine:3.19}"
# payload agent_version — CI injects from the git tag (release.yml). Local dev
# falls back to this.
AGENT_VERSION="${AGENT_VERSION:-0.0.0-dev}"

echo "[build-linux] image=$BUILD_IMAGE musl fully-static agent_version=$AGENT_VERSION"

docker run --rm \
    --network host \
    --platform linux/amd64 \
    -v "$(pwd)":/src -w /src \
    "$BUILD_IMAGE" \
    sh -c "
        set -e
        apk add --no-cache build-base cmake perl linux-headers git curl bash file pkgconf autoconf automake libtool >/dev/null
        echo '[build-linux] toolchain:' \$(gcc --version | head -1)
        # src/*.o generation-safety — reuse of glibc-built objects breaks the
        # musl link (undefined __strdup etc.), so always recompile.
        make clean
        make vendor-fetch
        make vendor-build
        make USE_VENDORED=1 AGENT_VERSION='${AGENT_VERSION}' release
        chown -R ${UID_HOST}:${GID_HOST} vendor dist build 2>/dev/null || true
    "

echo
echo "[build-linux] OK — dist artifacts:"
ls -la dist/
