# payload 계약 (에이전트 -> 엔진)

에이전트가 RabbitMQ 브로커로 발행하는 wire 계약. 2종 바이너리 모두 동일 스키마다.
Linux/Windows 차이는 아래 "OS별 차이"에만 있다.

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
- composite_id, machine_id, mac_addresses: 감사·표시용. 식별·라우팅에는 쓰지 않는다. machine_id는 안정적 소스(/etc/machine-id, dbus, 클라우드 IMDS, Windows MachineGuid)가 없으면 억지로 채우지 않고 null이다(예: non-systemd+비클라우드 리눅스). composite_id는 machine_id가 없어도 mac 기반으로 유니크가 유지된다.
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
| disks[] | 물리 디스크. device/kind/major/minor/size 등 |
| mounts[] | 마운트 구조. mount/major/minor/total_bytes/fstype/kind. 동적 사용량(free/avail)은 싣지 않는다(metrics 전담) |
| services[] | 서비스. unit/sub/pid/exe |
| listen_ports[] | 리슨 소켓. proto/addr/port/pid/comm |
| interfaces[] | 인터페이스. name/address/prefix/family/kind/gateway (IPv4+IPv6) |
| mac_addresses[] | MAC 목록(감사용) |
| ip_external[] | 외부(공인) IP. IMDS 전용 best-effort — 클라우드 메타데이터(AWS/Azure/GCP)나 AGENT_EXTERNAL_IP env 로만 채운다. VM 게스트는 NAT된 공인 IP를 자기 인터페이스에서 볼 수 없어, IMDS 없는 환경(OpenStack floating IP 등)에서는 null. floating IP 매핑이 필요하면 엔진이 클라우드 API로 채운다(에이전트 범위 밖) |

services.pid와 listen_ports.pid로 unit-포트 조인이 가능하다. Linux services는 systemd unit(systemctl) 기반이고, systemd가 없는 SysV 호스트(CentOS/RHEL 6)는 /var/lock/subsys의 실행 중 서비스를 열거해 /var/run/<name>.pid로 pid/exe를 채운다. 표준 pid 파일이 있는 데몬만 pid/exe가 차고, 부팅 1회성 스크립트(iptables/network 등)·pid 파일이 표준 위치에 없는 데몬·systemd의 socket·MainPID 없는 unit은 pid/exe=null이다(추측 pid를 넣지 않는다). Windows는 SCM(EnumServicesStatusEx)에서 pid/exe를 채운다.
interfaces[].gateway는 해당 인터페이스의 default route 게이트웨이 IP(없으면 null). Linux는 IPv4 default route(/proc/net/route), Windows는 실행 시 OS 세대를 감지해 NT6+에서는 FirstGatewayAddress, NT5.2(2003/XP)에서는 GetIpForwardTable(IPv4 default route)로 얻는다. NT5.2의 IPv6 gateway 항목은 null.

## metrics 필드

공통 메타데이터 + 아래.

| 필드 | 내용 |
|---|---|
| collection_interval_sec | 설정된 수집 주기(초). 엔진 표본 충분성 계산 기준 |
| loadavg | 1/5/15분 (Linux) |
| cpu_stat | user/nice/system/idle/iowait/irq/softirq/steal |
| mem_* / swap_* | mem_total_kb, mem_free_kb, mem_available_kb, mem_buffers_kb, mem_cached_kb, swap_total_kb, swap_free_kb. 불변식 mem_available<=mem_total, swap_free<=swap_total 보장 |
| disk_io[] | device/kind + reads/writes 카운터 |
| net_io[] | interface/kind + rx/tx 카운터 |
| mounts[] | 마운트 사용량. mount/kind/fstype/total_bytes/free_bytes/avail_bytes. 시계열(free/avail)은 여기만 싣는다 — inventory 는 구조만, metrics 는 사용량만이라 중복이 없다. total_bytes 는 % 계산용으로 양쪽에 둔다(mem_total_kb 와 동일 이유) |

