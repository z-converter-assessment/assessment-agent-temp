# assessment-agent-temp

Assessment 수집 에이전트(C)의 릴리즈 빌드 전용 컴팩트 트리. 소스는 Linux/Windows 공용이고,
빌드 프로파일별로 5종의 바이너리를 산출한다.

## 릴리즈 산출물 (5종)

| 파일 | 타깃 | ABI / 런타임 | 빌드 |
|------|------|--------------|------|
| assessment-agent-linux-x86_64 | Ubuntu 18.04+, EL7-9, Amazon Linux 2/2023 | glibc 2.17 / kernel 3.10 | `make release` |
| assessment-agent-linux-x86_64-glibc2.12 | EL6 (SysV, no os-release) | glibc 2.12 / kernel 2.6.32 | `make release-legacy` |
| assessment-agent-win2016-x64.exe | Server 2016/2019/2022/2025, Win10/11 | NT 10.0, OpenSSL 3.x | `make release` |
| assessment-agent-win2008r2-x64.exe | Server 2008R2/2012/2012R2, Win7/8/8.1 | NT 6.1, OpenSSL 3.x | `make PROFILE=win7 release` |
| assessment-agent-win2003-x86.exe | Server 2003 / XP (32-bit) | NT 5.2 i686, OpenSSL 1.0.2u | `make PROFILE=legacy32 release` |

## Linux 빌드

release 산출물은 native amd64 Linux에서만 만든다. glibc ABI 하한이 빌드 호스트 glibc로 결정되므로,
modern은 manylinux2014(glibc 2.17), legacy는 manylinux2010(glibc 2.12) 호스트를 쓴다.

```bash
# modern (glibc 2.17)
docker run --rm -v "$PWD":/src -w /src quay.io/pypa/manylinux2014_x86_64 \
    bash -lc 'make vendor-fetch && make vendor-build && make USE_VENDORED=1 release'

# legacy (glibc 2.12)
docker run --rm -v "$PWD":/src -w /src quay.io/pypa/manylinux2010_x86_64 \
    bash -lc 'make vendor-fetch && make vendor-build && make USE_VENDORED=1 release-legacy'
```

컨테이너 없이 native 빌드하려면: `sudo bash scripts/build-prep.sh` 후 위 make 시퀀스.
`make verify` / `verify-legacy` 가 glibc 심볼 상한, 동적 의존 화이트리스트, 금지 API를 강제한다.

## Windows 빌드

MSYS2 + mingw-w64 (native) 또는 x86_64/i686-w64-mingw32 크로스 툴체인.
legacy32는 NT 5.2 startup을 여전히 내는 구형 i686 MinGW-w64 + win32/threads NT5.2 패치가 필요하다.

```bash
cd windows-agent
make vendor-fetch && make vendor-build && make release              # win2016-x64
make PROFILE=win7     vendor-build && make PROFILE=win7     release  # win2008r2-x64
make PROFILE=legacy32 vendor-build && make PROFILE=legacy32 release  # win2003-x86
```

`make verify` 가 import table에 Windows 시스템 DLL만 남았는지 확인한다.

## 트리

```
src/ include/ Makefile   Linux 소스 + 빌드
deploy/ scripts/         install.sh / systemd / sysv 등 (바이너리에 embed)
windows-agent/           Windows 소스 + 빌드 (3 프로파일)
```
