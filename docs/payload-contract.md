# payload 계약 (에이전트 -> 엔진)

에이전트가 RabbitMQ 브로커로 발행하는 wire 계약. 이 문서는 산문 설명이고,
기계검증 가능한 정본(source of truth)은 `schema/wire.schema.json`(JSON Schema, `schema_version:"1.0"`)이다.
CI가 두 바이너리의 `emit` dry-run 출력(inventory / metrics / task.result / error 4종 전부)을 이
스키마로 강제해(`scripts/check-contract.sh`), 리눅스-윈도우 트리 간 드리프트와 자기계약 위반을
릴리즈 전에 잡는다. 스키마는 구조(네임스페이스/타입/단위/attr맵/null/os_family)를 강제하지만 metric
이름과 attr 키 어휘는 개방형이라, metric/attr 이름이 소비자 키와 어긋나는 producer 드리프트(값은
맞는데 키 오타로 조용히 드롭)는 못 잡는다. 그래서 어휘 정본 `schema/metric-vocab.json` + 검사
`scripts/check-vocab.py`를 같은 게이트에 얹어, emit 의 system.* metric 이름과 attr 키가 어휘의
부분집합인지 강제한다(OS/커널 조건부 미발행은 허용). 로컬 검증:
`scripts/check-contract.sh dist/assessment-agent-linux-x86_64` /
`... dist/assessment-agent-windows-x86.exe wine`.

계약의 뼈대는 2레이어 분리다. wire(Layer 1)는 자원 네임스페이스별 raw 카운터/게이지 사실만 싣고,
USE Method(Utilization/Saturation/Errors) 해석(Layer 2)은 엔진이 한다. USE 는 평가 프레임이지
스키마가 아니므로 wire 에 넣지 않는다 — raw 사실만 싣고 파생(rate/util/await/ratio)은 소비자가
계산하는 Prometheus/OpenTelemetry 공통 컨벤션을 따른다. metrics/inventory 는 OTel system.* semantic
conventions 의 네이밍/속성/단위/카운터 타이핑에 맞춘다. 대표 예시는 `docs/wire-examples.json`,
사이징 해석 의도는 `docs/classification-rationale.md`.

## 인코딩 규약

- 자원은 `system.<resource>` 네임스페이스 객체. 그 안에 `metric` 객체들이 들어가고, 각 metric 은
  `{type, unit, points}`. `points` 는 데이터포인트 배열이고 각 포인트는 `{attr:{...}, value}`.
- `type`: `counter`(monotonic 누적, 엔진이 델타) 또는 `gauge`(순간값, 직접 사용).
- `unit`: OTel base 단위. 시간=`s`, 크기=`By`, 이용률=`1`(ratio 0..1, % 아님), 링크속도=`bit/s`,
  카운트=`operations`/`packets`/`errors`/`segments` 등. Windows-ism(100ns)·sectors·jiffies·% 는
  에이전트가 base 로 정규화한다.
- `attr`: 표준 키. `device`(안정 id), `direction`(read/write | receive/transmit | in/out),
  `state`(used/free/cached/available/reserved), `source`(신호원), `scope`(some/full),
  `window`(avg10/60/300), `resource`(cpu/memory/io), `type`(세부 구분). 방향/상태를 metric 이름에
  박지 않고 attribute 로 뺀다.
- null 의미론: `value`=실측(0 포함), `value:null`=측정불가/미지원. 가짜값을 채우지 않는다.
  namespace 전체가 null 이면(예: Windows system.pressure) 그 네임스페이스 값이 `null`.

## 메시지 종류와 라우팅 키

| message_type | 라우팅 키 | 주기 |
|---|---|---|
| inventory | server.inventory | 시작 시 + inventory_refresh 간격 |
| metrics | server.metrics | AGENT_INTERVAL_SEC 간격 |
| error | server.error | 수집 실패 시 |
| task.result | task.result | worker task 완료 시 |

worker 는 `agent.tasks.{agent_id}` 큐를 구독하고, 엔진은 그 큐로 task 를 발행한다.
metrics/inventory 는 envelope + system.*/block_devices 를 싣고, task.result/error 는 envelope + 실행 결과·실패 body 를 싣는다(task.result 에 `task_policy` 추가).

## 공통 메타데이터 (모든 메시지)

schema_version("1.0"), message_type, message_id, hostname, machine_id, composite_id, agent_id,
agent_version, boot_time, collected_at, agent_started_at, os_family.