## task.result 필드

task_id, status, exit_code, failure_reason, duration_ms, completed_at,
stdout_tail, stderr_tail + 공통 메타데이터 + os_family/os_id/os_version.
task_id로 매칭하므로 식별 큐와 무관하다.

## device kind taxonomy

에이전트 공용 분류기 하나가 정한다. 엔진은 정규식/major 추론 없이 kind로만 필터한다.

| 대상 | kind 값 |
|---|---|
| disks / disk_io | physical, partition, lvm, raid, virtual |
| interfaces / net_io | physical, loopback, bridge, veth, bond_master, bond_member, vlan, tunnel, virtual |
| mounts | data, virtual_fs, boot, image (+ fstype) |

한 device를 여러 메시지가 다르게 태그하지 않는다.
- disks/interfaces: lo/loop/ram/sr 등만 pre-drop. interfaces 의 가상 종류는 sysfs(bridge/bonding/tun_flags dir)와 uevent DEVTYPE(vlan/bridge/bond/veth, vxlan/gre/sit 등 터널)로 판별한다 — 이름 규칙이 아니라 커널 신호 기준.
- mounts: 실제 스토리지만 싣는다. pseudo/virtual filesystem(proc/sysfs/cgroup/selinuxfs/usbfs 등)은 /proc/filesystems 의 nodev 플래그(커널이 "블록 디바이스 없음"으로 표시)로 배포판 무관하게 pre-drop 하고, 네트워크(nfs/cifs)·FUSE 실데이터는 남긴다. 그 결과 Linux mounts.kind 는 data/boot/image 이고 virtual_fs 는 taxonomy 예약값이다.

## 값 의미론 (0 / null)

모든 수치 필드는 아래 규칙으로 표현한다. 에이전트는 정석대로 표기하고, 엔진은 이 의미론으로 해석한다.

- 값(0 포함) = 측정에 성공했고 그 값이다. 실측 0은 유효한 0이다(예: idle이라 cpu_run_queue=0, 스왑 미사용이라 swap_total=0).
- null = 측정할 수 없다. 그 OS에 개념 자체가 없거나(미지원), 측정 인프라/권한이 없어 값을 모른다.
- 추측/대체값은 넣지 않는다. 확실히 모르면 null (틀린 값이 조인·판정을 오염시키는 것보다 낫다).
- 배열 지표는 측정된 축만 싣는다. 못 잰 축은 제외한다(빈 배열 = 미측정).

즉 "0인데 왜 0인지"를 엔진이 되묻지 않아도 되게, 실측 0과 측정 불가를 표기 단계에서 가른다. 미지원 지표를 0으로 채우면 실측 0과 구분되지 않으므로 금지한다.

적용:
- Linux는 표준 지표를 전부 /proc·/sys로 실측하므로 값이고, 측정 불가한 것(os_codename, ip_external, pid 파일 없는 SysV 서비스의 pid/exe)만 null이다.
- Windows cpu_stat: user/system/idle는 실측, nice/iowait/irq/softirq/steal은 Windows에 개념이 없어 null(GetSystemTimes 미제공).
- Windows mem_free_kb: perflib "Free & Zero Page List Bytes"로 진짜 free를 실측(available과 별개). 이 카운터가 없는 NT5.2는 null.
- Windows mem_buffers_kb/mem_cached_kb, load_1m/5m/15m: Windows 미지원 -> null.
- saturation.disk_queue: IOCTL_DISK_PERFORMANCE로 실측하고, diskperf 미부착(측정 불가, OpenStack virtio 등)이면 빈 배열이다. cpu_run_queue/mem_paging_rate는 perflib 카운터를 못 읽으면 null이다.

## OS별 차이

