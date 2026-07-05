# payload 계약 (에이전트 -> 엔진)

에이전트가 RabbitMQ 브로커로 발행하는 wire 계약. 이 문서는 산문 설명이고,
기계검증 가능한 정본(source of truth)은 `schema/wire.schema.json`(JSON Schema)이다.
CI가 두 바이너리의 `emit` dry-run 출력(inventory / metrics / task.result / error 4종 전부)을 이
스키마로 강제해(`scripts/check-contract.sh`), 리눅스-윈도우 트리 간 드리프트와 자기계약 위반을
릴리즈 전에 잡는다. 로컬 검증:
`scripts/check-contract.sh dist/assessment-agent-linux-x86_64` /
`... dist/assessment-agent-windows-x86.exe wine`.

공통 스키마이되, 아래 "OS별 차이"에 명시된 분기(예: metrics.saturation은 Windows 전용)는
`os_family`로 갈리며 스키마의 조건부(if/then)로 강제된다. 값 의미론(값=실측, null=측정불가)도
스키마가 nullable 여부로 인코딩한다.

## 메시지 종류와 라우팅 키

| message_type | 라우팅 키 | 주기 |
|---|---|---|
| inventory | server.inventory | 시작 시 + inventory_refresh 간격 |
| metrics | server.metrics | AGENT_INTERVAL_SEC 간격 |
| error | server.error | 수집 실패 시 |
| task.result | task.result | worker task 완료 시 |

worker는 `agent.tasks.{agent_id}` 큐를 구독하고, 엔진은 그 큐로 task를 발행한다.

## 공통 메타데이터 (모든 메시지)

message_type, message_id, hostname, machine_id, composite_id, agent_id,
agent_version, boot_time, collected_at, agent_started_at, os_family.

## 식별과 라우팅

- agent_id: 첫 실행 시 생성해 영구 저장하는 UUID(Linux state dir, Windows `%ProgramData%\assessment-agent\agent-id`). MAC/machine_id 재발급과 무관하게 불변. 엔진 식별키이자 worker 큐 키.
- composite_id, machine_id, mac_addresses: 감사, 표시용. 식별, 라우팅에는 쓰지 않는다. machine_id는 안정적 소스(/etc/machine-id, dbus, 클라우드 IMDS, Windows MachineGuid)가 없으면 억지로 채우지 않고 null이다(예: non-systemd+비클라우드 리눅스). composite_id는 machine_id가 없어도 mac 기반으로 유니크가 유지된다.
- prep-image(양 OS) + Linux image-prep.sh가 agent-id를 삭제해 골든 이미지 클론마다 새 UUID를 받게 한다.

## agent_version

payload agent_version은 스키마 계약 버전이 아니라 릴리즈/빌드 정체성이다.
CI가 git 태그에서 주입한다(`v1.0.0` -> `1.0.0`, release.yml). 로컬/dev 빌드는 `0.0.0-dev`.
엔진은 이 값을 스키마 분기 gate로 쓰지 않고 opaque 메타데이터(관측/낙오 노드 추적)로 저장한다.
계약 변경은 이 값이 아니라 필드/큐 구조로 표현된다.

## inventory 필드

공통 메타데이터 + 아래.

| 필드 | 내용 |
|---|---|
| os_id, os_version, os_codename | OS 배포 식별 |
| cpu_cores, cpu_model | CPU |
| kernel_version | 커널 |
| mem_total_kb, swap_total_kb | 메모리/스왑 총량 |
| disks[] | 물리 디스크. name/kind/major/minor/size_bytes/type |
| mounts[] | 마운트 구조. mount/major/minor/total_bytes/fstype/kind. 동적 사용량(free/avail)은 싣지 않는다(metrics 전담) |
| services[] | 서비스. unit/sub/pid/exe. 열거 인프라에 접근 불가면 배열 대신 null (비-systemd Linux / SCM 접근(OpenSCManager) 실패 Windows). 빈 배열 []="열거했고 0개"와 의미가 다르다 |
| listen_ports[] | 리슨 소켓. proto/addr/port/uid/pid/comm (uid는 Windows에서 null) |
| interfaces[] | 인터페이스. name/address/prefix/family/kind/gateway (IPv4+IPv6) |
| mac_addresses[] | MAC 목록(감사용) |
| ip_external[] | 외부(공인) IP. IMDS 전용 best-effort — 클라우드 메타데이터(AWS/Azure/GCP)나 AGENT_EXTERNAL_IP env 로만 채운다. VM 게스트는 NAT된 공인 IP를 자기 인터페이스에서 볼 수 없어, IMDS 없는 환경(OpenStack floating IP 등)에서는 null. floating IP 매핑이 필요하면 엔진이 클라우드 API로 채운다(에이전트 범위 밖) |

