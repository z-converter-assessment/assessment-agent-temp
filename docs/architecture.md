# Assessment 수집 에이전트 — 아키텍처 개요

코드 라인이 아니라 아키텍처 관점에서 이 프로젝트를 설명한다. 큰 그림 -> 핵심 설계 결정 -> OS별 전략 차이 순으로 읽으면 된다.

---

## 1. 큰 그림

자체 소유·운영하는 fleet(사내 VM 다수)에서 두 가지를 한다: (1) 자산 인벤토리와 자원 지표를 수집·발행해 엔진이 fleet 을 평가(assessment)하게 하고, (2) 엔진 명령(task.install)을 받으면 워커가 ZDM(마이그레이션 에이전트)을 설치한다. 평가는 자산 구성 파악, 자원 사용 분석, 서비스 워크로드 분류, 용량 계획(right-sizing) 을 포괄한다. 평가가 마이그레이션으로 이어지고, ZDM 설치가 그 시작점이다.

```
  [ each server ]                 [ message bus ]        [ engine ]
  assessment-agent  --publish-->  RabbitMQ          -->  consumer -> DB
   (C, single binary)             (exchange                assess the fleet:
     inventory       ---------->    "assessment")          - inventory/asset
     metrics         ---------->  server.inventory         - resource usage
     error           ---------->  server.metrics           - workload class
                                  server.error             - right-sizing
   worker  <-- basic.get poll <-- agent.tasks.<agent_id>  (exchange
           --- task.result   -->  task.result routing key   "assessment.tasks")
```

성격: 방어적 IT 자산관리 + 마이그레이션 부트스트랩 도구다. 관측·발행과, 우리 소유 호스트에 우리 마이그레이션 에이전트(ZDM)를 설치하는 것이 동작 범위다 — 대상 호스트는 전부 우리가 소유·관리한다. 표준 인벤토리(포트-프로세스 매핑, 서비스 실행 경로)를 위해 root/LocalSystem으로 돈다 — netstat/ss/tasklist 와 같은 범주. ZDM 설치는 우리 소유 VM 에 우리 마이그레이션 에이전트를 관리자 동의 하에 배포하는 정규 구성관리다.

역할 분담이 이 시스템의 뼈대다:
- 에이전트: raw 관측값만 수집·발행한다(시계열 계산·상태 없음). 별도로 엔진 명령 시 ZDM 을 설치한다.
- 엔진: rate/delta/utilization 계산, 서비스 분류, fleet 평가를 한다.

---

## 2. 세 가지 아키텍처 원칙

이후 모든 설계가 이 셋에서 파생된다.

### A. 하나의 바이너리로 그 OS 계열 전체를 커버한다

OS별로 정확히 2종만 산출한다.

| 바이너리 | 커버 범위 | 전략 |
| --- | --- | --- |
| assessment-agent-linux-x86_64 | 전 x86_64 Linux (kernel >= 2.6.32, glibc 무관): EL6-9 / Ubuntu18+ / Debian10+ / SUSE11-15 / Amazon / Oracle | musl 완전 static |
| assessment-agent-windows-x86.exe | Windows XP SP2 / Server 2003 SP1 ~ 2016+/Win10-11 (NT5.2 ~ NT10) | 단일 i686 + 런타임 세대 분기 |

두 OS가 같은 목표("한 바이너리로 구세대부터 최신까지")를 전혀 다른 방법으로 푼다 — 4절에서 상술.

### B. 수집은 에이전트, 계산·평가는 엔진 (stateless 에이전트)

