# 통합 자원 데이터 모델 (에이전트 발행 정본, canonical 재정리)

> 성격: 협의용 설계 정본(에이전트 -> 엔진). self-contained. 삭제 자유.
> storage-layering / resource-signals 두 요청과 diskperf 실기 검증을 하나로 합치고,
> 호스트 메트릭의 업계 정본인 OpenTelemetry system semantic conventions 에 맞춰 재구성한다.
> OTLP 전송을 쓰자는 게 아니라, OTel 의 네이밍/속성/단위/카운터 타이핑 규약을 현행 JSON wire 에 적용한다.

## 0. 핵심 구조: 2레이어 분리

- Layer 1 (wire): 자원 네임스페이스별 raw 카운터/게이지 사실. USE 를 wire 에 넣지 않는다.
- Layer 2 (engine): USE Method(Utilization/Saturation/Errors) 매핑. Layer 1 raw 를 엔진이 해석(이미 recommendation.py).
근거: USE 는 평가 프레임이지 스키마가 아니다. wire 를 USE 로 짜면 한 축이 여러 필드에 파편화된다.
raw 사실만 싣고 파생(rate/util/await/ratio)은 소비자가 계산 — Prometheus/OTel 공통 컨벤션.

## 1. 설계 원칙

- 2트리: Linux(musl static, kernel 2.6.32+) / Windows(single i686, 2003 SP1+). 스키마 동일.
- null 의미론: 값=실측(0 포함), null=측정불가/미지원. 가짜값 금지.
- agent=raw counter / engine=derive: 에이전트는 emit 시점 누적 raw 스냅샷만. rate/delta 는 엔진.
- 카운터 타이핑 명시: 각 지표는 counter(monotonic 누적) 또는 gauge(순간). counter 는 엔진이 델타, gauge 는 직접.
- base 단위(OTel/Prometheus 정본): 시간=seconds(float), 크기=bytes(By), 이용률=ratio(0..1, % 아님),
  링크속도=bit/s, 카운트=정수. Windows-ism(100ns)·sectors·jiffies·% 는 에이전트에서 base 로 정규화.
- attribute 표준화: device, direction(read/write | receive/transmit | in/out), state(used/free/cached/available),
  source(신호원), scope(some/full), window(10/60/300). 이름에 방향 박지 않고 attribute 로.
- 신호원 명시 라벨: OS별 대리 신호 비대칭을 source attribute 로 노출(presence 추론 금지).
- 하한 명시 = null: 신호별 커널/드라이버 하한 미달이면 계약상 null(PSI 4.20+, MemAvailable 3.14+, virtio-net speed 등).

## 2. Layer 1 — wire raw 스키마 (OTel system.* aligned)

인코딩: 자원 네임스페이스 객체 안에 데이터포인트 배열. 각 포인트 = {attributes..., value}.
`t`=counter, `g`=gauge. 단위는 OTel 표기(s, By, {operations}, 1=ratio, bit/s).

### 2.1 system.cpu

| metric | 타입 | 단위 | attr | Linux 소스 | Windows 소스 | null |
|---|---|---|---|---|---|---|
| cpu.time | t | s | state(user/system/nice/idle/iowait/irq/softirq/steal) | /proc/stat (jiffies/CLK_TCK) | GetSystemTimes(100ns->s; user/system/idle만) | 미지원 state |
| cpu.run_queue | g | {tasks} | source(procs_running\|processor_queue) | procs_running(/proc/stat) | System\Processor Queue Length(perflib) | - |
| cpu.logical.count | g | {cpu} | - | nproc/sysconf | GetSystemInfo | - |

### 2.2 system.memory / system.paging

| metric | 타입 | 단위 | attr | Linux | Windows | null |
|---|---|---|---|---|---|---|
| memory.usage | g | By | state(used/free/cached/buffered/available) | /proc/meminfo (available: 3.14+ 실측, 미만 계산 or null) | GlobalMemoryStatusEx + perflib Available MBytes | 미지원 state |
| memory.limit | g | By | - | MemTotal | ullTotalPhys | - |
| memory.commit.usage | g | By | - | Committed_AS(/proc/meminfo) | Committed Bytes(perflib) | - |
| memory.commit.limit | g | By | - | CommitLimit | Commit Limit(perflib) | - |
| paging.operations | t | {operations} | direction(in/out), type(major/minor) | /proc/vmstat pswpin/pswpout, pgmajfault | Pages Input/Output/sec raw(perflib) | swapless: out 상시 0(실측) |

### 2.3 system.disk (per device; Windows 소스 IOCTL -> perflib 전환)