mounts[].fstype=null도 같은 "측정 불가"의 정공이다 — Windows GetVolumeInformation 실패(RAW/미포맷/BitLocker-locked 볼륨)로 파일시스템을 못 읽은 경우다. services=null과 함께, 빈 배열/""로 위장하지 않고 null로 발행한다.

services.pid와 listen_ports.pid로 unit-포트 조인이 가능하다. Linux services는 systemd unit(systemctl) 기반이고, systemd가 없는 SysV 호스트(CentOS/RHEL 6)는 /var/lock/subsys의 실행 중 서비스를 열거해 /var/run/<name>.pid로 pid/exe를 채운다. 표준 pid 파일이 있는 데몬만 pid/exe가 차고, 부팅 1회성 스크립트(iptables/network 등), pid 파일이 표준 위치에 없는 데몬, systemd의 socket, MainPID 없는 unit은 pid/exe=null이다(추측 pid를 넣지 않는다). Windows는 SCM(EnumServicesStatusEx)에서 pid/exe를 채운다.
interfaces[].gateway는 해당 인터페이스의 default route 게이트웨이 IP(없으면 null). Linux는 IPv4 default route(/proc/net/route), Windows는 실행 시 OS 세대를 감지해 NT6+에서는 FirstGatewayAddress, NT5.2(2003/XP)에서는 GetIpForwardTable(IPv4 default route)로 얻는다. NT5.2의 IPv6 gateway 항목은 null.

## metrics 필드

공통 메타데이터 + 아래.

모든 신규 신호는 raw 카운터로만 발행한다 — 엔진이 델타/비율/임계를 계산하니 에이전트는
단위 변환·해석을 하지 않는다. 커널/구성이 없어 못 읽는 신호는 0 위조 없이 null(엔진이 신뢰도로 흡수).

