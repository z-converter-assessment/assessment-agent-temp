# 지원 OS 매트릭스

설치 대상 OS의 정본 목록이다. Linux install은 `deploy/lib/detect-os.sh`가 이 매트릭스로 호스트를 게이트하고, 미지원이면 한 줄 사유와 함께 이 문서를 안내한다. Windows는 NT 세대 게이트 없이 단일 바이너리가 커버한다.

관련 문서: 설치·운영은 [../README.md](../README.md), 빌드·툴체인은 [../docs/BUILD.md](../docs/BUILD.md).

## 왜 이 목록인가 (커버 전략)

- Linux는 musl 완전 static 단일 바이너리다. glibc 버전 의존이 없어 지원 경계는 glibc가 아니라 커널(>= 2.6.32)이다. 아래 배포판 목록은 "설치 스크립트가 인식·게이트하는 대상"이고, 실제 로드 가능 범위는 커널 하한만 넘으면 더 넓다.
- Windows는 단일 i686 바이너리 하나가 XP SP2 / Server 2003 SP1(NT5.1 SP2 / NT5.2 SP1)부터 최신(2016+/Win10-11, NT10)까지 커버한다. 32비트지만 64비트 Windows에서도 WOW64로 동작한다. 세대 분기는 런타임(agent_is_nt6 + GetProcAddress)이다.

## Linux (assessment-agent-linux-x86_64)

| 배포판 계열 | 버전 | init | 비고 |
| --- | --- | --- | --- |
| Ubuntu | 18.04 / 20.04 / 22.04 / 24.04 | systemd | |
| Debian | 10 / 11 / 12 / 13 | systemd | |
| RHEL / CentOS / Rocky / AlmaLinux / Oracle Linux | 6 / 7 / 8 / 9 | 6=SysV, 7+=systemd | EL6는 커널 2.6.32(SysV init). CentOS Stream 8/9 포함 |
| Amazon Linux | 2 / 2023 | systemd | |
| SUSE Linux Enterprise / openSUSE Leap | 11 / 12 / 15 (any SP) | 11=SysV, 12+=systemd | SLES 11은 커널 3.0.x(SysV init) |
| Tencent OS | 4.x | systemd | |

init 시스템은 install이 자동 감지한다 — systemd 호스트는 system 유닛(`/etc/systemd/system`), SysV 호스트(EL6/SLES11)는 `/etc/init.d`. 같은 바이너리, 같은 `install` 명령이다.

### os-release 부재 폴백 (구세대)

CentOS/RHEL/Oracle 6과 SLES 11은 `/etc/os-release`를 갖지 않는다. 이 경우 detect-os.sh가 `*-release` 마커 파일로 폴백한다.

- EL6: `/etc/redhat-release`(또는 centos/oracle/system-release)에서 "release 6" 토큰 매칭.
- SLES 11: `/etc/SuSE-release`에서 `VERSION = 11` 매칭.

os-release 경로는 systemd 세대 전 대상에 대해 항상 1차 소스로 남는다.

## Windows (assessment-agent-windows-x86.exe)

| 커버 범위 | 커널 세대 | 비고 |
| --- | --- | --- |
| Windows XP SP2 / Server 2003 SP1 | NT 5.1 SP2 / NT 5.2 SP1 | 실기 하한(NT5.2 로드 가드). 32비트 네이티브 |
| Windows Vista / 7 / 8 / 8.1, Server 2008 / 2008 R2 / 2012 / 2012 R2 | NT 6.0 ~ 6.3 | |
| Windows 10 / 11, Server 2016 / 2019 / 2022 | NT 10.0 | 64비트 Windows에서 WOW64로 동작 |

Windows는 관리자 명령프롬프트에서 `assessment-agent-windows-x86.exe install`을 1회 실행한다. LocalSystem 자동시작 서비스로 등록된다.

## 미지원 시

Linux install이 미지원 호스트를 만나면 한 줄 사유를 출력하고 중단한다. 커널 2.6.32 이상인데 위 목록에 없는 배포판이라면 바이너리 자체는 로드될 가능성이 높으나, 설치 스크립트의 서비스 등록 경로가 검증되지 않았다. 이 경우 매트릭스 확장은 detect-os.sh 게이트와 함께 논의한다.