## 식별과 라우팅

- agent_id: 첫 실행 시 생성해 영구 저장하는 UUID(Linux state dir, Windows `%ProgramData%\assessment-agent\agent-id`).
  MAC/machine_id 재발급과 무관하게 불변. 엔진 식별키이자 worker 큐 키.
- composite_id, machine_id: 감사, 표시용. 식별/라우팅에는 쓰지 않는다. machine_id 는 안정 소스
  (/etc/machine-id, dbus, 클라우드 IMDS, Windows MachineGuid)가 없으면 억지로 채우지 않고 null 이다.
  composite_id 는 machine_id 가 없어도 mac 기반으로 유니크가 유지된다.
- prep-image(양 OS) + Linux image-prep.sh 가 agent-id 를 삭제해 골든 이미지 클론마다 새 UUID 를 받게 한다.

## agent_version

payload agent_version 은 스키마 계약 버전이 아니라 릴리즈/빌드 정체성이다. 계약 버전은 `schema_version`.
CI 가 git 태그에서 주입한다(`v1.0.0` -> `1.0.0`). 로컬/dev 빌드는 `0.0.0-dev`. 엔진은 이 값을 스키마
분기 gate 로 쓰지 않고 opaque 메타데이터로 저장한다.

`schema_version`(현재 `"1.0"`, major.minor)은 시스템 통일 계약 버전이다 — wire/assessment API/export/task.install 이
한 major 축을 공유한다(엔진 `contract.py` CONTRACT_VERSION 정본). additive 변경(필드/enum 추가)은 버전을 올리지
않고 소비자가 미지 필드를 관용한다. 구조 파괴만 major 범프(엔진+에이전트+DR 동시 flag-day). 인바운드 task.install 은
에이전트가 major 게이트를 강제한다(major!=1/부재 = 다운로드·실행 전 거부, `failure_reason="unsupported_contract_version"`).

## inventory 필드 (정적 서술자)

호스트 정적 사실. 파생 불가한 host-only 값이다.

- hostname, os_id, os_version, os_codename(Windows null), kernel_version, cpu_model, cpu_cores(논리 코어),
  mem_total_bytes(By), ip_external(IMDS best-effort, 없으면 null).
- OS 재현 서술자(flat 최상위): arch(uname machine / wProcessorArchitecture), bits(32|64, arch 파생),
  boot_firmware(uefi|bios|null; Linux /sys/firmware/efi, Windows GetFirmwareType/env 폴백),
  secure_boot(bool|null; Linux efivars, Windows SecureBoot\State), edition(Windows EditionID, Linux null),
  product_name(Windows CurrentVersion ProductName 원문 — 에이전트 교정 없음, Win11 을 "Windows 10 Pro" 로
  오보하는 등 OS 자체가 부정확할 수 있어 엔진이 kernel_version(빌드번호)과 대조해 해석. Linux null),
  timezone(Linux IANA / Windows tz 키명 원문, 엔진이 IANA 매핑), rtc_utc(bool|null; Linux /etc/adjtime, Windows RealTimeIsUniversal).
- boot(object|null): kernel_cmdline, root_ref_type(uuid|label|partuuid|path), grub_install_target. Linux /proc/cmdline,
  Windows null(GRUB/cmdline 개념 부재).
- nonblock_mounts[](Linux; Windows null): 블록장치 없는 마운트(tmpfs/nfs/cifs/9p/fuse.* + bind). {source,target,fstype,options[],fs_freq,fs_passno}.
- services[]: 서비스 카탈로그(unit/sub/pid/exe). 열거 불가면 null(빈배열과 구분).
- listen_ports[]: 리슨 소켓(proto/addr/port/uid/pid/comm). 포트 기반 서비스 분류용.
- block_devices[], net_interfaces[], lvm_vgs[](Linux 조건부): 아래 "스토리지/네트워크 토폴로지".

## metrics 필드 (system.* 네임스페이스)

collection_interval_sec + 아래 네임스페이스. metrics 는 `system.cpu`/`system.memory`/`system.disk`/
`system.network` 를 반드시 싣는다(스키마 required). 나머지는 지원 시 값, 미지원 시 null.

### system.cpu