에이전트는 raw 누적 카운터(monotonic)만 싣는다. rate/delta/utilization/await 는 전부 엔진이 계산한다. 근거:
- 에이전트가 상태를 안 들면 재시작·재부팅에 정확도 손실이 없다(직전 샘플을 기억할 필요가 없음).
- 시계열 계산 로직이 엔진 한 곳에 모여 일관된다(에이전트 버전별 계산 드리프트 없음).
- 누적 카운터는 rate 의 시간적분이라, 엔진이 어떤 창(사이징 기준 14일)이든 Δcounter/Δwall 로 정확한 창 시간가중 평균을 복원한다. gauge 점표본(avg10 등)을 주기 표본하면 표본 사이 스파이크를 놓치고 평활 편향이 생겨 창 통계엔 부정확하다 — 그래서 창 판정용 신호는 counter 로 싣는다(대표 사례: PSI 를 ratio gauge 와 stall.time counter 두 형태로 발행, 14일 canonical 은 counter).

### C. value = 실측, null = 측정 불가 (가짜값 금지)

0을 포함한 값은 실측이고, OS에 개념이 없거나 커널/세대가 못 뽑는 필드는 null로 둔다. 절대 가짜값(0, 추정치)으로 채우지 않는다. 이 규칙이 4절의 버전 폴백과 직결된다.

---

## 3. 데이터 모델 (wire, 요지)

에이전트가 엔진으로 보내는 한 번의 발행은 "측정값 한 무더기"다. 그 무더기를 어떻게 담고(인코딩), 무엇을 담고(값), 시계열을 어떻게 이어붙이는지(안정키·identity)를 정한 게 wire 계약이다. 뼈대는 두 표준을 빌려왔다 — 무엇을 측정할지는 USE Method(Utilization/Saturation/Errors), 이름과 단위 컨벤션은 OpenTelemetry의 `system.*` 네임스페이스.

- 인코딩: 발행 페이로드는 datapoint-array, 즉 측정값(datapoint) 여러 개를 배열로 나열한 형태다. 하나의 지표(metric)는 `system.cpu.time` 같은 네임스페이스 이름 아래 `{type, unit, points:[{attr, value}]}` 로 담긴다 — `points` 한 칸이 datapoint 하나고, `attr`(어느 장치/코어/상태인지)로 구분되며 `value`가 실측치다. 배열 전체는 envelope(누가·언제·어떤 스키마로 보냈는지를 적은 봉투)로 감싸고 `schema_version:"1.0"` 을 박는다. 예: `system.cpu.time` metric 안에 `attr={cpu:"0", state:"user"}` point, `attr={cpu:"0", state:"idle"}` point ... 가 늘어선다. 방향(direction)·상태(state)·장치(device) 같은 차원을 metric 이름에 박지 않고 attr 로 직교 분해하는 게 datapoint-array 를 택한 이유다 — 상태나 장치가 늘어도 metric 이름(스키마 표면)은 그대로고 point 만 추가된다(OTel 관례). 필드/타입의 정본은 `schema/wire.schema.json`.
- 값: 에이전트는 raw 누적 카운터(부팅 이후 계속 증가하는 monotonic 값)만 싣는다. "초당 몇" 같은 비율(rate/delta/utilization/await)은 엔진이 두 샘플의 차이로 계산한다(원칙 2.B). metric 의 `type` 은 counter 와 gauge 둘뿐이다 — counter 는 rate 의 시간적분이라 엔진이 임의 창을 Δ로 복원하고, gauge 는 회수 가능 캐시량 같은 순간 스냅샷이다. 그리고 에이전트는 OS마다 제각각인 원시 단위 — CPU jiffies, 디스크 sector, Windows 100ns 틱, 퍼센트 — 를 전부 base 단위 세 가지(시간=seconds, 크기=bytes, 비율=0..1 ratio)로만 정규화해서 보낸다. 엔진은 단위를 다시 해석할 필요가 없다.
- device 안정키(id_type): 디스크·네트워크 지표는 장치마다 시계열이 갈리는데, 이 시계열을 묶는 키로 커널이 붙인 이름(`sda`, `dm-0`, `C:`)을 쓰면 안 된다 — 재부팅·디스크 순서 변경으로 이름이 바뀌면 엔진 입장에선 다른 장치로 보여 그래프가 끊긴다. 그래서 바뀌지 않는 식별자를 우선순위로 골라 쓴다: 디스크는 dm/uuid -> serial -> by-path -> name, 파티션은 partuuid; Windows 디스크는 gptid -> mbrsig -> serial, 볼륨은 volguid; 네트워크는 MAC 주소. 이름은 표시용으로만 남기고, 실제 키는 이 안정 id(id_type이 어떤 종류인지 함께 표기)다. metric 쪽 device attr 는 `<scheme>:<value>` 결합형(`serial:...`, `mac:...`, 시스템 전역은 `aggregate:system`), 인벤토리 토폴로지(block_devices/net_interfaces)는 id/id_type 분리형으로 같은 안정키를 노출한다.
- identity: 호스트를 구분하는 유일키는 hostname 이 아니라 `agent_id`(불변 UUID)다. 처음 실행될 때 한 번 만들어 state 디렉터리에 원자적으로(임시파일 + fsync + rename) 저장하고 이후엔 읽어 재사용한다 — 절단된 파일로 UUID 가 재발급되는 사고를 막으려는 의도. 덕분에 hostname·MAC·machine_id 가 바뀌어도(이미지 재배포, 볼륨 재사용) 엔진은 같은 호스트로 보고 같은 행을 업데이트한다. 보조로 machine_id(안정 소스 있을 때만, 없으면 null)와 composite_id(machine_id + MAC 지문의 해시라 machine_id 가 null 이어도 유니크가 유지됨)를 감사·표시용으로 함께 싣지만, 식별·라우팅엔 쓰지 않는다.
- USE 축: 사이징에 필요한 신호를 USE Method 세 축으로 나눠 수집한다. U(Utilization, 이용률 — 분모가 있어 "몇 % 찼나"를 낼 수 있는 값: disk io_time, memory available, net link.speed), S(Saturation, 포화 — 자원이 모자라 대기가 생긴 정도: Linux PSI 적분값이 정본이고 run_queue/commit 이 폴백), E(Errors, 오류 — 파싱은 하되 사이징 계산엔 반영하지 않는다. 오류가 있으면 계산 신뢰도를 오염시키므로 confidence 게이트로 격리).

