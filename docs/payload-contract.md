# payload 계약 (에이전트 -> 엔진)

에이전트가 RabbitMQ 브로커로 발행하는 wire 계약. 5종 바이너리 모두 동일 스키마다.
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
- composite_id, machine_id, mac_addresses: 감사·표시용. 식별·라우팅에는 쓰지 않는다.
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
| mounts[] | 마운트. mount/kind/fstype/total_bytes 등 |
| services[] | 서비스. unit/sub/pid/exe |
| listen_ports[] | 리슨 소켓. proto/addr/port/pid/comm |
| interfaces[] | 인터페이스. name/address/prefix/family/kind (IPv4+IPv6) |
| mac_addresses[] | MAC 목록(감사용) |
| ip_external[] | 외부 IP |

services.pid와 listen_ports.pid로 unit-포트 조인이 가능하다.

## metrics 필드

공통 메타데이터 + 아래.

| 필드 | 내용 |
|---|---|
| loadavg | 1/5/15분 (Linux) |
| cpu_stat | user/nice/system/idle/iowait/irq/softirq/steal |
| mem_* / swap_* | mem_total_kb, mem_free_kb, mem_available_kb, mem_buffers_kb, mem_cached_kb, swap_total_kb, swap_free_kb. 불변식 mem_available<=mem_total, swap_free<=swap_total 보장 |
| disk_io[] | device/kind + reads/writes 카운터 |
| net_io[] | interface/kind + rx/tx 카운터 |
| mounts[] | mount/kind/fstype + 사용량 |

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

한 device를 여러 메시지가 다르게 태그하지 않는다. lo/loop/ram/sr 등만 pre-drop한다.

## OS별 차이

- Windows kind는 coarse(IfType/드라이브 종류 기반). Linux는 세분.
- Windows os_version은 DisplayVersion을 쓴다.
- Windows metrics는 saturation 객체를 싣는다: `{disk_queue, cpu_run_queue, mem_paging_rate}`.
  현재 disk_queue만 값을 발행(IOCTL_DISK_PERFORMANCE.QueueDepth 합)하고,
  cpu_run_queue/mem_paging_rate는 null(미측정). perflib(HKEY_PERFORMANCE_DATA) 실기 검증 후 raw 값으로 채운다.
- listen_ports.uid는 Windows에서 null(POSIX uid 없음).

## 빌드/릴리즈

소스 빌드(5종), CI 태그 릴리즈, 저장소 트리는 BUILD.md 참고.