| metric | type | unit | attr | Linux 소스 | Windows 소스 | null |
|---|---|---|---|---|---|---|
| cpu.time | counter | s | cpu, state(user/nice/system/idle/iowait/irq/softirq/steal) | /proc/stat per-cpu jiffies/CLK_TCK | NtQuerySystemInformation SystemProcessorPerformanceInformation(class 8) per-cpu 100ns->s(user/system/idle) | 미지원 state |
| cpu.run_queue | gauge | tasks | source(procs_running\|processor_queue) | procs_running | perflib System Processor Queue Length | - |
| cpu.blocked | gauge | tasks | source(procs_blocked) | procs_blocked | (Windows null) | Windows |
| cpu.logical.count | gauge | cpu | - | /proc/stat cpuN 개수 | NtQuery per-cpu 개수 | - |
| cpu.mce | counter | events | - | /proc/interrupts MCE 행 per-cpu 합산 | (WHEA=NT6+ 로드가드 -> null) | Windows |

### system.memory / system.paging

| metric | type | unit | attr | Linux | Windows | null |
|---|---|---|---|---|---|---|
| memory.usage | gauge | By | state(free/cached/buffered/available) | /proc/meminfo(available 3.14+ 실측) | GlobalMemoryStatusEx + perflib | 미지원 state |
| memory.limit | gauge | By | - | MemTotal | ullTotalPhys | - |
| memory.commit.usage/.limit | gauge | By | - | Committed_AS / CommitLimit | perflib Committed Bytes / Commit Limit | - |
| memory.oom_kill | counter | events | - | /proc/vmstat oom_kill(4.13+) | (null) | 커널<4.13; Windows |
| memory.hardware_corrupted | gauge | By | - | /proc/meminfo HardwareCorrupted | (null) | - |
| memory.edac | counter | errors | type(correctable/uncorrectable) | /sys/devices/system/edac/mc*/ce_count·ue_count | (null) | EDAC 미노출(VM) -> null |
| paging.operations | counter | operations | direction(in/out), type(major) | /proc/vmstat pswpin/pswpout, pgmajfault | perflib Pages Input/Output/sec | - |

### system.disk (per device)

Windows saturation 은 죽은 IOCTL_DISK_PERFORMANCE 대신 perflib PhysicalDisk 로 뽑는다(구세대 viostor 도
동작). throughput 은 NtQuerySystemInformation(diskperf 독립·단조) 시스템 전역 집계 + perflib per-disk 폴백.

| metric | type | unit | attr | Linux | Windows | null |
|---|---|---|---|---|---|---|
| disk.io | counter | By | device, direction(read/write) | diskstats sectors*512 | NtQuery 전역(device=aggregate:system) + perflib per-disk Read/Write Bytes/sec raw | - |
| disk.operations | counter | operations | device, direction | diskstats reads/writes | NtQuery 전역 + perflib per-disk Reads/Writes/sec raw | - |
| disk.io_time | counter | s | device | diskstats io_ticks(ms->s) | perflib % Idle Time raw(busy=uptime-idle) | perflib 인스턴스 부재 |
| disk.operation_time | counter | s | device, direction | diskstats time_reading/writing(ms->s) | perflib Avg. Disk sec/Read,Write raw(ticks/PerfFreq->s) | 인스턴스 부재 |
| disk.pending_operations | gauge | operations | device | diskstats in_flight | perflib Current Disk Queue Length | 인스턴스 부재 |
| disk.errors | counter | errors | device, kind, class, (member) | mdraid mismatch_cnt(kind=mdraid class=mismatch_cnt) + ext4 errors_count(kind=ext4 class=errors_count) | (개념 부재 -> 빈 metric) | metric 키 항상 발행; 소스 부재 시 빈 points |

- Windows 전역 집계는 `device="aggregate:system"`, per-disk 는 `device="name:PhysicalDriveN"`.
  둘을 분리해 await(Δoperation_time/Δoperations)가 per-disk 에서 정확히 페어된다.
- disk.io_time 은 uptime 기준 busy 누적기라 재부팅 시 0 리셋(엔진이 counter reset 로 감지). idle 카운터가
  단조 전진하지 않는 비활성 디스크는 io_time 이 높아도 IOPS/queue=0 이라 엔진이 cross-signal 로 걸러낸다.

### system.network (per device; device=MAC 안정키)

