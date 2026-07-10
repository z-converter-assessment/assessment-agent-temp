# 스토리지 레이아웃 파싱 (block_devices)

에이전트가 서버당 스토리지 레이아웃을 얻는 방식. inventory 메시지의 `block_devices` 배열로 발행한다.

핵심 원칙: 외부 명령(`lsblk`/`df`/`wmic` 등)을 shell out 하지 않는다. C 바이너리가 커널 인터페이스(sysfs/procfs 파일, DeviceIoControl IOCTL, Native API)를 직접 읽는다. 근거: musl static(리눅스) / 단일 i686(윈도우)로 외부 유틸 유무·PATH·로케일에 독립적이어야 하고, 파싱 대상이 텍스트 출력이 아니라 커널 원천이라 안정적이다.

구현: `src/collect_inventory.c`(Linux) / `windows-agent/src/collect_inventory.c`(Windows), 안정키 헬퍼는 `src/collect_util.c`.

---

## 1. 공통 데이터 형식 (wire)

각 노드는 아래 필드를 가진다(`schema/wire.schema.json`의 `$defs/block_device`). required: name, type, id, id_type, parent.

| 필드 | 타입 | 의미 |
| --- | --- | --- |
| name | string | 표시용 이름(불안정, 예 sda1 / dm-0 / C: / PhysicalDrive0). 시계열 키로 쓰지 않는다 |
| type | string | disk / part / lvm / crypt / raid / mpath / dm / volume / swap |
| size_bytes | int \| null | base 단위 bytes. 측정 불가면 null |
| fstype | string \| null | ext4 / xfs / ntfs / swap 등. 미마운트/미포맷이면 null |
| mountpoint | string \| null | 마운트 경로. swap 은 `[SWAP]`(Linux) / pagefile 경로(Windows) |
| parent | string \| null | 부모의 id 값(불안정 name/dm-N 아님). root=null. 부모 복수면 노드를 반복 발행 |
| id | string \| null | 시계열 안정 자연키(아래 id_type 스킴) |
| id_type | enum \| null | dm / partuuid / wwid / serial / by-id / by-path / fsuuid / gptid / mbrsig / volguid / mac / ifguid / name / null |

설계 요점: 시계열 자연키는 이름(sda/dm-0)이 아니라 안정 id다. 재부팅/재검출로 커널 이름이 바뀌어도 id 는 불변이라 엔진이 같은 장치로 인식한다. parent 도 이름이 아니라 부모의 id 값을 담아 토폴로지를 안정 id 그래프로 잇는다. 부모가 여럿(예 RAID 멤버 N개, LVM PV N개)이면 자식 노드를 부모 수만큼 반복 발행해 다대다 관계를 표현한다.

---

## 2. Linux 파싱 소스

함수 `inv_collect_block_devices()`.

### 계층별 원천

| 계층 | 원천(파일/디렉토리) | 판정/추출 |
| --- | --- | --- |
| 장치 열거 | `/sys/block/` (opendir) | whole-disk + dm-* + md* 노드 열거 |
| 크기 | `/sys/block/<dev>/size` | 512-byte 섹터 수 x 512 = bytes |
| dm 종류 | `/sys/block/<dev>/dm/uuid` | 접두 `LVM-`->lvm, `CRYPT-`->crypt, `mpath-`->mpath, 그 외 dm |
| RAID | 이름이 `md`로 시작 | type=raid |
| 부모(dm/md) | `/sys/block/<dev>/slaves/` | 각 슬레이브(PV/RAID멤버)를 resolve_block_id 로 id 화 -> parent |
| 파티션 | `/sys/block/<disk>/<disk><N>/partition` 존재 | 파티션 노드. size 는 `.../size` |
| 마운트 | `/proc/mounts` | device basename 매칭. `/dev/mapper` 심볼릭은 realpath 로 dm-N 해석 |
| swap | `/proc/swaps` | 파티션/파일 스왑. size(KB)x1024. mountpoint=`[SWAP]` |

### 안정 id 해석 (`collect_util.c`)

디스크 `disk_device_id()` — 우선순위:
1. `/sys/block/<dev>/dm/uuid` -> `dm:<uuid>` (dm 계열 최우선)
2. `/sys/block/<dev>/serial` 또는 `/sys/block/<dev>/device/serial` -> `serial:<값>`
3. `/dev/disk/by-path/` 심볼릭 역참조 매칭 -> `by-path:<링크명>`
4. 폴백 `name:<dev>`

