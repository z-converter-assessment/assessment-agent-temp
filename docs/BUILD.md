# 빌드 / 릴리즈

단일 소스에서 OS별 2종 바이너리를 산출한다(Linux 1종 + Windows 1종). 산출물 목록과 설치/사용법은 [../README.md](../README.md) 참고.

## 빌드

### Linux (1종)

Alpine 컨테이너에서 musl 완전 static으로 빌드한다. 단일 바이너리가 glibc 버전과 무관하게 커널 2.6.32 이상 전 x86_64 리눅스를 커버한다(EL6/SLES11 실기 확인).

```bash
sh scripts/build-linux.sh
```

산출물은 `dist/assessment-agent-linux-x86_64`. build-linux.sh가 컨테이너 안에서 `make clean` -> `vendor-build`(musl) -> `release`를 돌리고, verify가 완전 static(PT_INTERP 없음, GLIBC 심볼 0)을 확인한다. glibc 세대 분기가 없어 vendor 정리 같은 세대 전환 이슈도 없다. 빌드 이미지는 `BUILD_IMAGE`(기본 `alpine:3.19`)로 바꿀 수 있다.

### Windows (1종, single i686)

단일 i686 바이너리(assessment-agent-windows-x86.exe)로 NT5.2(2003/XP)부터 NT10(2016+/Win10-11)까지 커버한다. 세대 분기는 런타임(collect.c `agent_is_nt6` + GetProcAddress)이라 프로파일 인자가 없다. debian:bookworm 컨테이너로만 빌드한다 — bookworm의 mingw-w64(i686, gcc 12.2, CRT runtime 10.x)가 NT5.2 startup을 유지한다. CRT 12+로 빌드하면 verify는 통과해도 XP/2003 실기 로드가 안 된다.

```bash
cd windows-agent
docker run --rm --network host -v "$PWD":/w -w /w debian:bookworm bash -c '
  apt-get update && apt-get install -y gcc-mingw-w64-i686 binutils-mingw-w64-i686 cmake make git perl
  make vendor-build && make release'
```

빌드 상세: agent 소스는 `_WIN32_WINNT=0x0600`으로 컴파일해 NT6 구조체를 선언하되, NT6 전용 함수는 하드임포트하지 않고 GetProcAddress로 해소한다. 벤더 라이브러리(OpenSSL 1.0.2u/curl/rabbitmq-c)는 `0x0502`로 빌드하고, Makefile이 rabbitmq-c의 win32/threads를 NT5.2용 패치(`windows-agent/patches/nt52-threads/`, SRWLock -> CRITICAL_SECTION)로 교체한다. verify가 (1) 시스템 DLL만 참조하는지, (2) NT6+ 심볼이 하드임포트되지 않았는지(NT5.2 로드 가드)를 검사해 위반 시 빌드를 실패시킨다.
win2003 실기 배포는 hive ComputerName + Tcpip MTU=1280 + EnablePMTUBHDetect=1(구형 스택 PMTU black-hole 감지) 설정을 사용한다.

## 계약 conformance

wire 계약의 기계검증 정본은 `schema/wire.schema.json`(JSON Schema)이고, 산문 설명은 [payload-contract.md](payload-contract.md)다. 두 바이너리는 `emit` dry-run 서브커맨드로 실제 직렬화 코드를 태워 한 페이로드를 stdout에 출력한다(발행/MQ/TLS 없음).

```bash
scripts/check-contract.sh dist/assessment-agent-linux-x86_64            # 네이티브
scripts/check-contract.sh dist/assessment-agent-windows-x86.exe wine    # 로컬 wine
```

`check-contract.sh`가 `emit inventory`/`emit metrics` 출력을 스키마로 검증한다(`scripts/validate-wire.py`). 필드/타입/null 의미론과 os_family 조건부(예: saturation은 Windows 전용)를 강제해, 리눅스-윈도우 트리 간 드리프트와 자기계약 위반을 잡는다.

## CI

`v*` 태그를 push하면 2종을 빌드해 해당 태그의 GitHub Release에 게시한다.

```bash
git tag v1.2.3 && git push origin v1.2.3
```

linux-x86_64와 windows-x86 둘 다 required — 없으면 릴리즈 실패한다. 릴리즈 전 계약 conformance가 강제된다: Linux는 빌드 잡에서 musl static을 네이티브 실행해, Windows는 `windows-latest` 러너에서 exe를 WoW64로 직접 실행해 emit 출력을 스키마로 검증한다. 어느 한쪽이라도 계약을 어기면 릴리즈가 막힌다.

## 트리

```
src/ include/ Makefile   Linux 소스 + 빌드
deploy/                  install.sh / systemd / sysv (바이너리에 embed)
scripts/                 build-linux.sh + 계약 검증(check-contract.sh, validate-wire.py)
schema/                  wire.schema.json (와이어 계약 기계검증 정본)
windows-agent/           Windows 소스 + 빌드 (single i686, 런타임 세대 dispatch, NT5.2 threads 패치)
.github/workflows/       태그 트리거 릴리즈 + 계약 conformance 게이트
```