| metric | type | unit | attr | Linux | Windows | null |
|---|---|---|---|---|---|---|
| network.io | counter | By | device, direction(receive/transmit) | /proc/net/dev | GetIfTable2(NT6)/GetIfTable(NT5.2) In/OutOctets | - |
| network.packets | counter | packets | device, direction | /proc/net/dev | In/OutUcast+NUcast | - |
| network.errors | counter | errors | device, direction | /proc/net/dev errs | In/OutErrors | - |
| network.dropped | counter | packets | device, direction | /proc/net/dev drop | In/OutDiscards | - |
| network.link.speed | gauge | bit/s | device | /sys/class/net/*/speed(Mbps->bit/s) | Receive/TransmitLinkSpeed | virtio-net/가상 NIC 부재 -> null |
| network.tcp.retransmits | counter | segments | - | /proc/net/snmp RetransSegs | GetTcpStatistics dwRetransSegs | - |
| network.conntrack.usage/.limit | gauge | entries | - | nf_conntrack_count/max | (개념 부재 -> null) | 모듈 미로드 또는 Windows -> null |

### system.pressure (PSI; Linux 4.20+; Windows null)

| metric | type | unit | attr | 소스 | null |
|---|---|---|---|---|---|
| pressure.stall.time | counter | s | resource(cpu/memory/io), scope(some/full) | /proc/pressure/* total(us->s), 14일 saturation canonical | 커널<4.20; Windows |
| pressure.stall.ratio | gauge | 1 | resource, scope, window(avg10/60/300) | /proc/pressure/* avg(%/100) | 상동 |

구 커널(EL6/EL7/Debian10/SLES11-12)·Windows 는 pressure=null. 엔진은 PSI 를 폴백 대체가 아니라 보강으로 쓴다.

### system.filesystem (per mount)

| metric | type | unit | attr | Linux | Windows |
|---|---|---|---|---|---|
| filesystem.usage | gauge | By | device, mountpoint, type, state(used/free/reserved) | statvfs + major:minor->block_devices 조인 id + fstype | GetDiskFreeSpaceEx + 볼륨 GUID + fs명 |
| filesystem.inodes.usage | gauge | count | mountpoint, state(used/free) | statvfs f_files/f_ffree | (NTFS inode 개념 부재 -> null) |

### system.cgroup (컨테이너 배포 시; VM/부재 시 null)

| metric | type | unit | 소스(v2 우선, v1 폴백) |
|---|---|---|---|
| cgroup.cpu.throttled.count | counter | events | cpu.stat nr_throttled |
| cgroup.cpu.throttled.time | counter | s | cpu.stat throttled_usec(v1 throttled_time ns) |
| cgroup.memory.usage | gauge | By | memory.current / memory.usage_in_bytes |
| cgroup.memory.limit | gauge | By | memory.max / memory.limit_in_bytes(무제한 sentinel 은 생략) |

Windows 는 cgroup 부재 -> system.cgroup=null. 신호가 전무하면 namespace=null.

## 스토리지/네트워크 토폴로지 (inventory)

### block_devices[] (정규화 단일 그래프)

부모-링크 평면 노드. 필드: name(표시용), type, size_bytes(By), fstype(null 가능), mountpoint(null 가능),
parent(부모의 id 값; root=null; 부모 복수면 노드 반복), id + id_type.

- type: disk/part/lvm/crypt/raid/mpath/dynamic/volume/swap(pass-through). swap=스왑 파티션/pagefile 노드.
- Linux 수집: /sys/block whole-disk + 파티션 + dm(dm/uuid prefix 로 lvm/crypt/mpath 판정)/md,
  슬레이브(/sys/block/*/slaves)로 parent 링크, swap 은 /proc/swaps.
- Windows 수집: 물리디스크(IOCTL_DISK_GET_LENGTH_INFO) + part(IOCTL_DISK_GET_DRIVE_LAYOUT_EX, parent=disk id) +
  볼륨(FindFirstVolume). 볼륨->디스크 parent 는 IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS(스팬/동적 볼륨은 extent
  디스크별로 노드 반복). 레이아웃 IOCTL(DRIVE_LAYOUT_EX)은 구세대 viostor 에서도 동작.