---

## 4. 핵심: 하나의 바이너리로 전 OS를 커버하는 두 전략

큰 그림에서 가장 중요한 설계 결정이다. Linux 와 Windows 가 문제를 다르게 푼다.

### 4.1 Linux — 정적 링크로 의존성을 제거

전략: musl libc 로 완전 static 링크한 단일 바이너리. glibc 버전과 커널 API 에 대한 런타임 의존성이 아예 없다.

- alpine 컨테이너에서 musl static 빌드 -> PT_INTERP 없음, GLIBC 심볼 0. 동적 로더·libc 를 안 탄다.
- 그래서 EL6(커널 2.6.32, glibc 2.12)부터 최신 배포판까지 하나로 로드된다. glibc/커널 버전이 "제외 사유"가 아니다(SLES11 glibc 2.11, centos6 커널 2.6.32 실기 확인).

즉 Linux 쪽은 "구세대 커버"를 링크 단계에서 끝낸다. 로드 자체는 어디서나 성공한다.

### 4.2 Windows — 런타임 세대 분기 + 로드 가드

Windows 는 정적 링크로 안 풀린다(NT5.2 와 NT10 의 시스템 DLL/로더가 다름). 대신 하나의 i686 바이너리가 런타임에 세대를 분기한다.

- 왜 i686(x64 아님): 하나의 i686 바이너리는 32비트 전용인 NT5.x(XP SP2 / 2003) 실기에 네이티브로 로드되면서, 동시에 모든 x64 Windows 의 WoW64 위에서도 돈다. x64 바이너리는 32비트 전용 XP/2003 에 아예 로드가 안 된다. 즉 i686 이 XP SP2 ~ NT10 을 한 바이너리로 만족하는 최소공통 ISA 다. arch 리포팅은 바이너리 비트수와 무관하게 런타임 PROCESSOR_ARCHITECTURE 로 호스트 실측을 싣는다.
- 세대 판별: `agent_is_nt6()`(windows-agent/src/collect_util.c)가 RtlGetVersion(GetProcAddress 로 해소)으로 major>=6 을 런타임 확인하고 프로세스 1회 해소 후 캐싱한다. RtlGetVersion 을 쓰는 건 GetVersionEx 와 달리 application manifest 에 안 속는 실제 NT 버전을 주기 때문. 컴파일 타임 프로파일 인자가 없다.
- NT5.2 로드 가드가 핵심 제약이다: NT6+ 전용 API(GetIfTable2/QueryFullProcessImageNameW/inet_ntop 등)를 import 테이블에 하드링크하면 안 된다 — 하나라도 링크되면 XP/2003 로드 자체가 실패한다. 그래서 그런 API 는 전부 GetProcAddress 로 런타임 해소한다.
- 빌드도 이 제약을 강제한다: debian:bookworm 의 mingw(i686, gcc 12.2, CRT runtime 10.x)로만 빌드한다(CRT 12+ 는 verify 통과해도 XP/2003 실기 startup 이 드롭). 벤더 라이브러리(OpenSSL 1.0.2u/curl/rabbitmq-c)는 `_WIN32_WINNT=0x0502` 로 빌드해 NT6 API(SRWLock 등)를 피하고, rabbitmq-c 스레드는 NT5.2 패치(SRWLock -> CRITICAL_SECTION)로 교체한다. verify 가 (1)import DLL 이 시스템 DLL 뿐인지 (2)NT6+ 심볼 하드임포트가 없는지를 검사해 위반 시 빌드를 실패시킨다.

