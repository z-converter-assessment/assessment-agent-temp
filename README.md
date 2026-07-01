# assessment-agent-temp

Assessment 수집 에이전트(C)의 릴리즈 빌드 전용 트리. 단일 소스에서 OS 세대별 5종 바이너리를 산출한다.
전송 wire 스키마는 5종 동일하고, 분기 축은 32/64bit이 아니라 NT/glibc 세대(제공 API + 암호 스택)다.

## 릴리즈 산출물 (5종)

| 파일 | 타깃 OS | ABI / 런타임 | 프로파일 |
|------|---------|--------------|----------|
| assessment-agent-linux-x86_64 | Ubuntu 18.04+, EL7-9, Amazon Linux 2/2023, SUSE 12/15 | glibc 2.17 / kernel 3.10 | manylinux2014 |
| assessment-agent-linux-x86_64-glibc2.12 | CentOS/EL 6 | glibc 2.12 / kernel 2.6.32 | manylinux2010 |
| assessment-agent-win2016-x64.exe | Server 2016/2019/2022/2025, Win10/11 | NT 10.0 x64, OpenSSL 3.x | modern |
| assessment-agent-win2008r2-x64.exe | Server 2008R2/2012/2012R2, Win7/8/8.1 | NT 6.1 x64, OpenSSL 3.x | win7 |
| assessment-agent-win2003-x86.exe | Server 2003 / XP (32-bit) | NT 5.2 i686, OpenSSL 1.0.2u | legacy32 |

## 빌드

### Linux (2종)

manylinux 컨테이너에서 빌드해 glibc ABI 하한을 컨테이너 glibc로 고정한다.

```bash
# modern (glibc 2.17)
BUILD_IMAGE=quay.io/pypa/manylinux2014_x86_64 BUILD_TARGET=release        sh scripts/build-linux.sh
# legacy (glibc 2.12)
BUILD_IMAGE=quay.io/pypa/manylinux2010_x86_64 BUILD_TARGET=release-legacy sh scripts/build-linux.sh
```

### Windows modern / win7 (x64 2종)

mingw-w64 x86_64 크로스툴체인.

```bash
cd windows-agent
X="CC=x86_64-w64-mingw32-gcc AR=x86_64-w64-mingw32-ar WINDRES=x86_64-w64-mingw32-windres OPENSSL_TARGET=mingw64 OPENSSL_CROSS=--cross-compile-prefix=x86_64-w64-mingw32-"
make PROFILE=modern $X vendor-build && make PROFILE=modern $X release   # win2016-x64
make PROFILE=win7   $X vendor-build && make PROFILE=win7   $X release   # win2008r2-x64
```

### Windows legacy32 (win2003-x86)

debian:bookworm 컨테이너에서 빌드한다. bookworm의 mingw-w64(gcc 12.2, CRT runtime 10.x)가 NT5.2(XP/2003) startup을 유지한다. CRT 12+ 툴체인(ubuntu 최신 등)으로 빌드하면 verify는 통과하나 실기 startup이 드롭된다.

```bash
cd windows-agent
docker run --rm --network host -v "$PWD":/w -w /w debian:bookworm bash -c '
  apt-get update && apt-get install -y gcc-mingw-w64-i686 binutils-mingw-w64-i686 cmake make git perl
  make PROFILE=legacy32 vendor-build && make PROFILE=legacy32 release'
```

Makefile이 rabbitmq-c의 SRWLock(Vista+)을 NT5.2용 CRITICAL_SECTION으로 교체하는 패치(`windows-agent/patches/nt52-threads/`)를 legacy32 빌드 시 자동 적용한다.
win2003 실기 배포에는 hive ComputerName + Tcpip MTU=1280 운영 설정이 필요하다.

## CI

`v*` 태그를 push하면 5종을 빌드해 해당 태그의 GitHub Release에 게시한다.

```bash
git tag v1.2.3 && git push origin v1.2.3
```

- required(없으면 릴리즈 실패): linux-x86_64, win2016-x64
- 나머지 3종은 있으면 붙이고 없으면 skip

## 트리

```
src/ include/ Makefile   Linux 소스 + 빌드
deploy/ scripts/         install.sh / systemd / sysv (바이너리에 embed)
windows-agent/           Windows 소스 + 빌드 (3 프로파일, NT5.2 threads 패치 포함)
.github/workflows/       태그 트리거 릴리즈
```
