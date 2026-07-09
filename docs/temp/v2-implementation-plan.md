# wire v2 구현 플랜 (에이전트, 2트리) — 검증 반영본

> 계약 락됨(v2-lock-confirm + inventory-gap-reply). 이 플랜은 실행 트래커. 각 페이즈는 "양 트리 구현 + emit 4종이 v2 스키마 통과(check-contract) + testbed 검증"을 게이트로 닫는다.
> 작업 전 adversarial 검증(verify-v2-impl-plan 워크플로) 반영: H1/H2/M1/M2 필수 + M3/M4/L1-L4 정정.

## 0. 전략 / 불변 원칙

- 브랜치: `feat/wire-v2` (분기 완료). 페이즈별 커밋. 완성 전 main 미머지.
- 동반 착지: `schema/wire.schema.json`(v2 교체 완료)와 collect.c 재작성은 같이 가야 check-contract가 안 깨진다.
- 증분 유효(정정 M2): 매 페이즈 끝 emit 4종이 v2 스키마 통과. 미완 system.* 네임스페이스(object|null)는 비거나 null이되, required 비-nullable 배열(inventory block_devices/net_interfaces)은 빈 배열 `[]`로 항상 발행(키 생략/null 이면 FAIL). required 스칼라(hostname/os_*/cpu_cores/mem_total_bytes 등)도 P1부터 채운다.
- 2트리 병행: 모든 수집 변경은 src/collect.c와 windows-agent/src/collect.c 양쪽.
- stateless raw: 에이전트는 emit 시점 raw 누적만. rate/delta/util/await 계산 없음. base 단위(s/By/ratio) 정규화만.
- 검증 하네스(정정 M4): 수치 불변식 probe를 git 추적 경로 `scripts/accept/`에 커밋(scratchpad 의존 폐기). check-contract.sh + validate-wire.py는 구조만 보므로, 값 정합/시계열 불변식은 accept probe가 담당. 페이즈->불변식 매핑표를 accept README에.

## 1. 공통 인프라 (P1 동반, 선행)

- schema: 교체 완료(v2). error $def에 hostname 추가됨(H1 — add_common_metadata가 항상 발행).
- envelope schema_version 배정(정정 M1): `schema_version:"2.0"`을 두 경로 모두에 추가한다 —
  - inventory/metrics/error: `add_common_metadata`(src/collect.c:79, windows-agent/src/collect.c:98).
  - task.result: `build_result_json_raw`(src/worker.c:153, windows-agent/src/worker.c:204) — add_common_metadata를 안 타므로 여기에 직접. 래퍼(worker_emit_sample_result_json)가 아니라 _raw 단일 소스에.
- task.result: 위 schema_version + `task_policy`(bool|null) 추가.
- error: v1 body 유지 + schema_version. hostname은 add_common_metadata가 이미 발행(스키마가 이제 허용).
- metric 헬퍼(양 트리 신규): `ns_get`/`metric_new(type,unit)`/`point_add(attr..,value)` — datapoint-array 빌더. 156개 flat cJSON_Add 대체.
- emit(main.c): dispatch 유지.
- check-contract.sh: 스키마 경로 그대로(schema/wire.schema.json). v2 emit 4종 통과 확인.
- revert(정정 L1): EnableCounterForIoctl은 워킹트리 미커밋으로 이미 discard됨(installer.c/BUILD.md엔 애초 코드/문구 없음 — 플랜에서 그 항목 삭제). 실제 폐기 대상은 windows-agent/src/collect.c `query_disk_queue`(IOCTL_DISK_PERFORMANCE, 정의 ~374-396, 호출 ~1562) 제거 + payload-contract.md:176 IOCTL 산문 정정. P1에서 함께.

## 2. 페이즈 시퀀스