핵심 대조: Linux 는 "로드는 어디서나 되고 기능 유무만 갈린다", Windows 는 "로드부터 지켜야 하고(하드임포트 금지) 기능은 런타임에 분기한다".

---

## 5. 버전 때문에 호출이 불가능할 때의 폴백

원칙 C(가짜값 금지)와 4절의 두 전략이 여기서 만난다. 없는 기능은 null 이고, 폴백 방식이 OS 별로 다르다.

### 5.1 Linux — 커널 하한 미달은 null

바이너리는 모든 커널(2.6.32+)에 로드되지만, 기능은 커널 버전에 종속된다. 원천 파일이 없으면 그 필드를 null 로 둔다(가짜값 없음).

- PSI(`/proc/pressure/*`): 커널 4.20+. 없으면 `system.pressure = null`.
- MemAvailable(`/proc/meminfo`): 3.14+. 없으면 available 계열 null.
- oom_kill 카운터: 4.13+. virtio-net link speed: 드라이버 미제공 시 null.

즉 Linux 폴백 = "원천을 읽어보고 없으면 null". 세대 분기 코드가 거의 없다(파일 존재 여부가 곧 폴백).

### 5.2 Windows — 런타임 세대 분기 + 완전형 폴백

NT6+ 전용 API 는 GetProcAddress 로 해소하고, NT5.2 에서 null 이 나오면 그 세대가 쓸 수 있는 대체 경로로 폴백한다.

- inet_ntop: Vista+ export. NT5.2 엔 없으니 `compat_inet_ntop`(직접 구현한 완전형)로 폴백.
- 네트워크 인터페이스: NT6 는 GetIfTable2/GetIfEntry2, NT5.2 는 GetIfTable/GetIfEntry. link.speed 처럼 NT5.2 가 못 뽑는 축은 null.
- 프로세스 실행 경로: NT6 는 QueryFullProcessImageNameW(WOW64 안전), NT5.2 는 다른 경로 + 실패 시 null.
- 디스크 성능: throughput(io/operations)은 NtQuerySystemInformation 1차 + perflib 폴백. NtQuery 값은 I/O 매니저가 직접 유지하는 시스템 전역 누적이라 diskperf 드라이버와 독립·단조 -> virtio 등 diskperf 미수집 환경에서도 소스가 된다(per-disk perflib 키와 충돌 안 나게 device 는 `aggregate:system`). saturation(%util/await/queue)만 perflib PhysicalDisk per-disk 로 뽑되, perflib 는 diskperf 의존이라 raw!=0 & 단조일 때만 값·아니면 null(가짜 0 금지). io_time 은 절대시각 기준(1601 epoch)의 PerfTime100nSec 대신 uptime - idle 로 계산한다 — 재부팅 때 0 으로 리셋되어 엔진이 counter reset 을 깨끗이 감지하게 하려는 의도(절대시각은 425년 오프셋 + 재부팅 spike 를 유발). 시스템 레지스트리(EnableCounterForIoctl 등)는 안 건드린다(관측 전용).

