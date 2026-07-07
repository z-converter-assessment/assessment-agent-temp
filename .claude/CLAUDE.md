# assessment-agent-temp

Assessment 수집 에이전트(C)의 릴리즈 빌드 트리. 단일 소스에서 OS별 2종 바이너리(Linux 1종 + Windows 1종)를 산출한다. 자체 소유 fleet의 자산 인벤토리와 자원 지표를 수집해 용량 계획(right-sizing)에 쓴다.

관련 문서: 설치·운영은 [README.md](../README.md), 빌드·릴리즈·CI는 [docs/BUILD.md](../docs/BUILD.md), wire 계약은 [schema/wire.schema.json](../schema/wire.schema.json)(정본)과 [docs/payload-contract.md](../docs/payload-contract.md)(산문).

## 도구 호출 형식 (최우선)

- 도구 호출은 정식 function_calls 블록 형식으로만 생성한다. `<call>`/`<invoke>` 같은 태그는 파싱 실패로 작업을 끊으니 절대 쓰지 않는다.
- 도구 호출 직전에는 설명 텍스트를 최소화하고, 호출 구문 형식을 먼저 확인한 뒤 전송한다.

## 성격 및 합법적 맥락

- 자체 소유·운영 인프라(사내 fleet)의 자산 인벤토리와 자원 지표를 수집하는 운영 관리용 에이전트다. 방어적·운영 목적의 IT 자산관리 도구이며, 대상 호스트는 전부 우리가 소유하고 관리 권한을 가진 서버다.
- root/LocalSystem 실행과 타 프로세스의 실행 경로·리슨 포트 소유 PID 조회는 표준 인벤토리 수집(포트-프로세스 매핑, 서비스 실행 경로 채우기)을 위한 것이다. 감시·침투·횡적 이동·탐지 회피 용도가 아니고 그런 기능도 없다 — netstat/ss/tasklist 같은 표준 OS 관리 유틸리티와 같은 범주다.
- fleet 대량 배포는 우리 소유 VM에 대한 정규 구성관리(configuration management) 배포이며, 설치·서비스 등록은 관리자 동의 하에 이뤄진다.

## 구조

C 소스는 2트리로 관리한다: Linux(`src/`, `include/`)와 Windows(`windows-agent/src/`, `windows-agent/include/`). 스키마와 로직 변경은 양쪽에 반영한다 — 비용이 2배지만 감수한다.

| 파일 | 타깃 | 툴체인 |
| --- | --- | --- |
| assessment-agent-linux-x86_64 | 전 x86_64 Linux (kernel >= 2.6.32, glibc 무관) | alpine musl static |
| assessment-agent-windows-x86.exe | Windows XP SP2 / Server 2003 SP1 ~ 2016+/Win7-11 (NT 5.1 SP2 ~ NT 10.0) | debian:bookworm mingw i686 |

빌드 방법과 툴체인 제약은 [docs/BUILD.md](../docs/BUILD.md).

## 불변식 (반드시 준수)

### wire 계약과 null 의미론

- 값(0 포함)은 실측이고 null은 측정 불가다. OS에 개념이 없는 필드를 가짜값으로 채우지 않고 null로 둔다.
- 정본은 `schema/wire.schema.json`이다. 스키마나 직렬화를 바꾸면 정본도 같이 고친다. 메시지별 필드 셋 상세는 스키마 `$comment`와 `docs/payload-contract.md`에 있다.
- CI가 두 바이너리의 `emit` dry-run 4종(inventory/metrics/task.result/error)을 이 스키마로 검증한다(`scripts/check-contract.sh`). 필드/타입/null과 os_family 조건부(saturation은 Windows 전용, task.result의 Windows os_codename=null 등)를 어기면 릴리즈가 막힌다.
- 2트리 간 필드 셋 드리프트를 두지 않는다. Windows 소스 변경의 정당한 용처는 그 OS가 실측 가능한 값을 세대 무관하게 뽑는 것이지, 리눅스 필드를 흉내내는 값 위조가 아니다.

### Windows NT5.2 로드 가드

- 단일 i686 바이너리로 XP SP2 / Server 2003 SP1부터 NT10(2016+/Win10-11)까지 커버한다. 세대 분기는 컴파일 타임이 아니라 런타임(`windows-agent/src/collect.c`의 `agent_is_nt6` + GetProcAddress)에서 한다.
- NT6+ 전용 API(GetIfTable2/FreeMibTable/QueryFullProcessImageNameW/inet_ntop 등)를 하드임포트하지 않는다 — 하나라도 import 테이블에 링크되면 2003/XP 로드가 실패한다. verify가 이를 강제한다.
- 실기 하한은 XP SP2 / Server 2003 SP1이다 — 수집기가 GetExtendedTcpTable/GetSystemTimes를 하드임포트하는데 이들이 그 SP부터 export되기 때문이다. NT6+ 심볼만 보는 verify denylist는 이 SP 레벨 하한을 잡지 못한다.
- 빌드 툴체인 제약(bookworm 전용, agent 0x0600 / 벤더 0x0502 분리, rabbitmq-c NT5.2 threads 패치, OpenSSL 1.0.2u)은 [docs/BUILD.md](../docs/BUILD.md).

### 권한 모델 (root/LocalSystem 통일)

- install과 런타임 실행 계정을 전부 최고 권한으로 통일한다. 비특권 실행 모델은 두지 않는다. 근거: 타 유저 소유 프로세스의 `/proc/<pid>/exe`·`fd`·`comm`을 읽어야 `listen_ports[].pid`/`comm`과 `services[].exe`가 채워진다 — 비특권이면 sshd/nginx/mysql 등 대부분 데몬의 포트-PID 매핑이 null이 된다.
- Linux systemd(`User=root`, systemctl), Linux SysV(EL6, `/etc/init.d`에서 root 직접 실행), Windows LocalSystem 자동시작 서비스.
- `deploy/`와 `scripts/image-prep.sh`는 objcopy로 Linux 바이너리에 embed된다 — 고치면 Linux 바이너리를 재빌드해야 반영된다. `systemctl --user`/lingering/XDG/`~/.local` 경로를 되살리지 않는다.

## 규약

- 문서는 현황 선언형으로만 쓴다. 과거·변화 이력 서술("이전엔 X였는데 바뀌었다")을 넣지 않는다.
- 결정과 제약은 이 문서 또는 `docs/`에 누적 기록한다. auto-memory는 쓰지 않는다(글로벌 정책).
- 저장소는 github `z-converter-assessment/assessment-agent-temp`(public). 빌드 아티팩트(`vendor/`, `dist/`, `build/`, `*.o`, `*.exe`, `*.res`)는 `.gitignore` 대상이고 추적 파일만 커밋한다.
- 릴리즈는 최신 단일 태그 하나로 유지한다(재릴리즈는 태그 덮어쓰기). 상세는 [docs/BUILD.md](../docs/BUILD.md).