### P1. 공통 인프라 + 디스크 USE + inventory 최소골격 (최우선)
- 공통 인프라(1절) 전체 + metric 헬퍼 + schema_version 양 경로.
- inventory 최소 골격(정정 M2): P1부터 emit inventory가 스키마 통과하도록 required 전부 발행 — hostname/os_id/os_version/os_codename/kernel_version/cpu_model/cpu_cores/`mem_total_bytes`(현행 mem_total_kb 키명 정정)/ip_external + services/listen_ports(v1 수집 재배선) + `block_devices:[]`/`net_interfaces:[]`(빈 배열 스텁, 실내용은 P4). Windows lvm_vgs 미발행 유지.
- system.disk 소스 전략(정정 H2 — 핵심):
  - throughput(disk.io=By, disk.operations): 현행 유지 = Linux /proc/diskstats, Windows NtQuerySystemInformation(SystemPerformanceInformation, diskperf 독립·단조) 1차 + perflib PhysicalDisk 폴백. IOCTL 아님.
  - saturation(disk.io_time=%util, disk.operation_time=await, disk.pending_operations=queue): 죽은 IOCTL_DISK_PERFORMANCE(query_disk_queue) 폐기 -> Windows는 perflib PhysicalDisk(% Idle Time/Avg.Disk sec/Current Disk Queue Length)로. 단 perflib는 diskperf 의존이라 일부 환경서 raw=0/비단조(collect.c:606 주석) -> 유효성 게이트: raw!=0 & 단조일 때만 값, 아니면 null(값=실측/null=측정불가 계약 준수, 가짜 0 금지). Linux는 /proc/diskstats(io_ticks/time_reading/writing)로 동일 축.
  - diskstats 필드 매핑(정확): name 이후 f1 reads_completed, f2 reads_merged, f3 sectors_read, f4 ms_reading, f5 writes_completed, f6 writes_merged, f7 sectors_written, f8 ms_writing, f9 in_flight, f10 io_ticks(ms), f11 weighted. -> io_time=f10, operation_time read=f4/write=f8, operations read=f1/write=f5, io read=f3*512/write=f7*512, pending=f9. (플랜 기존 f7/f11/f13 표기는 오독 — 이 매핑으로 정정.)
  - device 안정키 계층 폴백: dm/uuid->partuuid->wwid->serial->by-id->by-path->name.
- 게이트: emit 4종 유효(check-contract) + dp-win2016(생존, 구세대 viostor)에서 perflib raw!=0 & 단조 검증 + raw vs cooked(Get-Counter) 대조 + Linux st-lvm 시계열 불변식(%util∈[0,1]/await sane/단조). (정정 M3: dp-win2012 재생성은 NT6.1 고유 델타가 실제 식별될 때만 — 담당/리드타임/폴백 명시해 별도 요청.)

### P2. CPU + Memory + Paging USE
- system.cpu: cpu.time(per-cpu x state, s), cpu.run_queue, cpu.blocked(Windows null), cpu.mce, cpu.logical.count(gauge,cpu). Linux /proc/stat cpuN, jiffies/CLK_TCK.
  - Windows per-cpu(정정 L2): NtQuery(SystemProcessorPerformanceInformation, class 8, 44B/코어 선형). query_system_io의 고정 2048B 버퍼 재사용 금지 — perf_query식 동적 재할당(malloc + ERROR_MORE_DATA/LENGTH_MISMATCH realloc 루프)으로 고코어(>46) 무음 드롭 방지. GetProcAddress 해소(하드임포트 금지).
- system.memory: usage(state used/free/cached/available), limit, commit.usage/.limit, oom_kill(vmstat 4.13+/null), hardware_corrupted, edac. Windows GlobalMemoryStatusEx+perflib.
- system.paging: paging.operations(direction in/out, type major).
- 게이트: per-cpu 코어수=nproc/logical, available 대조, 구커널 null(centos6 MemAvailable).