즉 Windows 폴백 = "런타임 세대 판별 -> NT6 경로 시도 -> 안 되면 NT5.2 대체 경로 또는 null".

---

## 6. Windows vs Linux 바이너리 차이 (정리)

| 축 | Linux | Windows |
| --- | --- | --- |
| 커버 전략 | musl 완전 static (의존성 제거) | 단일 i686 + 런타임 세대 분기 |
| 커버 범위 | kernel 2.6.32+ (glibc 무관) | NT5.2 ~ NT10 |
| 빌드 툴체인 | alpine musl static | debian:bookworm mingw i686 (CRT v10) |
| 로드 가능성 | 어디서나 로드(정적) | 하드임포트 금지로 지켜야(NT5.2 로드 가드) |
| 세대 폴백 | 커널 하한 미달 -> null (파일 존재로 판정) | agent_is_nt6 + GetProcAddress -> 대체경로/null |
| 수집 원천 | procfs/sysfs 직접 읽기(외부 명령 안 씀) | Win32 API + IOCTL + Native API(NtQuery) + perflib(레지스트리 HKEY_PERFORMANCE_DATA) |
| OS 전용 신호 | PSI(S축), cgroup, swap(/proc/swaps) | saturation(perflib), pagefile(swap 노드), memory.edac는 null 패리티 |
| 계약 | 동일 wire 스키마 (필드셋 대칭) | 동일 wire 스키마 (필드셋 대칭) |

수집 원천의 성격 차이가 크다:
- Linux 는 커널이 텍스트/바이너리 파일로 노출한 것을 직접 파싱한다(/proc/stat, /proc/meminfo, /proc/pressure, /sys/block, /proc/mounts, /dev/disk/by-*). 외부 유틸(lsblk/df/ss)을 shell out 하지 않는다 — static 바이너리라 대상 호스트에 유틸이 있는지·PATH·로케일에 독립적이어야 하기 때문.
- Windows 는 커널이 파일로 안 주니 API/IOCTL/Native API 로 질의한다. 디스크는 PhysicalDrive 핸들 + IOCTL, 성능 카운터는 perflib(레지스트리 성능 데이터) + NtQuerySystemInformation.

---

## 7. 수집 구조 (collect 계층)

양 트리(Linux `src/`, Windows `windows-agent/src/`)가 동일하게 4계층 + 내부 헤더로 나뉜다.

| 파일 | 담당 |
| --- | --- |
| collect_model.c | wire 프리미티브 + envelope + identity(machine/agent/composite/mac) |
| collect_util.c | 파싱 유틸(proc/sysfs, perflib/NtQuery/IOCTL, 세대 판별, 안정키) |
| collect_metrics.c | system.* 수집기 + collect_metrics_payload |
| collect_inventory.c | os 서술자 + services/listen_ports + block_devices/net_interfaces |

계층 규칙: metrics 와 inventory 는 서로 호출하지 않고 model+util 만 단방향 의존. 공개 API 만 collect.h, 파일 간 공유 심볼은 collect_internal.h.

---

## 8. 발행 / 연결 모델

발행은 두 exchange 로 갈린다: 텔레메트리는 `assessment`, 워커 태스크·결과는 `assessment.tasks`. 라우팅 키는 넷이다.