- Windows kind는 coarse(IfType/드라이브 종류 기반). Linux는 세분.
- Windows os_version은 DisplayVersion(없으면 ReleaseId, 예: Server 2016 -> "1607"). 둘 다 없는 구버전은 RtlGetVersion의 NT major.minor로 채운다(2012R2 -> "6.3", 2003 -> "5.2").
- Windows disk_io는 NtQuerySystemInformation(SystemPerformanceInformation)의 시스템 전역 누적 I/O로
  단일 엔트리(device=PhysicalDrive0)를 싣는다. I/O 매니저 카운터라 단조증가·provider 독립(NT5.2~NT10).
  perflib PhysicalDisk 디스크 카운터는 diskperf 성능 통계에 의존해 환경별로 죽거나(예: 2012R2+virtio raw=0)
  부팅 후 리셋(비단조)이라 1차 소스로 부적합 -> NtQuery 불가 시에만 perflib per-disk(PhysicalDriveN)로 폴백.
  전역 집계 특성상 파일 I/O(네트워크 리다이렉터 등)를 포함하며 물리 디스크별 분해는 하지 않는다.
- Windows metrics는 saturation 객체를 싣는다: `{disk_queue, cpu_run_queue, mem_paging_rate}`.
  disk_queue는 물리 디스크별 배열 `[{device, queue}]`(device=PhysicalDriveN)로 IOCTL_DISK_PERFORMANCE의
  QueueDepth를 실측한다. diskperf 미부착(OpenStack virtio 등)이면 IOCTL이 ERROR_INVALID_FUNCTION이라 측정
  불가 -> 해당 디스크를 배열에서 제외한다(빈 배열=미측정). perflib "Current Disk Queue Length"는 diskperf
  미부착 시에도 raw 0을 내 측정 불가와 실측 0을 구분하지 못해 쓰지 않는다.
  cpu_run_queue(System\Processor Queue Length)·mem_paging_rate(Memory\Pages/sec 누적)는 perflib raw이며
  카운터를 못 읽으면 null이다.
- Windows cpu_stat은 user/system/idle만 실측하고 nice/iowait/irq/softirq/steal은 미지원이라 null이다.
  mem_free_kb는 perflib로 진짜 free를 실측(available과 별개, NT5.2는 null), mem_buffers/cached·load_1m/5m/15m은 미지원 null이다. (값 의미론 참고)
- listen_ports.uid는 Windows에서 null(POSIX uid 없음).
- disks/disk_io/mounts 의 major/minor 는 Windows 에서 null 이다. major:minor(dev_t)는 리눅스 커널 장치번호 개념이라 Windows 에 대응이 없다 — 0 으로 위조하지 않는다(디스크 정체성은 name=PhysicalDriveN).
- interfaces.prefix 는 NT6+ 에서 OnLinkPrefixLength 로 실측한다. NT5.2(2003/XP)는 구형 GAA 구조체에 이 필드가 없어, IPv4 는 GetIpAddrTable(dwMask popcount)로 실측하고 IPv6 는 이 테이블에 없어 측정 불가라 null 이다(0 은 유효한 /0 실측과 구분되지 않으므로 미측정에 쓰지 않는다). 즉 prefix 는 측정 가능한 전 경로에서 값이고, 오직 NT5.2 IPv6 에서만 null 이다.
- mounts 는 리눅스와 동일하게 inventory=구조(mount/kind/fstype/total_bytes/major/minor) / metrics=사용량(mount/kind/fstype/total_bytes/free_bytes/avail_bytes)으로 역할 분리한다 — 동적 free/avail 은 metrics 에만 싣는다.
- os_version/kernel_version/cpu_model 은 측정 불가 시 null 이다("" 나 "Unknown" 같은 대체값을 넣지 않는다). 실무상 RtlGetVersion·cpuid 폴백으로 거의 항상 채워진다.

## 빌드/릴리즈

소스 빌드(2종), CI 태그 릴리즈, 저장소 트리는 BUILD.md 참고.