재현(reproduction) 확장 키는 자연 노드타입에만 발행하고 비해당 노드는 키를 생략한다(엔진 OUTPUT 에서 null).
- disk 노드: partition_table(gpt|mbr|null), sector_size, serial, wwn(0x+소문자hex; Linux by-id, Windows VPD 0x83 시도·장치 미제공 시 null),
  rotational(true HDD/false SSD). Linux sysfs queue/*, Windows geometry/STORAGE_DEVICE_DESCRIPTOR/seek-penalty(NT5.2 null).
- part 노드: part_number, part_start_bytes, part_type(MBR '0x'+hex / GPT 소문자무중괄호 GUID), part_name(GPT 레이블),
  part_flags(esp/bios_grub/lvm/raid/swap/msftres + required/legacy_boot/hidden/no_automount/boot; 무플래그=[], 파싱실패=null).
  Linux=GPT LBA1/MBR LBA0 직독(O_NONBLOCK+bounded)+sysfs, Windows=DRIVE_LAYOUT_EX. id_type=partuuid(GPT)/name(MBR).
- fs 얹힌 노드: fs_uuid, fs_label, block_size, mount_options[], fs_freq, fs_passno. Linux=by-uuid/by-label 역매핑+
  statvfs+fstab(UUID=/LABEL=/PARTUUID=/dev/마운트포인트 매칭), Windows=GetVolumeInformation(볼륨시리얼 XXXX-XXXX/label)+클러스터.
- lvm 노드: lvm_vg, lvm_lv(dm/name 파싱), lvm_segtype, lvm_stripes, lvm_stripe_size_kib(/etc/lvm/backup 파싱).
- raid 노드: raid_level(0|1|5|6|10, 비수치 null), raid_chunk_kib, raid_metadata, raid_uuid(/sys/block/md*/md/*).
- crypt 노드: crypt_type(luks1|luks2|null; dm/uuid CRYPT-LUKS prefix). 그 외(PLAIN/VERITY/BitLocker)=null.

### lvm_vgs[] (Linux; /etc/lvm/backup 존재 시)

{name, vg_uuid, extent_size_bytes, size_bytes, free_bytes, pv_ids, data_percent, metadata_percent} 발행.
/etc/lvm/backup/<vg> 텍스트 메타데이터 파싱(device raw read·외부명령 없이)으로 뽑는다: name/vg_uuid/extent_size,
size=sum(pe_count)*extent_size*512, free=(sum(pe_count)-alloc_pe)*extent_size*512. alloc_pe 는 물리 PE 를 소비하는
세그먼트(type=striped/linear)의 extent_count 만 합산한다 — thin/raid/cache/snapshot 의 상위·가상 세그먼트 extent_count 는
가상 크기라 PE 를 안 쓰고 실제 소비는 hidden sub-LV(_tdata/_rimage/_cdata 등)의 striped 세그먼트에 있어 그것만 센다
(전 세그먼트 합산 시 이중계산으로 free 가 음수->0 위조). pv_ids=각 PV device 를
resolve 한 block_device 안정 id(join-ready). data_percent/metadata_percent 는 런타임 충전율이라 정적 백업엔 없어,
device-mapper ioctl(/dev/mapper/control DM_TABLE_STATUS — dmsetup 같은 외부명령 없이)로 thin-pool 타깃 status 의
used/total 블록을 직접 읽어 percent 로 발행한다. VG 당 thin-pool 정확히 1개일 때만 반영(0/다수/status 포맷 불일치 -> null).
LVM 토폴로지는 block_devices 의 type=lvm 노드(dm/uuid parent 체인 + lvm_vg/lvm_lv/
segtype/stripes)로도 커버된다. /etc/lvm/backup 부재 시 키 생략.

### net_interfaces[]

{name(표시용), id, id_type, kind, speed_mbps, addresses[], gateway, mtu, dns[], routes[], bond_mode, vlan_id}.
addresses[]={address, prefix, family, origin}. Linux getifaddrs + /sys/class/net(MAC id, 폴백 by-path/name).
Windows GetAdaptersAddresses(MAC id).

- speed_mbps: Linux /sys/class/net/*/speed, Windows NT6+ Transmit/ReceiveLinkSpeed(Mbps 환산). Windows NT5.2 및 virtio/가상 NIC 등 속도 부재 시 null. 그 호스트 링크속도는 metrics `network.link.speed`(bit/s)에 실리므로, 소비자는 inventory speed_mbps 가 null 이면 link.speed 로 폴백한다(중복 발행 회피 -> 한쪽만 실측).
- mtu: Linux sysfs mtu / Windows Mtu. routes[]: 정적 비-default IPv4 {dest CIDR, via}(Linux /proc/net/route dest!=0 & gw!=0,
  Windows GetIpForwardTable NETMGMT). 조회실패 null, 무라우트 [].