- server.inventory (인벤토리 — 시작 시 1회 + inventory_refresh 주기)
- server.metrics (메트릭 — interval 주기)
- server.error (수집/발행 실패 이벤트)
- task.result (워커 태스크 완료 — assessment.tasks exchange)

주기·스케줄: metrics 기본 60s, inventory refresh 기본 3600s. inventory 데드라인엔 15% 지터를 얹는다 — fleet 전체가 같은 순간에 재발행해 브로커를 때리는 thundering herd 를 흩뜨리려는 의도. 스레드는 없다. 단일 프로세스 단일 루프가 metrics 발행과 worker_tick 을 같은 회전에서 처리하고, 태스크 실행만 자식(Linux fork / Windows thread)으로 분리한다. 긴 metrics interval 동안 sleep 을 25초 청크로 쪼개 그 사이 워커 AMQP 연결에 heartbeat 를 흘려보내(pump) 연결이 heartbeat timeout 으로 죽지 않게 한다.

발행 연결(inventory/metrics/error): 발행마다 단명(ephemeral) 연결이다 — connect -> login(SASL PLAIN) -> channel 1 -> confirm.select -> publish -> confirm -> close. exchange 는 passive declare(선언이 아니라 존재 확인)하고, 새 연결이라 confirm 대기 seqno 는 항상 1이다. 저빈도(60s/3600s)라 연결 오버헤드가 무의미하고, stale 연결 상태관리를 통째로 없애 robustness 를 얻는 의도적 선택. inventory/metrics 는 발행 실패 시 백오프(1s 배수, interval 상한) 무한 재시도 후 성공하면 복구 알림(PUBLISH_RECOVERED)을 발행하고, error 는 재시도 없이 1회다.

워커 연결(task.install -> ZDM 설치): persistent 연결 하나를 붙들고 tick 마다 재사용한다. 엔진이 task.install 을 발행하면 워커가 자기 큐(agent.tasks.<agent_id>)를 basic.get 폴링해 수신하고, tarball 다운로드 -> sha256 검증 -> 추출 -> installer 실행으로 ZDM 을 설치한 뒤 task.result 를 발행한다. 큐는 엔진이 첫 task 때 lazy 생성하므로 워커는 큐 부재(404)를 에러가 아니라 "대기"로 구분한다. 채널 예외는 전체 reconnect 없이 채널 1만 재오픈해 복구하고, 브로커 다운 시엔 워커를 죽이지 않고 백오프(상한 60s)로 재연결한다.

설치 흐름의 안전장치(의도):
- single-slot: 자식이 살아 있으면 새 태스크를 안 뽑는다(동시 설치 금지).
- 멱등 3중 게이트: 상태 디렉터리(done/results/running)로 이미 끝난 태스크의 재실행·재발행과 redelivery 재설치를 막는다. 크래시로 중단된 install 은 시작 시 복구 경로가 internal_error 결과를 합성·발행한다.
- 다운로드 호스트 화이트리스트(WORKER_DOWNLOAD_ALLOWED_HOSTS): CSV 정확 일치(서브도메인 와일드카드 없음), 빈 값이면 전부 거부. sha256 뿐 아니라 파일 크기까지 정확 일치를 요구한다.
- 신뢰성: publisher confirm + persistent 메시지(delivery_mode=2). 발행 성공 후에만 ack·done 처리해서, ack 를 놓쳐도 브로커 redeliver + 멱등 게이트로 이중 설치가 안 난다.
- fd 격리: 설치 자식이 브로커 소켓 등 부모 fd 를 상속하지 않게 Linux 는 fork 직후 fd 를 전부 닫고(FD_CLOEXEC + close-range sweep), Windows 는 스레드 모델이라 자식에 넘기는 파이프 핸들에 상속 금지 플래그를 건다.

---

## 9. 배포 / 권한 모델

