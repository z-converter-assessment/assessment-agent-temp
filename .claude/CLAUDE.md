# assessment-agent-temp

Assessment 수집 에이전트(C)의 릴리즈 빌드 트리. 단일 소스에서 OS별 2종 바이너리(Linux 1종 + Windows 1종)를 산출한다. 자체 소유 fleet의 자산 인벤토리와 자원 지표를 수집해 용량 계획(right-sizing)에 쓴다.

관련 문서: 아키텍처 개요는 [docs/architecture.md](../docs/architecture.md), 설치·운영은 [README.md](../README.md), 빌드·릴리즈·CI는 [docs/BUILD.md](../docs/BUILD.md)(빌드 이식성 개념 배경은 [docs/portability-concepts.md](../docs/portability-concepts.md)), wire 계약은 [schema/wire.schema.json](../schema/wire.schema.json)(정본)과 [docs/payload-contract.md](../docs/payload-contract.md)(산문), 스토리지 레이아웃 파싱은 [docs/storage-layout.md](../docs/storage-layout.md).

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

### collect 소스 구성 + 코드 컨벤션 (신규 수집 코드는 이 규칙을 따른다)

수집기는 양 트리 대칭으로 4계층 파일 + 내부 헤더로 나눈다. 빌드는 `wildcard src/*.c`라 파일 추가 시 Makefile 수정 불요.

| 파일 | 담당 | 링크 |
| --- | --- | --- |
| collect_model.c | wire 프리미티브 + envelope + identity(machine/composite/agent/cloud/mac) | 최하층 |
| collect_util.c | 파싱 유틸(proc/sysfs, mounts, device-id, net, perflib/NtQuery/IOCTL 등) | 최하층 |
| collect_metrics.c | system.* 수집기 + collect_metrics_payload + build_error_payload | model+util 의존 |
| collect_inventory.c | os 서술자 + services/listen_ports + block_devices/net_interfaces + collect_inventory_payload | model+util 의존 |
| include/collect_internal.h | collect_*.c 공용 내부 선언(model+util) + 공유 struct/매크로 | 공개 API 는 collect.h |

- 계층 규칙: metrics 와 inventory 는 서로 호출하지 않는다. 둘 다 model+util 만 의존(단방향). 두 곳에서 쓰는 헬퍼는 util(공유 파싱)이나 model 로 올린다. 공개 API 만 collect.h, 파일간 공유 내부 심볼은 non-static + collect_internal.h 선언.
- 네이밍 스킴(양 트리 통일): wire 프리미티브 `wire_*`(wire_ns/wire_metric/wire_point/wire_point_attr/wire_point_value/wire_point_null/wire_metric_scalar/wire_add_envelope), metrics 수집기 `metrics_collect_*`, inventory 수집기 `inv_collect_*`. 파싱 헬퍼는 서술적 이름 유지(disk_device_id 등).
- datapoint 발행: device(+direction)+value 패턴은 `wire_point_dev_dir(metric, device, direction, value)` 한 줄로. 단일값은 `wire_metric_scalar(ns, name, type, unit, have, value)`. 조건부 null 등 헬퍼에 안 맞는 경우만 wire_point + wire_point_attr + wire_point_value/wire_point_null 를 쓴다.
- 코드 스타일: 한 줄에 한 문장(세미콜론으로 여러 문장 뭉치지 않는다). 3회 이상 반복되는 발행 패턴은 헬퍼로 추출. collect 계층 주석은 한국어(다이어그램 제외).
- 2트리 대칭: 파일 레이아웃/네이밍/필드셋을 Linux(`src/`)와 Windows(`windows-agent/src/`) 동일하게 유지. 한쪽에 신호를 추가하면 다른 쪽도 실측 발행하거나 측정불가 null 로 필드셋을 맞춘다(예: memory.edac 는 Windows 도 null 2점 발행).

## 불변식 (반드시 준수)

### wire 계약과 null 의미론

- 값(0 포함)은 실측이고 null은 측정 불가다. OS에 개념이 없는 필드를 가짜값으로 채우지 않고 null로 둔다.
- 정본은 `schema/wire.schema.json`이다. 스키마나 직렬화를 바꾸면 정본도 같이 고친다. 메시지별 필드 셋 상세는 스키마 `$comment`와 `docs/payload-contract.md`에 있다.
- CI가 두 바이너리의 `emit` dry-run 4종(inventory/metrics/task.result/error)을 이 스키마로 검증한다(`scripts/check-contract.sh`). 필드/타입/null과 os_family 조건부(saturation은 Windows 전용, task.result의 Windows os_codename=null 등)를 어기면 릴리즈가 막힌다.
- 2트리 간 필드 셋 드리프트를 두지 않는다. Windows 소스 변경의 정당한 용처는 그 OS가 실측 가능한 값을 세대 무관하게 뽑는 것이지, 리눅스 필드를 흉내내는 값 위조가 아니다.