### P3. Network + PSI
- system.network: io/packets/errors/dropped(direction), link.speed(bit/s), tcp.retransmits, conntrack.usage/.limit(Linux; Windows 미발행 D10). device attr=MAC.
  - Linux: /proc/net/{dev,snmp,netstat}, /sys/class/net/*/{address,speed}, nf_conntrack.
  - Windows(정정 L3): errors/dropped/io/packets는 MIB_IF_ROW2(GetIfEntry2 dispatch, collect.c:744-747; NT5.2 GetIfTable 폴백 789-792) — WMI(Win32_PerfRawData_Tcpip) 미채택(COM 신규표면 회피). tcp.retransmits는 GetTcpStatistics(MIB_TCPSTATS dwRetransSegs, GetProcAddress). link.speed=MIB_IF_ROW2 TransmitLinkSpeed.
- system.pressure(Linux): pressure.stall.ratio+time(resource/scope/window). Windows/구커널 null.
- inventory net_interfaces 실내용(P4와 함께 or 여기): addresses[]/gateway/kind.
- 게이트: MAC 키 안정(t0/t1), PSI 대조, virtio speed null, Windows PSI null.

### P4. 스토리지 토폴로지 + Filesystem (inventory 실내용)
- block_devices[] 실내용: 정규화 노드 + type=swap. Linux lsblk 풀트리 + sysfs holders/slaves 폴백(EL6/7, 슈퍼블록 fstype). Windows layout IOCTL(DRIVE_LAYOUT_EX+VOLUME_DISK_EXTENTS), 동적디스크 물리매핑. lvm_vgs[].
- net_interfaces addresses/gateway/kind, ip_external 실측(P3서 미완이면).
- system.filesystem: usage(By, state used/free), inodes.usage(Windows null).
- 게이트: st-nested(7계층)/st-lvm(stripe/thin id)/dp-win2016(basic+dynamic)/centos6(sysfs 폴백) 대조.

### P5. Errors 축 완전체
- disk.errors: mdraid(/sys/block/md*/md)/btrfs(/sys/fs/btrfs/*/devinfo/*/error_stats)/ext4(/sys/fs/ext4/*/errors_count)/ioerr, Windows 이벤트로그. device attr scheme = md/btrfsuuid/fsuuid 등(D4 카탈로그).
- net tcp TcpExt, memory hardware_corrupted/edac(P2), cpu.mce(P2).
- 게이트: st-raid/st-btrfs/st-mpath 소스 파싱.

### P6. cgroup + 마감
- system.cgroup(컨테이너, st-cgroup). VM null.
- 마감: 전 페이즈 emit 4종 v2 통과, 양 트리 빌드+verify(NT5.2 로드가드), check-contract 그린, 문서(payload-contract/BUILD/CLAUDE) v2 격상.

## 3. 2트리 대응 맵

| 개념 | Linux | Windows |
| --- | --- | --- |
| schema_version(inv/metrics/error) | add_common_metadata(collect.c:79) | add_common_metadata(collect.c:98) |
| schema_version+task_policy(task.result) | build_result_json_raw(worker.c:153) | build_result_json_raw(worker.c:204) |
| metrics/inventory 빌더 | collect_metrics/inventory_payload 재작성 | 동(fill_saturation 흡수) |
| 디스크 throughput | /proc/diskstats | NtQuery 1차 + perflib 폴백(유지) |
| 디스크 saturation | /proc/diskstats io_ticks/time | perflib PhysicalDisk(IOCTL query_disk_queue 폐기) + 유효성 게이트 |
| per-cpu | /proc/stat cpuN | NtQuery class8 동적버퍼 |

## 4. 검증 하네스 (매 페이즈)

1. build: build-linux.sh(alpine musl) + windows docker(bookworm) -> verify(static/NT5.2 로드가드 NT6_FORBID_SYMS).
2. contract: check-contract.sh 양 바이너리 emit 4종 -> v2 스키마 그린.
3. accept(정정 M4): scripts/accept/의 git 추적 probe로 값 정합/시계열 불변식.
   - Linux: st-lvm/centos6(rescue)/st-raid/st-btrfs/st-mpath. emit vs 네이티브, 불변식.
   - Windows: dp-win2016(생존, 구세대 viostor, 부하하네스 부착됨). raw vs cooked, perflib raw!=0/단조. dp-win2012 재생성은 NT6.1 델타 식별 시만.
   - win2003(정정 L4): WinRM/PS 부재 -> 오프라인 emit dry-run(wine) + 스키마 contract로 커버. 라이브 엔진 대조 제외.

## 5. 리스크 / 완화

- (H2) Windows perflib 회귀: throughput은 NtQuery 유지(diskperf 독립), saturation만 perflib+유효성 게이트(raw!=0/단조 아니면 null). 무조건 IOCTL/NtQuery 폐기 금지.
- diskstats/perflib 필드 오프셋 오독(프로토타입서 실발생): 시계열 불변식+raw vs cooked로 각 페이즈 차단.
- NT5.2 로드가드: 신규 API(NtQuery per-cpu/GetTcpStatistics) GetProcAddress 해소, verify 강제.
- per-cpu 동적버퍼(L2), 페이로드 팽창 상한은 후순위(현 VM 규모).
- 2트리 드리프트: check-contract 동일 스키마 강제 + metric 헬퍼 구조 통일.

## 6. 착수 순서

검증 필수 4건(H1 스키마 hostname[완료], H2 소스전략, M1 task.result schema_version, M2 inventory 스텁+mem_total_bytes) 반영됨. P1 = feat/wire-v2에서 (a) revert 스코프 정정(query_disk_queue) (b) 공통 인프라(envelope 양경로/metric 헬퍼) (c) inventory 최소골격(required 전부, 배열 빈스텁) (d) system.disk(throughput NtQuery 유지 + saturation perflib 게이트) (e) 빌드+contract+accept(dp-win2016) 게이트. 그린이면 P2.