| 필드 | 내용 |
|---|---|
| collection_interval_sec | 설정된 수집 주기(초). 엔진 표본 충분성 계산 기준 |
| load_1m / load_5m / load_15m | 1/5/15분 로드애버리지. Linux 실측, Windows는 미지원 null |
| cpu_stat | user/nice/system/idle/iowait/irq/softirq/steal |
| procs_running / procs_blocked | 실행 큐 길이 / D-state IO 블록 프로세스 수(/proc/stat). procs_running=CPU 포화 주신호, procs_blocked=IO발 CPU 로드 분리(근본원인 판정 핵심). Windows null |
| cpu_per_core[] | 코어별 user..steal raw 카운터 배열(/proc/stat cpu0..N). 단일스레드 병목 감지. Windows는 별도 수집(NtQuerySystemInformation) — 미구현시 빈 배열/null |
| schedstat_run_wait_ns | runqueue 대기시간 누적(ns, /proc/schedstat). 실행 대기 적분값이라 procs_running 스냅샷보다 우선. CONFIG_SCHEDSTATS 없으면 null |
| pswpin / pswpout | 스왑 in/out 페이지 누적(/proc/vmstat). 메모리 포화 주신호 + 메모리발 디스크I/O 판별(근본원인). Windows는 Pages Input/sec 등 별도 |
| oom_kill | OOM 킬 누적(/proc/vmstat, 4.13+). 없으면 null. pgmajfault는 파일 mmap fault 혼입으로 발행 안 함 |
| psi_cpu_some_total / psi_mem_some_total / psi_io_some_total | PSI some total(us 누적, /proc/pressure/*, 4.20+). 관측·검증용(분류 미사용). 없으면 null |
| tcp_retrans_segs / tcp_tw | TCP 재전송 누적(/proc/net/snmp RetransSegs) / TIME_WAIT 소켓 수(/proc/net/sockstat). 네트워크 품질 신호 |
| conntrack_count / conntrack_max | nf_conntrack 현재/상한(연결 고갈 신호). conntrack 모듈 미로드면 파일 부재 -> null |
| disk_io[] | device/kind + reads_completed/writes_completed/sectors_read/sectors_written + await 원자료 time_reading_ms/time_writing_ms(diskstats 7·11) + io_ticks_ms/weighted_io_ms(13·14, %util·avgqu 참고값). virtio라 %util 대신 await가 포화 주신호 |
| net_io[] | interface/kind + rx/tx bytes/packets/errors/drops. drops는 포화 신호 |
| mounts[] | 마운트 사용량. mount/kind/fstype/total_bytes/free_bytes/avail_bytes + inodes_total/inodes_free(작은 파일 폭증 시 바이트 남아도 inode 먼저 소진돼 ENOSPC 조기감지). 시계열(free/avail/inode)은 여기만 싣는다 — inventory 는 구조만, metrics 는 사용량만이라 중복이 없다. total_bytes 는 % 계산용으로 양쪽에 둔다(mem_total_kb 와 동일 이유) |

## task.result 필드

task_id, status, exit_code, signal_no, failure_reason, duration_ms, completed_at,
stdout_tail, stderr_tail + os_family/os_id/os_version/os_codename + 공통 메타데이터.
단 task.result는 공통 메타데이터 중 composite_id를 싣지 않고(boot_time, agent_started_at은 항상 null),
os 식별은 os_family/os_id/os_version/os_codename으로 붙인다(inventory와 동일 4필드, Windows
os_codename은 codename 개념이 없어 null). task_id로 매칭하므로 composite_id 같은 식별키 없이도
조인되며, 식별 큐와 무관하다.

exit_code와 signal_no는 POSIX wait status를 그대로 반영해 상호배타다:

| 종료 형태 | exit_code | signal_no |
|---|---|---|
| 정상 종료(WIFEXITED) | 값 | null |
| 시그널 종료(crash/timeout kill, WIFSIGNALED) | null | 값(예: 9=SIGKILL, 11=SIGSEGV, 15=SIGTERM, 6=SIGABRT) |
| 상태 미포착(internal) | null | null |

엔진은 이 세 조합으로 "코드 N 종료"/"시그널 N 사망"/"상태 미포착"을 구분한다. signal_no는 자식(install.sh)을
종료시킨 시그널 번호로, timeout 케이스(worker의 SIGTERM vs 강제 SIGKILL)도 이 값으로 갈린다. Linux 전용
개념이라 Windows task.result는 항상 null(POSIX 시그널 없음).

task.result 필드 셋도 스키마(`task_result`)가 강제해, 트리 간 드리프트(예: 한쪽만 os_codename)를 릴리즈 전에 막는다.

## error 필드

공통 메타데이터 전체(composite_id 포함, task.result와 달리 add_common_metadata 경로를 그대로 탄다) + 아래.

| 필드 | 내용 |
|---|---|
| error_code | 실패 분류 코드(예: COLLECT_INVENTORY_FAILED, PUBLISH_RECOVERED). 자유형 문자열 |
| error_message | 사람이 읽는 실패 설명 |
| failed_component | 실패 단계(collect/publish 등). 미지정 fallback은 두 트리 모두 "agent" |
| retry_count | 재시도 횟수. 재시도 맥락에서만 실리는 옵셔널 |
| first_failed_at | 최초 실패 시각(iso8601). 옵셔널 |
| recovered_at | 복구 시각(iso8601). 복구 이벤트에서만 실리는 옵셔널 |

collected_at은 공통 메타데이터와 동일한 초 정밀도 iso8601이다(두 트리 통일). 이 필드 셋도 스키마(`error`)가 강제한다.

## device kind taxonomy

에이전트 공용 분류기 하나가 정한다. 엔진은 정규식/major 추론 없이 kind로만 필터한다.

| 대상 | kind 값 |
|---|---|
| disks / disk_io | physical, partition, lvm, raid, virtual |
| interfaces / net_io | physical, loopback, bridge, veth, bond_master, bond_member, vlan, tunnel, virtual |
| mounts | data, virtual_fs, boot, image (+ fstype) |

한 device를 여러 메시지가 다르게 태그하지 않는다.
- disks/interfaces: lo/loop/ram/sr 등만 pre-drop. interfaces 의 가상 종류는 sysfs(bridge/bonding/tun_flags dir)와 uevent DEVTYPE(vlan/bridge/bond/veth, vxlan/gre/sit 등 터널)로 판별한다 — 이름 규칙이 아니라 커널 신호 기준.
- bond(본딩): bond_master(bond0 등)가 본딩 네트워크의 집계 단위다 — /proc/net/dev 의 bond0 엔트리가 슬레이브 합산 트래픽 카운터를 그대로 나른다. bond_member(물리 leg eth0/eth1)는 그 합산분이 이미 bond_master 에 실리므로 net_io 집계에서 제외한다. 슬레이브를 physical 로 태그하지 않는다(그러면 bond_master + physical 합산 시 이중집계). 엔진은 net_io 를 kind in {physical, bond_master} 로 집계해야 본딩 호스트 트래픽이 누락(0)되지 않는다.
- mounts: 실제 스토리지만 싣는다. pseudo/virtual filesystem(proc/sysfs/cgroup/selinuxfs/usbfs 등)은 /proc/filesystems 의 nodev 플래그(커널이 "블록 디바이스 없음"으로 표시)로 배포판 무관하게 pre-drop 하고, 네트워크(nfs/cifs), FUSE 실데이터는 남긴다. 그 결과 Linux mounts.kind 는 data/boot/image 이고 virtual_fs 는 taxonomy 예약값이다.
- mounts dedup: 같은 (major,minor) block 스토리지가 여러 경로(bind 마운트, 동일 LV 이중 마운트)로 올라와도 (major,minor) 기준으로 dedup 해 한 번만 발행한다. inventory, metrics 가 같은 수집 경로(collect_mounts)를 타 양쪽 모두 적용되므로, 엔진이 시계열 용량/사용량을 합산할 때 이중집계되지 않는다 — 시계열 payload 엔 major/minor 가 없어 엔진 단독 device dedup 이 불가하므로 에이전트가 보장한다.

## 값 의미론 (0 / null)

모든 수치 필드는 아래 규칙으로 표현한다. 에이전트는 정석대로 표기하고, 엔진은 이 의미론으로 해석한다.

- 값(0 포함) = 측정에 성공했고 그 값이다. 실측 0은 유효한 0이다(예: idle이라 cpu_run_queue=0, 스왑 미사용이라 swap_total=0).
- null = 측정할 수 없다. 그 OS에 개념 자체가 없거나(미지원), 측정 인프라/권한이 없어 값을 모른다.
- 추측/대체값은 넣지 않는다. 확실히 모르면 null (틀린 값이 조인, 판정을 오염시키는 것보다 낫다).
- 배열 지표는 측정된 축만 싣는다. 못 잰 축은 제외한다(빈 배열 = 미측정).

즉 "0인데 왜 0인지"를 엔진이 되묻지 않아도 되게, 실측 0과 측정 불가를 표기 단계에서 가른다. 미지원 지표를 0으로 채우면 실측 0과 구분되지 않으므로 금지한다.

적용:
- Linux는 표준 지표를 전부 /proc, /sys로 실측하므로 값이고, 측정 불가한 것(os_codename, ip_external, pid 파일 없는 SysV 서비스의 pid/exe)만 null이다.
- Windows는 개념이 없거나 카운터를 못 읽는 지표(cpu_stat 일부, mem_free/buffers/cached, load, saturation 등)를 null로 둔다. 필드별 세부는 아래 "OS별 차이".

## OS별 차이

- Windows kind는 coarse(IfType/드라이브 종류 기반). Linux는 세분.
- Windows os_version은 DisplayVersion(없으면 ReleaseId, 예: Server 2016 -> "1607"). 둘 다 없는 구버전은 RtlGetVersion의 NT major.minor로 채운다(2012R2 -> "6.3", 2003 -> "5.2").
- Windows disk_io는 NtQuerySystemInformation(SystemPerformanceInformation)의 시스템 전역 누적 I/O로
  단일 엔트리(device=PhysicalDrive0)를 싣는다. I/O 매니저 카운터라 단조증가, provider 독립(NT5.2~NT10).
  perflib PhysicalDisk 디스크 카운터는 diskperf 성능 통계에 의존해 환경별로 죽거나(예: 2012R2+virtio raw=0)
  부팅 후 리셋(비단조)이라 1차 소스로 부적합 -> NtQuery 불가 시에만 perflib per-disk(PhysicalDriveN)로 폴백.
  전역 집계 특성상 파일 I/O(네트워크 리다이렉터 등)를 포함하며 물리 디스크별 분해는 하지 않는다.
- saturation은 Windows metrics 전용이다. Linux는 이 객체를 싣지 않고 load_1m/5m/15m, cpu_stat.iowait로
  포화를 표현하며, 엔진이 os_family로 정규화한다(load<->cpu_run_queue, iowait<->disk_queue). 스키마는
  os_family=windows일 때만 saturation을 요구하고 linux일 때는 금지한다.
- Windows metrics는 saturation 객체를 싣는다: `{disk_queue, cpu_run_queue, mem_paging_rate}`.
  disk_queue는 물리 디스크별 배열 `[{device, queue, read_time, write_time, idle_time, read_count,
  write_count, query_time}]`(device=PhysicalDriveN)로 IOCTL_DISK_PERFORMANCE를 실측한다. queue=QueueDepth,
  나머지는 await·%util 산출용 시간/카운트 원자료(100ns 누적 raw, 엔진이 델타·단위 해석). diskperf
  미부착(OpenStack virtio 구세대 등)이면 IOCTL이 ERROR_INVALID_FUNCTION이라 측정 불가 -> 해당 디스크를
  배열에서 제외한다(빈 배열=미측정, 엔진이 포화 미관측 처리). perflib "Current Disk Queue Length"는 diskperf
  미부착 시에도 raw 0을 내 측정 불가와 실측 0을 구분하지 못해 쓰지 않는다.
  cpu_run_queue(System\Processor Queue Length), mem_paging_rate(Memory\Pages/sec 누적)는 perflib raw이며
  카운터를 못 읽으면 null이다.
- Windows 신규 신호(right-sizing): net_io[]에 rx_drops/tx_drops(MIB_IF_ROW2 In/OutDiscards, NT5.2는
  MIB_IFROW dw* 폴백)를 발행한다. metrics 최상위에 mem_pages_input(Memory\Pages Input/sec — 총 Pages/sec와
  달리 파일 mmap I/O 미혼입, 하드 read 폴트만)과 tcp_retrans_segs(GetTcpStatistics.dwRetransSegs)를 발행한다.
  기존 mem_paging_rate(총 Pages/sec)는 계약 유지를 위해 그대로 둔다. per-core 이용률은 GetSystemTimes가
  전체만 줘 NtQuerySystemInformation(SystemProcessorPerformanceInformation) 신규 수집이 필요 -> 우선순위 낮아 미구현(후속).
- Windows cpu_stat은 user/system/idle만 실측하고 nice/iowait/irq/softirq/steal은 미지원이라 null이다.
  mem_free_kb는 perflib로 진짜 free를 실측(available과 별개, NT5.2는 null), mem_buffers/cached, load_1m/5m/15m은 미지원 null이다.
- listen_ports.uid는 Windows에서 null(POSIX uid 없음).
- disks/disk_io/mounts 의 major/minor 는 Windows 에서 null 이다. major:minor(dev_t)는 리눅스 커널 장치번호 개념이라 Windows 에 대응이 없다 — 0 으로 위조하지 않는다(디스크 정체성은 name=PhysicalDriveN).
- interfaces.prefix 는 NT6+ 에서 OnLinkPrefixLength 로 실측한다. NT5.2(2003/XP)는 구형 GAA 구조체에 이 필드가 없어, IPv4 는 GetIpAddrTable(dwMask popcount)로 실측하고 IPv6 는 이 테이블에 없어 측정 불가라 null 이다(0 은 유효한 /0 실측과 구분되지 않으므로 미측정에 쓰지 않는다). 즉 prefix 는 측정 가능한 전 경로에서 값이고, 오직 NT5.2 IPv6 에서만 null 이다.
- mounts 는 리눅스와 동일하게 inventory=구조(mount/kind/fstype/total_bytes/major/minor) / metrics=사용량(mount/kind/fstype/total_bytes/free_bytes/avail_bytes)으로 역할 분리한다 — 동적 free/avail 은 metrics 에만 싣는다.
- os_version/kernel_version/cpu_model 은 측정 불가 시 null 이다("" 나 "Unknown" 같은 대체값을 넣지 않는다). 실무상 RtlGetVersion, cpuid 폴백으로 거의 항상 채워진다.

## 빌드/릴리즈

소스 빌드(2종), CI 태그 릴리즈, 저장소 트리는 BUILD.md 참고.
