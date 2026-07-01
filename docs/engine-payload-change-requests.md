# 엔진 -> 에이전트 payload 개선 요청

엔진(assessment-engine) 쪽에서 "에이전트를 못 고쳐서 엔진이 대신 보정/추론하던" 로직을 전수 조사한 결과다. 각 항목은 에이전트가 데이터를 더 좋은 형식/규칙으로 보내면 엔진의 흡수/추론 로직을 제거할 수 있는 것들이다.

self-contained — 엔진 코드를 몰라도 읽히게 각 항목에 "엔진 현상 / 원인 / 에이전트 변경 / 형식 예시 / 엔진이 얻는 것"을 담았다. 코드 위치는 실측 기준(파일:라인)이다. 검토 후 "각 항목 반영 가부 + 실제 채택할 필드명/구조 + 제약"을 회신해주면 엔진 쪽 짝 변경(스키마·mapper·필터 제거)을 맞춰 진행한다.

## 전환 원칙 (무중단)

엔진 inbound 모델은 `extra=ignore`라 에이전트가 새 필드를 추가해도 안 깨진다.
1. 에이전트가 신형 필드를 구형과 함께 발행(additive). 엔진 영향 0.
2. 엔진이 "신형 있으면 신형, 없으면 구형" 분기로 읽음.
3. 전 배포 완료 후 `agent_version` major bump -> 엔진이 구형 경로·흡수 로직 제거.

## 우선순위 요약

| # | 항목 | 값 | 비고 |
|---|------|----|------|
| 1 | device 분류 규칙 통일 (disk/net/mount 물리·가상) | HIGH | 규칙이 에이전트·엔진 7곳에 드리프트 — 핵심 |
| 2 | services <-> listen_ports pid join | HIGH | Windows dwProcessId 이미 보유 |
| 3 | ip_internal 구조화 + iface명 + virtual + IPv6 | HIGH | 현재 IPv4만·CIDR 문자열·가상 미태그 |
| 4 | 식별자: MAC 파생 해시 -> 안정 UUID | HIGH(장기) | 엔진 식별 마이그레이션 동반 |
| 5 | Windows saturation 카운터 | MEDIUM | PDH 카운터 |
| 6 | Windows 메트릭 canonical 매핑 | MEDIUM | 값 정합 |
| 7 | boot_time 정적 소스 | MEDIUM | 현행 확인 필요 |
| 8 | task.result os 필드 보강 | LOW | |

---

## 1. device 분류 규칙 통일 (disk / net / mount) — HIGH

네가 지적한 "물리·가상 device 규칙 불일치"의 핵심. 같은 "이 device가 가상이냐" 판정이 지금 에이전트(Linux)·에이전트(Windows)·엔진에 걸쳐 서로 다른 규칙으로 흩어져 드리프트한다.

### 현재 규칙이 흩어진 위치 (실측)

Linux 에이전트 (`src/collect.c`) — 수집기마다 다른 필터:
- 디스크 (`is_excluded_block_dev`, L131): 제외 = {`loop*`,`ram*`,`sr*`,`fd*`}. dm-/md(LVM/RAID)·zram·nbd·파티션은 제외 안 함 -> 엔진으로 넘어감.
- MAC (`collect_mac_addresses`, L863): 제외 = {`lo`,`docker*`,`br-*`,`veth*`,`virbr*`} + `type==1`(ethernet)만.
- ip_internal (`collect_internal_ips`, L821): 제외 = loopback만. IPv4만(IPv6 아예 미수집). docker0/virbr0/veth가 IP를 가지면 그대로 실림.
- net_io (`collect_net_io`, L1521): 제외 = `lo`만. docker/br/veth/virbr/bond 전부 실림.

Windows 에이전트 (`windows-agent/src/collect.c`):
- 디스크 (`enumerate_physical_disks`/`enumerate_disk_io`): PhysicalDrive0..31만. `major=0` 고정.
- mounts (`enumerate_mounts`): DRIVE_FIXED만. `major=0` 고정.
- net_io (`enumerate_net_io`): 어댑터 전부, 필터 없음.

엔진 (`web/services/device_filters.py` + `db/repositories/query/types.py` SQL) — 또 다른(가장 긴) 규칙:
- 디스크 제외: {loop,ram,zram,fd,sr,nbd} + {dm-,md} + 파티션(sdaN/vdaN/hdaN/xvdaN/nvme..pN/mmcblk..pN).
- net 제외: {lo,veth,sit,tunl,ip6tnl,gre,gretap,erspan,dummy,ifb,nlmon} + {bondN,teamN} + {brN,br-*,dockerN,virbrN} + vlan(eth0.100) + Windows 필터드라이버(`<adapter>-<filter>-NNNN`).
- mount: `major==0` + Windows drive letter + `/boot` + 이미지 fstype(squashfs/iso9660/udf).