- dns[]: Linux 전역 /etc/resolv.conf 를 default-route 인터페이스에만 부착(그 외 null), Windows per-adapter FirstDnsServerAddress.
- bond_mode: Linux sysfs bonding/mode raw 토큰(엔진 정규화), Windows null. vlan_id: Linux /proc/net/vlan VID, Windows null.
- origin(static|dhcp): Windows NT6+ PrefixOrigin 실측(NT5.2 null), Linux netlink RTM_GETADDR IFA_F_PERMANENT(permanent=static, else dhcp).

### device 안정키 (계층 폴백)

metric 의 device attr 와 block_devices/net_interfaces 의 id/parent 는 이름(perflib `0 C:`, Linux `dm-N`)이
재부팅/디스크추가에 바뀌므로 안정 id 를 조인 키로 쓴다. `id`(값) + `id_type`(라벨) 동반. parent 는 부모의 name 이
아니라 부모의 id 값으로 링크한다. metric device attr 는 `<scheme>:<value>` 결합형(예 `serial:...`, `mac:...`,
`aggregate:system`), block_devices/net_interfaces 는 id/id_type 분리형.

- 디스크(Linux): dm/uuid -> serial -> by-path -> name. 파티션: partuuid -> name.
- 디스크(Windows): GPT DiskId(gptid) -> MBR Signature(mbrsig) -> serial -> name. 볼륨: volume GUID(volguid).
- 네트워크: MAC(mac). 폴백 Windows ifguid, Linux by-path, 최후 name.
- id_type enum(디스크/블록 축): dm/partuuid/wwid/serial/by-id/by-path/fsuuid/gptid/mbrsig/volguid/name/null. 네트워크 축: mac/ifguid/by-path/name/null. mac/ifguid 는 네트워크 전용이라 디스크 축 enum 에 없다(축별 분리).

## USE 매핑 (Layer 2; 엔진 해석 참조)

wire 가 아니라 엔진 해석의 참조표다. source attribute 가 OS별 신호원 비대칭을 노출해 엔진은 os_family
분기 없이 source 로 판별/랭킹한다.

| 자원 | 축 | 산출(Layer1 raw 로부터) |
|---|---|---|
| CPU | U | 1 - rate(cpu.time[idle]) / Σrate(cpu.time) |
| CPU | S | pressure(cpu, some) 우선, 없으면 cpu.run_queue/logical.count |
| Memory | U | 1 - memory.usage[available]/memory.limit |
| Memory | S | pressure(memory) 우선, 없으면 paging.operations[out] rate / commit.usage:limit |
| Disk | U | rate(disk.io_time) (장치 busy 비율) |
| Disk | S | Δdisk.operation_time/Δdisk.operations(await) 우선, disk.pending_operations 보조 |
| Network | U | rate(network.io)/network.link.speed(속도 null 이면 분모 부재) |
| E(전 자원) | E | rate(disk.errors/network.errors/network.tcp.retransmits/memory.edac/hardware_corrupted) |

E축은 사이징 숫자가 아니라 엔진의 confidence(오염 게이트) + monitoring/attention 으로 소비된다.

## task.result / error 필드

envelope + 실행 결과·실패 body. 상세 필드/조건은 스키마 $defs(task_result/error) 참조.

- task.result: task_id 로 매칭. exit_code/signal_no 상호배타(Windows signal_no 항상 null).
  task_policy(bool\|null)는 exit_code 보다 우선. boot_time/agent_started_at 은 워커 컨텍스트라 nullable.
- error: envelope(composite_id 포함) + error_code/error_message/failed_component. hostname 은 공통 경로가 항상 발행.

## 값 의미론 (0 / null)

- 값(0 포함)=실측, null=측정불가/미지원. OS 에 개념이 없는 필드를 가짜값으로 채우지 않는다.
- 신호별 하한 미달은 null: PSI 4.20+, MemAvailable 3.14+, oom_kill 4.13+, virtio-net link speed,
  EDAC(VM 미등록), NTFS inode.

## OS별 차이 (스키마 조건부로 강제)

- system.pressure: Windows 는 null(스키마 `os_family=windows -> system.pressure:null`).
- lvm_vgs: Windows 는 미발행(스키마 `not required lvm_vgs`).
- system.cgroup: Windows null. cpu.blocked/cpu.mce/memory.oom_kill/edac/conntrack: Windows null.
- os_codename: Windows null. task.result 의 Windows signal_no 항상 null.

## 빌드/릴리즈

빌드·툴체인·릴리즈는 [docs/BUILD.md](BUILD.md). wire 계약 정본은 [schema/wire.schema.json](../schema/wire.schema.json).