- 권한: install·런타임 계정을 전부 최고 권한(root/LocalSystem)으로 통일한다. 근거 — 타 유저 소유 프로세스의 `/proc/<pid>/exe`·`comm` 을 읽어야 `listen_ports[].pid`·`services[].exe` 가 채워진다. 비특권이면 sshd/nginx/mysql 등 대부분 데몬의 포트-PID 매핑이 null 이 된다.
- Linux: systemd(User=root) 또는 SysV(EL6, /etc/init.d root 직접). Windows: LocalSystem 자동시작 서비스.
- Windows 오프라인 주입: WinRM/cloudbase-init 없이 배포한다. 이미지 -> bootable volume -> bastion NTFS 마운트 -> exe 복사 + SYSTEM hive 에 서비스(auto)+시스템 env 오프라인 주입(hivex) -> boot-from-volume 재생성 -> 부팅 시 서비스 자동.
- identity 영속: agent_id 는 state dir(Windows %ProgramData%, Linux /var/lib 또는 ~/.local/state)에 원자적으로 쓴다. 바이너리 install dir 과 분리돼 있어, 바이너리만 교체하는 재배포(fleet 의 volume 재사용)는 id 를 안 건드린다 -> 엔진이 같은 행 업데이트. 이미지 클론 시엔 prep-image 가 id 를 지워 클론마다 새 id 를 받게 한다.
- machine_id 비대칭: Linux 는 machine_id 안정 소스(/etc/machine-id·dbus·IMDS)가 없어도 MAC 기반 composite_id 로 유니크를 유지하며 계속 뜬다. Windows 는 레지스트리 MachineGuid 가 없으면 치명 종료하고 MACHINE_ID_UNRESOLVED 를 발행한다 — Windows 에선 MachineGuid 부재가 정상 상태가 아니라 이미지/레지스트리 손상 신호라서다.

---

## 10. 품질 게이트

- 계약 conformance(CI): 두 바이너리의 `emit` dry-run 4종(inventory/metrics/task.result/error)을 wire 스키마로 강제한다. Linux 는 빌드 잡에서 musl static 네이티브 실행, Windows 는 windows-latest 러너에서 WoW64 로 exe 직접 실행. 어느 한쪽이라도 계약을 어기면 릴리즈가 막힌다.
- 2트리 대칭: Linux/Windows 필드셋 드리프트를 두지 않는다. 한쪽에 신호를 추가하면 다른 쪽도 실측 발행하거나 측정불가 null 로 필드셋을 맞춘다(예: memory.edac 는 Windows 도 null 2점).
- 실기 검증: 실제 하드웨어 테스트베드에서 값을 ground truth(/proc·lsblk·WMI)와 대조하고 다중 샘플 counter 단조를 확인한다(신/구 커널 Linux, NT5.2 win2003, NT6 win2012R2 등).

---

## 11. 비용과 트레이드오프 (의도적 선택)

- 2트리 유지비 2배: 스키마·로직 변경을 Linux/Windows 양쪽에 반영한다. 단일 크로스플랫폼 추상화 대신 각 OS 의 커널 인터페이스를 직접 다루는 대가로, 세대·OS 별 정확성과 로드 안전성을 얻는다.
- 발행마다 연결·basic.get 폴링: AMQP 관용(연결 재사용·basic.consume)과는 다르지만, 저빈도 워크로드에선 단순성·robustness 가 이득이다. 발행 빈도가 크게 오르면 재고 대상.
- 가짜값 금지의 대가: 못 뽑는 필드가 null 로 많이 남을 수 있으나(구세대·구커널), "0 과 측정불가를 구분"하는 게 평가 정확성(사이징 포함)의 전제라 이를 우선한다.

---

## 요약 한 줄

한 OS 계열을 바이너리 하나로 커버하되 — Linux 는 musl static 으로 의존성을 제거하고, Windows 는 런타임 세대 분기 + 로드 가드로 구세대 로드를 지킨다. 에이전트는 raw 관측값만 발행하고(stateless), 못 뽑는 건 가짜값 없이 null 로 둔다. 계산·분류·평가는 엔진의 몫이다.
