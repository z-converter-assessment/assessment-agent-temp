# 빌드 / 릴리즈

단일 소스에서 OS별 2종 바이너리를 산출한다(Linux 1종 + Windows 1종). 산출물의 설치·사용법은 [../README.md](../README.md), wire 계약은 [payload-contract.md](payload-contract.md)를 본다.

| 파일 | 타깃 | 프로파일 | 툴체인 |
| --- | --- | --- | --- |
| assessment-agent-linux-x86_64 | 전 x86_64 Linux (kernel >= 2.6.32, glibc 무관): EL6-9 / Ubuntu18+ / Debian10+ / SUSE11-15 / Amazon / Oracle / Tencent | musl static | alpine musl |
| assessment-agent-windows-x86.exe | Windows XP SP2 / Server 2003 SP1 ~ 2016+/Win7-11 (NT 5.1 SP2 ~ NT 10.0) | single i686 | debian:bookworm (gcc 12.2, CRT v10) |

## 빌드

### Linux

Alpine 컨테이너에서 musl 완전 static으로 빌드한다. 단일 바이너리가 glibc 버전과 무관하게 커널 2.6.32 이상 전 x86_64 리눅스를 커버한다(EL6 2.6.32, SLES11 3.0.13 실기 확인).

```bash
sh scripts/build-linux.sh
```

산출물은 `dist/assessment-agent-linux-x86_64`. `build-linux.sh`가 컨테이너 안에서 `make clean` -> `vendor-build`(musl) -> `release`를 돌리고, verify가 완전 static(PT_INTERP 없음, GLIBC 심볼 0)을 확인한다. 빌드 이미지는 `BUILD_IMAGE`(기본 `alpine:3.19`)로 바꿀 수 있다.

`deploy/`와 `scripts/image-prep.sh`는 objcopy로 이 바이너리에 embed되므로, 그 파일을 고치면 Linux 바이너리를 재빌드해야 반영된다.

### Windows

단일 i686 바이너리로 XP SP2 / Server 2003 SP1부터 NT10(2016+/Win10-11)까지 커버한다. 세대 분기는 런타임(`windows-agent/src/collect.c`의 `agent_is_nt6` + GetProcAddress)이라 프로파일 인자가 없다. debian:bookworm 컨테이너로만 빌드한다 — bookworm의 mingw-w64(i686, gcc 12.2, CRT runtime 10.x)가 NT5.2 startup을 유지한다. CRT 12+(ubuntu 최신 등)로 빌드하면 verify는 통과해도 XP/2003 실기 로드가 안 된다.

```bash
cd windows-agent
docker run --rm --network host -v "$PWD":/w -w /w debian:bookworm bash -c '
  apt-get update && apt-get install -y gcc-mingw-w64-i686 binutils-mingw-w64-i686 cmake make git perl
  make vendor-build && make release'
```

빌드 규칙:

- agent 소스는 `_WIN32_WINNT=0x0600`으로 컴파일해 NT6 구조체(MIB_IF_ROW2, GAA gateway/prefix)를 선언하되, NT6 전용 함수는 하드임포트하지 않고 GetProcAddress로 해소한다.
- 벤더 라이브러리(OpenSSL 1.0.2u / curl / rabbitmq-c)는 `0x0502`로 빌드한다(SRWLock 등 NT6 API 회피). Makefile이 rabbitmq-c의 win32/threads를 NT5.2 패치(`windows-agent/patches/nt52-threads/`, SRWLock -> CRITICAL_SECTION)로 자동 교체한다.
- verify가 (1) import DLL이 시스템 DLL뿐인지, (2) NT6+ 심볼이 하드임포트되지 않았는지(NT5.2 로드 가드)를 검사해 위반 시 빌드를 실패시킨다.
- 실기 하한은 XP SP2 / Server 2003 SP1이다 — 수집기가 GetExtendedTcpTable/GetExtendedUdpTable(리슨 포트 소유 PID)과 GetSystemTimes(CPU 시간)를 하드임포트하는데 이들이 그 SP부터 export되기 때문이다. NT6+ 심볼만 보는 verify denylist는 이 SP 레벨 하한을 잡지 못한다.
- OpenSSL 1.0.2u(EOL)를 전 세대에 쓴다 — XP/2003 TLS 호환을 위해 감수한다. TLS 1.3이 없어, 엔드포인트 TLS 하드닝 시 전 Windows가 동시에 끊긴다.

win2003 실기 배포는 hive ComputerName + Tcpip MTU=1280 + EnablePMTUBHDetect=1(GUID 무관 PMTU black-hole 감지, 오버레이 경로에서 AMQP 핸드셰이크 블랙홀 회피) 운영 설정을 사용한다.

## 계약 conformance

wire 계약의 기계검증 정본은 `schema/wire.schema.json`(JSON Schema)이고 산문 설명은 [payload-contract.md](payload-contract.md)다. 두 바이너리는 `emit` dry-run 서브커맨드로 실제 직렬화 코드를 태워 한 페이로드를 stdout에 출력한다(발행/MQ/TLS 없음). 대상은 inventory / metrics / task.result / error 4종이다.

```bash
scripts/check-contract.sh dist/assessment-agent-linux-x86_64            # 네이티브
scripts/check-contract.sh dist/assessment-agent-windows-x86.exe wine    # 로컬 wine
```

`check-contract.sh`가 4종 `emit` 출력을 스키마로 검증한다(`scripts/validate-wire.py`). 필드/타입/null 의미론과 os_family 조건부(saturation은 Windows 전용, task.result의 Windows os_codename=null 등)를 강제해, 리눅스-윈도우 트리 간 드리프트와 자기계약 위반을 잡는다.

## 릴리즈 (CI)

`.github/workflows/release.yml`이 `v*` 태그 push를 받아 2종을 빌드하고 해당 태그의 GitHub Release에 게시한다(softprops upsert). 릴리즈는 최신 단일 태그 하나로 유지하고, 재릴리즈는 그 태그를 덮어쓴다.

```bash
git tag -f vX.Y.Z && git push -f origin vX.Y.Z
```

linux-x86_64와 windows-x86 둘 다 required — 없으면 릴리즈가 실패한다(best-effort 항목 없음). 릴리즈 전 계약 conformance가 강제된다: Linux는 빌드 잡에서 musl static을 네이티브 실행해, Windows는 `windows-latest` 러너에서 exe를 WoW64로 직접 실행해 `emit` 출력을 스키마로 검증한다. 어느 한쪽이라도 계약을 어기면 릴리즈가 막힌다.

재릴리즈 전에는 이전에 실행 중인 GitHub Actions의 성공/실패를 확인하고 대기한다.

## 저장소 트리

```
src/ include/ Makefile   Linux 소스 + 빌드
deploy/                  install.sh / systemd / sysv (바이너리에 embed)
scripts/                 build-linux.sh + 계약 검증(check-contract.sh, validate-wire.py)
schema/                  wire.schema.json (wire 계약 기계검증 정본)
windows-agent/           Windows 소스 + 빌드 (single i686, 런타임 세대 dispatch, NT5.2 threads 패치)
.github/workflows/       태그 트리거 릴리즈 + 계약 conformance 게이트
```

로컬 빌드 아티팩트(`vendor/`, `dist/`, `build/`, `*.o`, `*.exe`, `*.res`)는 `.gitignore` 대상이다. 추적 파일만 커밋한다.