파티션 `part_device_id()`:
1. `/dev/disk/by-partuuid/` 역참조 매칭 -> `partuuid:<uuid>`
2. 폴백 `name:<part>`

(id_type enum 에 wwid/by-id/fsuuid 도 있으나 현재 producer 미발행 — 표시용/미래 확장.)

### 다층 토폴로지 예시 (representative: disk -> part -> raid -> lvm -> crypt -> ext4)

물리 디스크 2개 -> 각 파티션 -> md RAID -> LVM LV -> LUKS crypt -> ext4 마운트. parent 가 자식->부모 id 로 이어지고, RAID/LVM 은 멤버 수만큼 노드 반복.

```json
[
  { "name":"sda", "type":"disk", "size_bytes":42949672960, "fstype":null, "mountpoint":null,
    "parent":null, "id":"by-path:pci-0000:00:05.0-scsi-0:0:0:0", "id_type":"by-path" },
  { "name":"sda1", "type":"part", "size_bytes":42948624384, "fstype":null, "mountpoint":null,
    "parent":"by-path:pci-0000:00:05.0-scsi-0:0:0:0", "id":"partuuid:8b1c...-01", "id_type":"partuuid" },
  { "name":"sdb1", "type":"part", "size_bytes":42948624384, "fstype":null, "mountpoint":null,
    "parent":"by-path:pci-0000:00:06.0-scsi-0:0:0:0", "id":"partuuid:9a2d...-01", "id_type":"partuuid" },

  { "name":"md0", "type":"raid", "size_bytes":42815455232, "fstype":null, "mountpoint":null,
    "parent":"partuuid:8b1c...-01", "id":"name:md0", "id_type":"name" },
  { "name":"md0", "type":"raid", "size_bytes":42815455232, "fstype":null, "mountpoint":null,
    "parent":"partuuid:9a2d...-01", "id":"name:md0", "id_type":"name" },

  { "name":"dm-0", "type":"lvm", "size_bytes":21474836480, "fstype":null, "mountpoint":null,
    "parent":"name:md0", "id":"dm:LVM-abcd1234...", "id_type":"dm" },

  { "name":"dm-1", "type":"crypt", "size_bytes":21458059264, "fstype":"ext4", "mountpoint":"/data",
    "parent":"dm:LVM-abcd1234...", "id":"dm:CRYPT-LUKS2-ef56...", "id_type":"dm" },

  { "name":"sda2", "type":"swap", "size_bytes":2147483648, "fstype":"swap", "mountpoint":"[SWAP]",
    "parent":null, "id":"partuuid:8b1c...-02", "id_type":"partuuid" }
]
```

RAID md0 가 2번 나오는 것(parent=sda1, parent=sdb1)이 멤버 반복 발행이다. 엔진은 같은 id(name:md0)로 하나의 RAID 노드에 부모 2개를 잇는다.

---

## 3. Windows 파싱 소스

함수 `inv_collect_block_devices()`. 관측 전용(레지스트리·시스템 상태 미변경).

### 계층별 원천