문제:
1. 같은 판정이 7곳 이상에 중복·드리프트. Linux net_io는 `lo`만 빼는데 엔진은 거대 목록을 뺀다 -> 에이전트가 안 뺀 걸 엔진이 정규식으로 뺌(이중). 목록이 어긋나면 leak(가상이 물리로 집계) 또는 double-count(bond master+member).
2. Windows는 major가 항상 0 -> 엔진의 major 기반 판정(물리 디스크·데이터 볼륨)이 무력 -> drive-letter 정규식으로 우회. 그래서 엔진은 Windows 물리 디스크 capacity를 환경 합산에서 제외한다.
3. 엔진 안에서도 "표시 경계 필터"(토폴로지 그래프·차트 — `is_virtual_interface`는 표시 경계에서만 적용, 저장은 전부 유지)와 "집계 필터"(cagg SQL)가 따로라, 화면 표현과 로직 필터 기준이 갈릴 수 있다.

### 원칙적 해결 — 에이전트가 device를 태그, 엔진은 태그로만 필터

device 종류는 OS를 직접 읽는 에이전트가 가장 정확히 안다. 규칙을 에이전트 한 곳(공용 분류 함수)으로 모으고, 엔진은 정규식/major 추론을 버리고 태그로만 필터한다. 이러면 규칙이 단일화되고 Linux/Windows·표시/집계가 같은 기준이 된다.

- 에이전트: device를 pre-drop하지 말고 전부 실되, 각 항목에 명시 태그를 붙인다 (Linux·Windows 동일 taxonomy).
  - 디스크 (disks/disk_io): `kind` = `physical` | `partition` | `lvm` | `raid` | `virtual`
  - 인터페이스 (net_io + 아래 3번 interfaces): `kind` = `physical` | `loopback` | `bridge` | `veth` | `bond_master` | `bond_member` | `vlan` | `tunnel` | `virtual`
  - 마운트 (mounts): `kind` = `data` | `virtual_fs` | `boot` | `image`, + `fstype`
- 한 device를 여러 메시지(inventory·metrics)가 다르게 태그하면 안 됨 — 분류 함수 하나를 공용으로.
- Windows: major=0 대신 위 태그로 물리/데이터 판정. 물리 디스크는 이미 PhysicalDrive로 잘 잡으니 `kind:"physical"`만 붙이면 됨.

형식 예시:
```
"disk_io": [
  {"device": "nvme0n1",   "kind": "physical",  "reads_completed": 12345, "sectors_read": 999, "major": 259, "minor": 0},
  {"device": "nvme0n1p1", "kind": "partition", "..." : 0},
  {"device": "dm-0",      "kind": "lvm",        "..." : 0}
]
"net_io": [
  {"interface": "eth0",     "kind": "physical", "rx_bytes": 1, "tx_bytes": 1},
  {"interface": "docker0",  "kind": "bridge",   "rx_bytes": 1, "tx_bytes": 1},
  {"interface": "veth1a2b", "kind": "veth",     "rx_bytes": 1, "tx_bytes": 1}
]
"mounts": [
  {"mount": "/",     "kind": "data",       "fstype": "ext4", "total_bytes": 1},
  {"mount": "/proc", "kind": "virtual_fs", "fstype": "proc"},
  {"mount": "/boot", "kind": "boot",       "fstype": "ext4"},
  {"mount": "C:\\",  "kind": "data",       "fstype": "ntfs", "total_bytes": 1}
]
```

엔진이 얻는 것:
- `device_filters.py` 정규식 catalog(`_VIRTUAL_DISK_RE`·`_LVM_DISK_RE`·`_PART_DISK_RE`·`_VIRTUAL_IFACE_RE`·`_WIN_IFACE_FILTER_RE`) 폐기.
- cagg/query SQL 필터가 `WHERE kind='physical'`(또는 명시 규칙)로 단순화.
- Windows major=0 우회·capacity 제외 해소.
- 표시 필터 vs 집계 필터 불일치 소멸 — 태그가 단일 진실.

정책 한 줄로: "가상이냐"를 에이전트 공용 분류 함수 하나가 정하고, 그 결과를 `kind`로 실어라. pre-drop(지금 Linux가 lo/loop 등 미리 버림)은 최소화하고 태그로 대체 — 관측성 유지 + 규칙 단일화. lo/loop처럼 절대 무의미한 것만 계속 drop해도 되나, 그 정책도 한 곳에 문서화.

---

## 2. services <-> listen_ports pid join — HIGH