| metric | 타입 | 단위 | attr | Linux 소스 | Windows 소스 | null |
|---|---|---|---|---|---|---|
| disk.io | t | By | device, direction(read/write) | diskstats sectors*512 | perflib Disk Read/Write Bytes/sec raw | - |
| disk.operations | t | {operations} | device, direction | diskstats reads/writes | perflib Disk Reads/Writes/sec raw | - |
| disk.io_time | t | s | device | diskstats io_ticks(ms->s) | perflib (query_time - % Idle Time raw)->s | perflib 인스턴스 부재 |
| disk.operation_time | t | s | device, direction | diskstats time_reading/writing(ms->s) | perflib Avg. Disk sec/Read,Write raw(ticks->s) | 인스턴스 부재 |
| disk.pending_operations | g | {operations} | device | diskstats in_flight | perflib Current Disk Queue Length | 인스턴스 부재 |
| disk.errors | t | {errors} | device, direction | /sys/block/*/device/ioerr_cnt, mdraid | 이벤트로그/storport(best-effort) | 소스 부재(대개 Windows null) |

검증: dp-win2012(Server2012, IOCTL_DISK_PERFORMANCE err=1)에서 perflib 로 await 1.5ms / 큐 0.78 / busy 80% 실측.
IOCTL 은 구세대 viostor 에서 죽어 폐기. 레이아웃 IOCTL(DRIVE_LAYOUT_EX/VOLUME_DISK_EXTENTS)은 성능 IOCTL 과 별개라 유지.

### 2.4 system.filesystem (per mount)

| metric | 타입 | 단위 | attr | Linux | Windows | null |
|---|---|---|---|---|---|---|
| filesystem.usage | g | By | device, mountpoint, type(fstype), state(used/free) | statvfs + /proc/mounts | GetDiskFreeSpaceEx | RAW/BitLocker: 용량 null |

### 2.5 system.network (per interface)

| metric | 타입 | 단위 | attr | Linux | Windows | null |
|---|---|---|---|---|---|---|
| network.io | t | By | device, direction(receive/transmit) | /proc/net/dev | MIB_IF_ROW2(GetIfEntry2, NT6 dispatch) | - |
| network.packets | t | {packets} | device, direction | /proc/net/dev | MIB_IF_ROW2 | - |
| network.errors | t | {errors} | device, direction | /proc/net/dev errs | MIB_IF_ROW2 In/OutErrors | - |
| network.dropped | t | {packets} | device, direction | /proc/net/dev drop | MIB_IF_ROW2 In/OutDiscards | - |
| network.link.speed | g | bit/s | device | /sys/class/net/*/speed(Mbps->bit/s) | MIB_IF_ROW2 TransmitLinkSpeed | virtio-net/가상 NIC 부재 -> null |

### 2.5e system.errors (USE E축 — 최대 canonical 파싱, testbed 검증)

방침: 문서보다 정석 컨벤션 우선. node_exporter/PerfMon 관례의 canonical 에러 소스를 최대한 파싱한다.
"완전 미관측"은 없다 — 전부 소스가 붙고, fault 시 실값·정상 시 실측 0·소스 부재 시 null. E축은 사이징 숫자가
아니라 엔진의 confidence(오염 게이트, steal 패턴) + monitoring/attention(net_retrans_pct 계층3)으로 소비된다.

| metric | 타입 | 단위 | Linux 소스 | Windows 소스 | virtio 기대 |
|---|---|---|---|---|---|
| disk.md.degraded / .mismatch / member.state·errors | g/t | {devices}/{errors} | /sys/block/md*/md/(+/proc/mdstat) | (동적디스크 상태 best-effort) | 소프트RAID면 실신호, 없으면 null |
| disk.btrfs.errors(write/read/flush/corruption/generation) | t | {errors} | /sys/fs/btrfs/<fsid>/devinfo/*/error_stats | (N/A) | btrfs면 실신호, 없으면 null |
| disk.ext4.errors(errors_count/warning_count/first·last_error_time) | t/g | {errors} | /sys/fs/ext4/<dev>/ | (N/A) | ext4면 실신호 |
| disk.mpath.path_state | g | {paths} | dm/uuid=mpath-* + dmsetup status(multipathd) | (MPIO best-effort) | mpath면 검출, path 상태 best-effort |
| disk.ioerr | t | {errors} | /sys/block/*/device/ioerr_cnt(scsi/vioscsi) | 이벤트로그 disk/Ntfs count | virtio-blk null / vioscsi 실측 |
| network.errors / .dropped | t | {packets} | /proc/net/dev errs·drop·fifo·frame·colls | Win32_PerfRawData_Tcpip In/OutErrors·Discards | 물리err ~0, discards 실값(예 rxDisc=16) |
| network.tcp.retransmits | t | {segments} | /proc/net/snmp RetransSegs + netstat TcpExt | MIB_TCPSTATS dwRetransSegs | 실신호(경로손실, 예 =2) |
| network.conntrack usage/limit | g | {connections} | /proc/sys/net/netfilter/nf_conntrack_{count,max} | (N/A) | 모듈 미로드 null |
| memory.hardware_corrupted | g | By | /proc/meminfo HardwareCorrupted(poisoned) | (WHEA 이벤트로그) | VM에도 존재, 정상 0 |
| memory.edac ce/ue | t | {errors} | /sys/devices/system/edac/mc*/ | (WHEA) | VM mc 미등록 -> null |
| cpu.mce | t | {events} | /sys/devices/system/machinecheck + /dev/mcelog | WHEA-Logger 이벤트 | 소스 존재, VM 주입 시만 nonzero |