| 계층 | 원천(API/IOCTL) | 판정/추출 |
| --- | --- | --- |
| 물리 디스크 열거 | `\\.\PhysicalDrive0..31` CreateFile | 열리는 인덱스만 |
| 디스크 크기 | `DeviceIoControl(IOCTL_DISK_GET_LENGTH_INFO)` | GET_LENGTH_INFORMATION.Length |
| 디스크 안정 id | `win_disk_id()` (IOCTL_DISK_GET_DRIVE_LAYOUT_EX 등) | gptid -> mbrsig -> serial -> name |
| 볼륨 열거 | `FindFirstVolumeW/FindNextVolumeW` | `\\?\Volume{GUID}\` |
| 볼륨 GUID | 볼륨명에서 `{...}` 추출 | `volguid:{GUID}` |
| 파일시스템 | `GetVolumeInformationW` | ntfs/fat 등(소문자화) |
| 마운트 | `GetVolumePathNamesForVolumeNameW` | C: 등. 없으면 볼륨GUID명 |
| 볼륨 크기 | `GetDiskFreeSpaceExW` | TotalNumberOfBytes |
| 볼륨->디스크(parent) | `DeviceIoControl(IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS)` | Extents[].DiskNumber -> 해당 디스크 id. extent 복수면 노드 반복(스팬 볼륨) |
| pagefile(swap) | `NtQuerySystemInformation(SystemPageFileInformation, class 18)` | TotalSize(pages) x page_size. id/id_type=null(볼륨 아닌 파일) |

### 안정 id 해석

디스크 `win_disk_id()` 우선순위: `gptid:<GPT DiskId>` -> `mbrsig:<MBR signature>` -> `serial:<제품 시리얼>` -> `name:<PhysicalDriveN>`.
볼륨: `volguid:{GUID}` (폴백 name).
pagefile: id/id_type = null (안정 볼륨 GUID 가 아니라 파일이라).

세대 무관(NT5.2~NT10 동일 경로). NT6+ 전용 API 하드임포트 없이 이 IOCTL/Native API 로만 뽑는다.

### 실측 예시 — win2003 (NT5.2, MBR 단일 디스크)

캡처된 실제 값(dp2003):

```json
[
  { "name":"PhysicalDrive0", "type":"disk", "size_bytes":42949672960, "fstype":null,
    "mountpoint":null, "parent":null, "id":"337777698", "id_type":"mbrsig" },
  { "name":"C:", "type":"volume", "size_bytes":42935926784, "fstype":"ntfs",
    "mountpoint":"C:", "parent":"337777698", "id":"{003cf6b1-7bad-11f1-a363-806e6f6e6963}", "id_type":"volguid" },
  { "name":"pagefile.sys", "type":"swap", "size_bytes":2145386496, "fstype":null,
    "mountpoint":"C:\\pagefile.sys", "parent":null, "id":null, "id_type":null }
]
```

volume C: 의 parent 가 디스크의 mbrsig(337777698)로 이어진다. 디스크 크기(42949672960 = 40GiB)는 IOCTL_DISK_GET_LENGTH_INFO 원값(WMI 실린더 반올림보다 정확).

### 실측 예시 — win2012R2 (NT6.3, 시스템예약 + C:)

캡처된 실제 값(WIN-777ISDMQSG4):

```json
[
  { "name":"PhysicalDrive0", "type":"disk", "size_bytes":42949672960, "fstype":null,
    "mountpoint":null, "parent":null, "id":"3784457715", "id_type":"mbrsig" },
  { "name":"\\\\?\\Volume{b112933a-...}\\", "type":"volume", "size_bytes":366997504, "fstype":"ntfs",
    "mountpoint":null, "parent":"3784457715", "id":"{b112933a-...}", "id_type":"volguid" },
  { "name":"C:", "type":"volume", "size_bytes":21105733632, "fstype":"ntfs",
    "mountpoint":"C:", "parent":"3784457715", "id":"{b112933b-...}", "id_type":"volguid" },
  { "name":"pagefile.sys", "type":"swap", "size_bytes":1476395008, "fstype":null,
    "mountpoint":"C:\\pagefile.sys", "parent":null, "id":null, "id_type":null }
]
```

시스템 예약 볼륨(마운트 없음 -> mountpoint=null, name=볼륨GUID)과 C: 둘 다 같은 물리 디스크(mbrsig 3784457715)를 parent 로 가리킨다.

---

## 4. 요약

- 원천: Linux = sysfs(`/sys/block/*`) + procfs(`/proc/mounts`,`/proc/swaps`) + `/dev/disk/by-*` 심볼릭. Windows = PhysicalDrive/Volume 핸들 + IOCTL(DISK_GET_LENGTH_INFO / VOLUME_GET_VOLUME_DISK_EXTENTS / DRIVE_LAYOUT_EX) + NtQuerySystemInformation(pagefile) + GetVolume* API.
- 형식: 평평한 노드 배열. 계층은 parent(부모 id) 링크로 표현. 부모 복수면 노드 반복 -> 다대다 그래프.
- 안정키: name 이 아니라 id(dm/partuuid/serial/by-path / gptid/mbrsig/volguid / MAC). 재부팅·재검출에도 불변이라 엔진 시계열/토폴로지가 안정.
- swap: Linux `/proc/swaps`, Windows pagefile(NtQuery). 둘 다 type=swap 노드로 필드셋 패리티.
- 값 의미론: 측정 불가 필드는 null(0 아님). 외부 명령 미사용, 커널 원천 직접 파싱.
