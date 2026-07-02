# 빌드 / 릴리즈

단일 소스에서 OS 세대별 5종 바이너리를 산출한다. 산출물 목록과 설치/사용법은 [../README.md](../README.md) 참고.

## 빌드

### Linux (2종)

manylinux 컨테이너에서 빌드해 glibc ABI 하한을 컨테이너 glibc로 고정한다.

```bash
BUILD_IMAGE=quay.io/pypa/manylinux2014_x86_64 BUILD_TARGET=release        sh scripts/build-linux.sh
BUILD_IMAGE=quay.io/pypa/manylinux2010_x86_64 BUILD_TARGET=release-legacy sh scripts/build-linux.sh
```

modern과 legacy는 같은 트리를 공유한다. src/*.o는 build-linux.sh가 매 빌드 자동 clean하지만, vendor(.a)는 재활용되므로 세대를 전환할 때(modern <-> legacy)는 vendor를 먼저 정리한다 — 다른 glibc로 빌드된 .a가 섞이면 legacy verify(GLIBC 2.13+ 금지)가 실패한다.

```bash
docker run --rm -v "$PWD":/src -w /src <해당 이미지> rm -rf vendor
```

CI(release.yml)는 매번 깨끗한 러너라 이 문제가 없다.

### Windows modern / win7 (x64 2종)

mingw-w64 x86_64 크로스툴체인으로 빌드한다.

```bash
cd windows-agent
X="CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar WINDRES=x86_64-w64-mingw32-windres OPENSSL_TARGET=mingw64 OPENSSL_CROSS=--cross-compile-prefix=x86_64-w64-mingw32-"
make PROFILE=modern $X vendor-build && make PROFILE=modern $X release   # win2016-x64
make PROFILE=win7   $X vendor-build && make PROFILE=win7   $X release   # win2008r2-x64
```

### Windows legacy32 (win2003-x86)

debian:bookworm 컨테이너에서 빌드한다. bookworm의 mingw-w64(gcc 12.2, CRT runtime 10.x)가 NT5.2(XP/2003) startup을 유지한다.

```bash
cd windows-agent
docker run --rm --network host -v "$PWD":/w -w /w debian:bookworm bash -c '
  apt-get update && apt-get install -y gcc-mingw-w64-i686 binutils-mingw-w64-i686 cmake make git perl
  make PROFILE=legacy32 vendor-build && make PROFILE=legacy32 release'
```

Makefile이 legacy32 빌드 시 rabbitmq-c의 win32/threads를 NT5.2용 패치(`windows-agent/patches/nt52-threads/`, SRWLock -> CRITICAL_SECTION)로 교체한다.
win2003 실기 배포는 hive ComputerName + Tcpip MTU=1280 설정을 사용한다.

## CI

`v*` 태그를 push하면 5종을 빌드해 해당 태그의 GitHub Release에 게시한다.

```bash
git tag v1.2.3 && git push origin v1.2.3
```

required는 linux-x86_64와 win2016-x64다. 나머지 3종은 있으면 붙이고 없으면 skip한다.

## 트리

```
src/ include/ Makefile   Linux 소스 + 빌드
deploy/ scripts/         install.sh / systemd / sysv (바이너리에 embed)
windows-agent/           Windows 소스 + 빌드 (3 프로파일, NT5.2 threads 패치)
.github/workflows/       태그 트리거 릴리즈
```