검증(st-raid/st-lvm/st-btrfs/st-mpath/dp-win2016): mdraid 멤버 state(in_sync/faulty)·errors·degraded·mismatch,
btrfs devinfo error_stats(write/read/flush/corruption/generation), ext4 errors_count/warning_count,
dm-multipath 검출(dm/uuid=mpath-*), tcp RetransSegs+TcpExt, net discards 실값(win rxDisc=16),
HardwareCorrupted(VM 존재), MCE 소스 존재 — 전부 파싱 확인. 완전체(블록/RAID/fs/mpath/device/net/mem/cpu 계층 커버).
유일한 완전 null 은 EDAC ce/ue(VM mc 미등록) — 그 자리는 HardwareCorrupted 가 메모리 에러를 대체 커버.

### 2.6 system.pressure (PSI; Linux kernel 4.20+)

| metric | 타입 | 단위 | attr | 소스 | null |
|---|---|---|---|---|---|
| pressure.{cpu,memory,io} | g | 1(ratio) | scope(some/full), window(10/60/300) | /proc/pressure/* avg | 커널<4.20; Windows |
| pressure.{cpu,memory,io}.total | t | s | scope(some/full) | /proc/pressure/* total(us->s) | 상동 |

구 커널(EL6/EL7/Debian10/SLES11-12)·Windows: pressure null. swapless 사각지대가 이 군에선 잔존 -> 엔진은 PSI 를 폴백 대체가 아니라 보강으로.

### 2.7 system.cgroup (컨테이너 배포 시만; VM 이면 전체 null)

| metric | 타입 | 단위 | attr | 소스 |
|---|---|---|---|---|
| cgroup.cpu.limit | g | {cpu} | - | cpu.max quota/period |
| cgroup.cpu.throttled.time | t | s | - | cpu.stat throttled_usec |
| cgroup.memory.limit | g | By | - | memory.max |
| cgroup.memory.events | t | {events} | type(oom/high) | memory.events |

### 2.8 스토리지 토폴로지 (inventory; 정규화 단일 그래프)

현행 disks[]/mounts[] 의 중복을 접어 단일 block 그래프로. disks/mounts 는 여기서 파생(뷰)하며 점진 폐기.

`block_devices[]` 평면 노드(부모-링크 DAG):
- name, type(disk/part/lvm/crypt/raid/mpath/dynamic/volume; unknown pass-through), size_bytes(By),
  fstype(null 가능), mountpoint(null 가능), parent(부모 id; root=null; 복수면 노드 반복),
  id + id_type(2.9 안정키), name(표시용).
- 수집: Linux lsblk 풀트리(util-linux 2.27+) + sysfs holders/slaves 폴백(EL6); fstype/uuid 갭은 슈퍼블록 매직.
  Windows layout IOCTL(DRIVE_LAYOUT_EX + VOLUME_DISK_EXTENTS; viostor 정상). 동적 디스크 물리 매핑 포함, LDM 논리 상세는 best-effort.

`lvm_vgs[]` (Linux 전용, os_family 조건부):
- {name, size_bytes, free_bytes} + 씬 {data_percent, metadata_percent}. 소스 vgs/lvs. lvm2 부재 null.
  Windows 확장 여력은 block_devices 의 디스크크기-파티션합(미할당)으로.

### 2.9 device 안정키 (계층 폴백 — testbed 검증 완료)

metric 의 device attribute 와 block_devices 의 id/parent 는 이름(perflib `0 C:`, Linux `dm-N`)이 재부팅/디스크추가에
바뀌므로, 이름이 아닌 안정 id 를 조인 키로 쓴다. 단일 필드로 부족(fs UUID 없는 stripe/thin LV, GUID 없는 RAW 디스크가
null) — 계층 폴백 + id_type 라벨로 100% 해소(st-lvm/dp-win2016 실측 확인).

Linux 폴백 순위:
1. dm 계열(lvm/crypt/mpath/raid): `/sys/block/<kname>/dm/uuid` (예 `LVM-...`) — 항상 존재·안정.
2. 파티션: PARTUUID.
3. 디스크: `/sys/block/<kname>/device/wwid` -> `serial` -> `/dev/disk/by-id`.
4. 리프 폴백: fs UUID.

Windows 폴백 순위:
1. 디스크: GPT DiskId -> MBR Signature -> SerialNumber/UniqueId(RAW 포함).
2. 파티션: GPT PartitionId GUID(MBR 파티션은 disk sig + 오프셋 조합).
3. 볼륨: volume GUID(`\\?\Volume{...}`).

- 각 노드/데이터포인트에 `id`(값) + `id_type`(dm|partuuid|wwid|serial|fsuuid|gptid|mbrsig|volguid) 동반.
- parent 는 부모의 name/kname(불안정)이 아니라 부모의 `id`로 링크(불안정 dm-N -> 부모 dm/uuid 로 정규화, 실측 확인).
- metric device attribute 도 같은 id 를 1차 키로(시계열 자연키 안정). name 은 표시용 부가.

## 3. Layer 2 — USE 매핑 (엔진 해석; wire 아님)

| 자원 | 축 | 산출(Layer1 raw 로부터) | Linux 신호(source) | Windows 신호(source) |
|---|---|---|---|---|
| CPU | U | 1 - rate(cpu.time[idle]) / Σrate(cpu.time) | cpu.time | cpu.time |
| CPU | S | pressure.cpu.some 우선, 없으면 cpu.run_queue/cores | psi \| procs_running | processor_queue |
| Memory | U | 1 - memory.usage[available]/memory.limit | meminfo | availphys |
| Memory | S | pressure.memory 우선, 없으면 paging.operations[out] rate / commit.usage/commit.limit | psi \| page_out \| commit | pages_input \| commit |
| Disk | U | rate(disk.io_time) (장치 busy 비율) | diskstats | perflib_idle |
| Disk | S | Δdisk.operation_time/Δdisk.operations (await) 우선, disk.pending_operations 보조 | diskstats | perflib_await |
| Disk | E | rate(disk.errors) | sysfs \| smart | eventlog |
| Network | U | rate(network.io)/network.link.speed | speed \| null | speed \| null |
| Network | E | rate(network.errors) | net_dev | mib_if |

source attribute 가 OS별 신호원 비대칭을 그대로 노출 -> 엔진은 os_family 분기 없이 source 로 판별/랭킹.

## 4. 현행 대비 변경 + 마이그레이션

이 모델은 wire v2 다. 현행 v1(jiffies/sectors/100ns/%, saturation.disk_queue 객체, rx_/tx_ 접두, disks/mounts 병행)과 단위·구조가 달라 additive 만으론 안 된다.

- 버전 cutover: 메시지에 schema_version 필드로 v1/v2 판별. 에이전트-엔진 동시 전환. 전환기엔 v2 발행 + 엔진 v2 소비.
- 무비용/즉시(구조): USE 를 wire 에서 분리(Layer 표만 이동), 신호원 source 라벨, 카운터/게이지 타이핑, 스토리지 단일 그래프.
- 유비용(cutover): base 단위 정규화(seconds/bytes/ratio), direction/state attribute 화. 엔진 counter_agg/mappers/recommendation 단위·필드 대응 필요.
- 엔진 영향 지점: consumer/mappers(_await_fields, _max_disk_queue), recommendation(disk_queue_p95/await/util 배선), db 컬럼 단위. disk_queue 폐기(ADR 0052 Phase E)와 정합.
- 에이전트 영향 지점: Windows saturation 소스 IOCTL -> perflib 전환. installer 의 EnableCounterForIoctl 및 관련 문서 revert — 무효(구세대 viostor 는 상위 partmgr 플래그로 못 켬)일 뿐 아니라, 관측 전용 에이전트가 시스템 레지스트리를 영구 변경하고 그 효과가 재부팅에 의존하는 것 자체가 부적절(그 세션 무효 + 재부팅은 에이전트가 유발 금지 + 침습적 사이드이펙트). 시스템 설정 변경은 재부팅 통제권 있는 프로비저닝/운영 시점의 일. Linux diskstats 는 이미 raw 라 재라벨 위주.

## 5. 계약 강제 / 우선순위

- schema/wire.schema.json 정본에 system.* 섹션 + os_family/커널 조건부 + 카운터-게이지-단위 명시.
- scripts/check-contract.sh: 두 바이너리 emit dry-run 이 v2 스키마로 검증(단위/타입/null/조건부).
- 우선순위:
  1. Windows 디스크 perflib 전환(util+sat 동시, IOCTL revert). 실기 검증 완료, 세 요청 교차점.
  2. base 단위 정규화 + Layer 분리 + source 라벨 + 카운터 타이핑(구조 정석화).
  3. PSI + network.link.speed + memory.commit/available(utilization 분모·saturation 통일).
  4. 스토리지 단일 그래프(block_devices/lvm_vgs, disks/mounts 파생).
  5. Errors(disk/net) + cgroup.