엔진 현상: 서비스 카테고리(db/web/mq/...) 분류 시, `services[]`(unit·sub)와 `listen_ports[]`(proto·port·comm·pid) 사이에 join key가 없어서, listen 소켓을 직접 분류(`detect_listen_categories`)한 뒤 이름 분류와 합집합하는 우회를 쓴다. opaque 서비스 이름(`MSSQL$INST01`,`W3SVC`)은 per-unit으론 못 잡고 listen 소켓으로만 구제.

원인: `services[]`가 pid를 안 실어 "이 unit이 그 포트를 연다"를 확정 못 함. listen_ports는 이미 pid를 싣는다(Linux는 `/proc/PID/fd` socket inode 매칭, Windows는 소켓 소유 pid). services만 pid가 없다. Windows는 `EnumServicesStatusExW(SC_ENUM_PROCESS_INFO)`가 dwProcessId를 이미 쥐고도 안 싣는다. Linux는 `systemctl list-units` 파싱이라 pid 없음(단 `systemctl show <unit> -p MainPID` 또는 cgroup으로 획득 가능).

에이전트 변경: `services[]` 각 항목에 `pid`(가능하면 `exe`) 추가.
```
"services":     [{"unit": "MSSQL$INST01", "sub": "running", "pid": 1234, "exe": "sqlservr"}]
"listen_ports": [{"proto": "tcp", "addr": "0.0.0.0", "port": 1433, "pid": 1234, "comm": "sqlservr"}]
```

엔진이 얻는 것: pid join으로 per-unit 정확 분류 -> `detect_listen_categories` union 우회 축소.

---

## 3. ip_internal 구조화 + iface명 + virtual + IPv6 — HIGH

엔진 현상: `ip_internal`을 CIDR 문자열 리스트로 받아 `ip_interface`로 파싱. 주소에 인터페이스 이름이 없어 토폴로지 그래프에서 docker0/virbr0 가상망을 정규식 휴리스틱으로 제외.

원인 (실측 `collect_internal_ips`, L814):
- CIDR 문자열(`"10.0.1.15/24"`)이라 엔진이 파싱해야 함.
- 인터페이스 이름 없음 -> 엔진이 가상망 추론.
- loopback만 제외 -> docker0/virbr0/veth의 IP가 그대로 실림(엔진이 다시 걸러야 함).
- IPv4만(`AF_INET`) -> 내부 IPv6 주소는 아예 미수집.

에이전트 변경: 문자열 배열을 구조화 인터페이스 배열로 (1번 net `kind`와 같은 분류 신호 공유):
```
// before
"ip_internal": ["10.0.1.15/24"]

// after
"interfaces": [
  {"name": "eth0",    "address": "10.0.1.15",  "prefix": 24, "family": "ipv4", "kind": "physical"},
  {"name": "docker0", "address": "172.17.0.1", "prefix": 16, "family": "ipv4", "kind": "bridge"},
  {"name": "eth0",    "address": "fd00::1",    "prefix": 64, "family": "ipv6", "kind": "physical"}
]
```
- address/prefix 분리, family 명시, kind로 가상 구분, IPv6 포함.
- `ip_external`은 같은 배열에 `scope:"external"`로 합치거나 별도 유지(협의).

엔진이 얻는 것: 문자열 파싱·IPv4/IPv6 추론·토폴로지 가상망 휴리스틱 제거. 1번 `net_io.kind`와 동일 분류라 화면 간 일관.

---

## 4. 식별자: MAC 파생 해시 -> 안정 UUID — HIGH(장기)

엔진 현상: 호스트 식별자 `composite_id = sha256(machine_id + 정렬·dedup된 MAC들)`. OpenStack Windows VM이 부팅마다 NIC MAC을 재발급하면 composite_id가 바뀌어 같은 VM이 중복 행. 엔진이 `machine_id+hostname`으로 기존 행을 찾아 재연결(relink)하는 흡수 로직을 둔다.

원인: 식별자가 휘발성 MAC에 의존.

에이전트 변경(정석): 첫 install 시 UUID 생성 -> 영구 저장(`/var/lib/.../agent-id` 등) -> 그 UUID를 식별자로 발행. MAC 무관 불변.
```
"agent_id": "550e8400-e29b-41d4-a716-446655440000",  // 첫 install 시 생성·영구 저장
"machine_id": "...",                                  // 표시 전용 유지
"mac_addresses": [...]                                // 감사용 유지(식별 미사용)
```

주의: 엔진 식별 키(DB UNIQUE·MQ 라우팅 `agent.tasks.{composite_id}`) 전면 마이그레이션이라 엔진 쪽 별도 ADR·이행 계획 필요. 1~3과 분리 검토. 에이전트는 먼저 `agent_id`를 additive로 실어두면 나중 전환에 대비됨.

---

## 5. Windows saturation 카운터 — MEDIUM

