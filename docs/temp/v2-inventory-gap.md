# wire v2 — inventory 메시지 누락 필드 (엔진 -> 에이전트, 락 보완 요청)

> 성격: 협의 회신. 계약 락 후 엔진이 v2 계약 문서(agent-data.md)를 재작성하다 발견한 inventory 완결성 갭.
> 신호/디바이스 모델은 락 유효. inventory 메시지가 스토리지·디바이스 토폴로지만 담고, 엔진이 호스트를
> 식별·분류·표시하는 데 필요한 정적 서술자·주소·서비스 카탈로그를 전부 드롭했다. 이건 재협상이 아니라
> 빠진 필드 보완이다(task.result 는 os_id/os_version/os_codename 을 이미 실으니 오버사이트로 보인다).

## 0. 요약

`inventory` 메시지(schema + 예시 linux/windows)가 현재 담는 것: envelope + `system.*` 없음 + `block_devices[]` + `net_interfaces[]` + `lvm_vgs[]`.

빠진 것: 엔진 `server_inventory` 가 v1 inventory 로 채우는 12개 필드 전부 + 인터페이스 IP 주소. 이게 없으면 호스트 표시·OS 분류·EOL 경보·서비스 뱃지·네트워크 토폴로지·IP 필터가 전부 깨진다. 계약이 v1 inventory 를 대체하지 못한다.

## 1. 누락 필드 — 왜 필요한가 + 제안 위치

### 1.1 호스트 식별·OS (host-only 문자열, 파생 불가 — 필수)

| 필드 | 엔진 용도 | 파생 가능? |
|---|---|---|
| hostname | 전 화면 호스트 표시·식별(목록·상세·보고서·토폴로지) | 불가(host-only) |
| os_id | OS 분류(예 almalinux/windows), 서버 목록·상세·리포트, 서비스 분류 OS 분기 | 불가 |
| os_version | OS EOL 판정(attention os_eol_warnings), OS 버전 분포 카드, right-sizing OS 분기 | 불가 |
| os_codename | OS 표시 라벨 | 불가 |
| kernel_version | Windows 빌드 표시·EOL, Linux 커널 표시 | 불가 |
| cpu_model | CPU 모델 표시(서버 상세) | 불가 |

task.result 는 이미 os_id/os_version/os_codename 을 top-level 로 싣는다 — inventory 에도 동일 top-level 로 실으면 일관. hostname·kernel_version·cpu_model 도 함께.
제안: OTel resource 관례(`host.name`·`os.type`·`os.version`·`os.build_id`·`host.arch`)를 따르거나, task.result 와 동형으로 top-level 정적 필드. 어느 쪽이든 스키마 inventory 분기에 명시 required.

### 1.2 호스트 스펙 (일부 파생 가능하나 확정 필요)

| 필드 | 엔진 용도 | v2 소스 후보 |
|---|---|---|
| cpu_cores | CPU 포화(run_queue/cores)·사이징 목표·per-core | `cpu.time` 의 distinct `attr.cpu` count 로 파생 가능. 단 명시 `cpu.logical.count`(gauge, unit cpu) 가 깔끔 — 통합모델에 있으나 스키마/예시 누락(D7). 추가 요청 |
| mem_total_kb | 메모리 사이징 목표·표시 | `memory.limit`(metrics)로 얻으나 inventory 시점 저장엔 metrics 대기. inventory 에도 실을지 확정 |
| swap_total_kb | 스왑 할당(프로비저닝 스펙) | `block_devices` type=swap 노드(size_bytes)로. block_device type enum 에 swap 명시 요청(현재 disk/part/lvm/crypt/raid/mpath/dynamic/volume) |

### 1.3 네트워크 주소 (net_interfaces 가 MAC/name/speed 만 — IP 드롭)

v1 `interfaces[]` = {name, address, prefix, family, kind, gateway}. v2 `net_interfaces[]` = {name, id(MAC), id_type, speed_mbps} — 주소 전부 드롭.

| 필요 | 엔진 용도 |
|---|---|
| 인터페이스별 IP(address, prefix, family) | 서버 IP 표시, 네트워크 토폴로지(L3 서브넷 공동소속 추론 build_network_topology), right-sizing API `ip` 필터(_disc_match 가 interfaces address 매칭) |
| gateway | 토폴로지·게이트웨이 표시 |
| ip_external | 외부 IP 표시(v1 별도 필드) |

제안: `net_interfaces[]` 에 `addresses[]`{address, prefix, family} + `gateway` 편입, 또는 별도 주소 배열. MAC 은 키로 유지하되 주소를 노드에 실어야 IP 기능이 산다.

### 1.4 서비스 카탈로그 (services·listen_ports — 서비스 분류 뱃지 기능 전체)

| 필드 | 엔진 용도 |
|---|---|
| services[] | 서비스 카테고리 분류(service_classifier), 워크로드 뱃지·역할 추론·보고서 서비스 구성 |
| listen_ports[]{proto,addr,port,uid,pid,comm} | 포트 기반 서비스 분류(name->comm->port 우선순위), 서비스 3단계 표시 |

이 둘은 USE Method system.* 재설계 대상이 아니라 v1 형태 그대로 유지가 맞다(협의 확인됨). 단 v2 inventory 스키마/예시에 명시 편입돼야 한다 — 현재 둘 다 빠져 서비스 분류 기능 전체가 계약상 근거 없음.

## 2. 요청

inventory 메시지에 위 1.1~1.4 를 스키마·예시로 편입(또는 명시적으로 "엔진 불요" 판정 — 단 위 용도상 대부분 필수). 특히:
- 1.1 정적 서술자 6개 = 필수(파생 불가). task.result 와 동형 top-level 권장.
- 1.3 인터페이스 IP = 필수(토폴로지·표시·필터). net_interfaces 노드에 주소 편입.
- 1.4 services/listen_ports = 필수(서비스 분류). v1 형태 유지·명시.
- cpu.logical.count(D7)·block_device type=swap 명시.

이 보완 후 inventory 계약이 완결되면 엔진이 v2 계약 문서를 완성한다. metrics/task.result/error 3종은 완결 확인됨 — inventory 만 갭.

## 3. 참고 — 이번에 반영된 계약 문서 어긋남(D1-D11, 엔진 측 해소 방침)

계약 문서(agent-data.md) 재작성 시 엔진은 스키마+예시를 정본으로 아래를 해소한다(에이전트 확인만): PSI 는 `pressure.stall.ratio`/`.time` + attr.resource(D1), disk E축은 단일 `disk.errors` + attr.kind/class(D2), block id_type 과 net id_type 분리(D3), metric device attr non-null vs block_device.id best-effort 구분(D5), 디스크 폴백 체인 = 스키마(...serial->by-id->by-path->name)(D6), Windows conntrack 은 metric 자체 미발행(D10). D4(metric device prefix 어휘 md/btrfsuuid 가 id_type enum 초과)만 prefix 카탈로그 별도 명시 필요 — 확인 요청.