### 계약 버저닝 (시스템 통일 major 축)

- 계약 버전은 `schema_version`(=`agent_version` 아님; agent_version은 릴리즈/빌드 정체성). 값 형식 `"major.minor"`, 현재 `"1.0"`. 엔진 `contract.py` CONTRACT_VERSION이 정본이고 wire/assessment API/export/task.install 이 이 한 major 축을 공유한다.
- additive 변경(필드/enum/축 추가, 삭제·타입·의미 변경 없음)은 버전을 올리지 않는다 — 소비자가 미지 필드를 관용하고 optional/null로 흡수한다. 이번 인벤토리 확장이 여기 해당한다. 구조 파괴 변경만 major 범프이며 엔진+에이전트+DR 동시 flag-day로 전환한다.
- 인바운드 task.install(특권 실행 명령)은 major 게이트를 강제한다: 페이로드 최상위 `schema_version`의 major가 에이전트가 아는 값(1)과 다르거나 부재면 다운로드/실행 전 거부하고 task.result `failure_reason="unsupported_contract_version"`으로 되받는다(`worker.c` `schema_version_major_ok`). 의미 드리프트를 최고권한으로 실행하지 않는다. 롤아웃은 엔진이 필드를 먼저 배선/배포한 뒤 활성.

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

## wire 계약

wire 계약은 schema_version `"1.0"`이다 — `schema/wire.schema.json`이 정본, 산문은 `docs/payload-contract.md`, 대표 예시는 `docs/wire-examples.json`, 사이징 해석 의도는 `docs/classification-rationale.md`. USE Method + OTel system.* 정렬 datapoint-array 인코딩. `schema_version`은 시스템 통일 계약 버전으로 엔진 `contract.py` CONTRACT_VERSION 이 정본이고 wire/assessment API/export/task.install 이 한 major 축을 공유한다(major 게이트). 양 트리(Linux/Windows) 구현이 CI check-contract 4종과 testbed 실측으로 검증된다. 구현이 반드시 지키는 불변식:

- 인코딩: payload는 datapoint-array다. system.* 네임스페이스 -> metric `{type,unit,points:[{attr,value}]}`. 4종 메시지 모두 envelope + `schema_version:"1.0"`. metrics/inventory 는 system.*/block_devices, task.result 는 실행 결과 body(+task_policy), error 는 실패 이벤트 body.
- 값: 에이전트는 raw 누적 카운터만 싣고 rate/delta/util/await는 엔진이 계산한다(stateless). base 단위 = seconds/bytes/ratio(0..1). jiffies/sectors/100ns/%를 에이전트에서 정규화한다.
- device 안정키(id_type): 시계열 자연키는 이름이 아니라 안정 id다. Linux 디스크 dm/uuid -> serial -> by-path -> name, 파티션 partuuid -> name; Windows 디스크 gptid -> mbrsig -> serial -> name, 볼륨 volguid. 네트워크 = MAC(폴백 by-path/name). wwid/by-id/fsuuid 는 id_type enum 엔 있으나 현재 producer 미발행. name 은 표시용.
- Windows 디스크 성능: throughput(io/operations)은 NtQuerySystemInformation(diskperf 독립·단조) 1차 + perflib 폴백을 유지한다. saturation(%util/await/queue)만 죽은 IOCTL_DISK_PERFORMANCE 대신 perflib PhysicalDisk로 뽑되, perflib는 diskperf 의존이라 raw!=0 & 단조일 때만 값·아니면 null(가짜 0 금지). EnableCounterForIoctl 등 시스템 레지스트리를 에이전트가 변경하지 않는다(관측 전용).
- USE 축: U는 분모 있는 이용률(disk io_time, memory available, network link.speed), S는 PSI(14일 canonical = `pressure.stall.time` 적분) + run_queue/commit 폴백, E는 완전체로 파싱하되 사이징에 미반영(엔진 confidence 오염 게이트 + attention).
- 커널/드라이버 하한 미달은 null(PSI 4.20+, MemAvailable 3.14+, oom_kill 4.13+, virtio-net link speed). 값 위조 없음.