엔진 현상: right-sizing(USE Method)은 utilization(CPU%/MEM%)뿐 아니라 saturation(대기·병목)으로 "이용률 낮아 보여도 자원 부족(under)"을 잡는다. Linux는 loadavg(run queue)·iowait·swap page-out으로 채우는데, Windows는 등가 신호를 안 보내 엔진이 saturation 축을 "미관측(unmeasured)"으로 두고 부분 평가만 한다.

원인: Windows 에이전트가 saturation 카운터를 안 읽어 안 보냄.

에이전트 변경: Windows PDH 카운터를 raw로 metrics에 추가:
- CPU: `System\Processor Queue Length` (프로세서 대기 스레드 — Linux run queue 대응)
- Disk: `PhysicalDisk\Avg. Disk Queue Length` (I/O 큐 — iowait 대응)
- Memory: `Memory\Pages/sec` (페이징 — swap 활동 대응)
```
"saturation": {"cpu_run_queue": 3, "disk_queue": 0, "mem_paging_rate": 12}
```
raw 값만 실어라 — "병목이냐" 판정은 엔진이 os_family로 분기해 OS별 임계로 한다(P1 raw-first).

엔진이 얻는 것: 값 있으면 saturation 축 채워져 `unmeasured` 해제 -> Windows 완전 평가 + under 탐지 강화.

주의: Windows Processor Queue Length(순간값, I/O 대기 미포함)와 Linux loadavg(감쇠 평균, uninterruptible 포함)는 의미가 완전히 같진 않다 -> 정규화하지 말고 raw로.

---

## 6. Windows 메트릭 canonical 매핑 — MEDIUM

엔진 현상: 저장 전 canonical 불변식을 방어적으로 클램프 — `mem_available_kb <= mem_total_kb`, `swap_free_kb <= swap_total_kb`. 위반 시 ceiling 클램프 + warning(used 음수·왜곡 % 방지).

원인: Windows pagefile->swap·메모리 매핑이 canonical Linux /proc 단위와 어긋날 때가 있어 방어 필요.

에이전트 변경: Windows 카운터를 canonical 불변식에 맞게 소스에서 매핑(available <= total, pagefile를 swap 의미로 일관). 형식 변경 아님 — 값 정합. 엔진 클램프는 defense로 유지하되 정상 에이전트면 발동 0.

참고: 어떤 Windows 카운터를 어떤 canonical 필드에 매핑하는지 에이전트 문서가 있으면 엔진과 대조하기 좋다.

---

## 7. boot_time 정적 소스 — MEDIUM

엔진 현상: 에이전트가 boot_time을 `/proc/stat btime`(정적) 대신 `now - uptime`(동적)으로 산출해 매 수집 ±1초 흔들린다고 엔진 주석에 기록됨. 엔진이 5초 tolerance로 "동일 부팅인가"를 판정(정확 비교하면 지터를 재부팅으로 오판 -> CPU/IO delta 간헐 NULL + 가짜 history 행).

에이전트 변경: `/proc/stat btime`(Linux) 정적값, Windows도 LastBootUpTime 정적값 발행. 형식 동일, 값 안정화만.

주의: 현행 에이전트가 이미 정적(btime)인지 먼저 확인 필요 — 엔진 주석이 stale일 수 있다. 이미 정적이면 확인만 하고 넘어감.

---

## 8. task.result os 필드 보강 — LOW

엔진 현상: task.result에 os_id/os_family가 없어(Linux worker는 os 미발행) 엔진이 exit code 성공 판정 정책 키를 만들 때 inventory에서 조회한다. (Windows worker는 os_version=CurrentBuildNumber만)

배경: ZConverter installer가 실제론 성공인데 non-zero로 끝나는 케이스(Windows exit 2, EL9 systemd start-limit exit 3)를 엔진이 os별 allowlist로 success 보정. 그 키에 os 정보 필요.

에이전트 변경: task.result에 `os_family` + `os_id` + `os_version` 추가.
```
"os_family": "linux", "os_id": "rocky", "os_version": "9.3"
```
엔진이 얻는 것: inventory 조회 제거. (exit code false-failure 근본은 installer 유닛 수정 — installer/ZDM 쪽 별개)

---

## 부록

- os_family: task.result 등 일부 메시지에서 nullable이라 엔진이 None을 Linux로 fallback. 모든 메시지에 일관 발행하면 fallback 제거.
- listen_ports.uid: Windows는 POSIX uid 없어 null — 정당한 플랫폼 차이, 변경 불요.
- 필드명·구조는 협의 대상 — 에이전트 구현상 더 자연스러운 형태가 있으면 역제안 환영.
- 검토 회신에 "각 항목 반영 가부 + 채택 필드명/구조 + 제약"을 담아주면 엔진 쪽 스키마·mapper·필터 제거를 짝으로 정리한다.
