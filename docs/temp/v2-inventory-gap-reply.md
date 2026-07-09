# wire v2 inventory 갭 — 회신 (에이전트 -> 엔진)

> `v2-inventory-gap.md`(엔진 락 보완 요청)에 대한 회신. 오버사이트 인정 — inventory 재설계 시 호스트 서술자·IP·서비스 카탈로그를 드롭했다. 전부 복원. 갱신 `wire.schema.v2.json` + `v2-example-messages.json`(6종 검증 통과).

## 0. 결론

엔진 지적 전면 수용. inventory 를 v1 완결성으로 복원(신호/디바이스 모델은 락 유효). D4(prefix 카탈로그) 해소. 나머지 D-items 확인.

## 1. inventory 복원 (1.1~1.4 반영)

1.1 호스트 식별·OS (top-level 정적, task.result 와 동형): `hostname`, `os_id`, `os_version`, `os_codename`, `kernel_version`, `cpu_model` 추가. inventory required 로 강제.

1.2 호스트 스펙:
- `cpu_cores`(int) inventory top-level 추가. 추가로 metrics `system.cpu`에 `cpu.logical.count`(gauge, unit cpu) 발행(D7).
- `mem_total_bytes`(By, base 단위) inventory top-level 추가(metrics memory.limit 와 동치이나 inventory 시점 저장용).
- swap: `block_device` type enum 에 `swap` 명시(스왑 파티션/pagefile 노드, size_bytes=스왑 할당, mountpoint=[SWAP]/pagefile 경로). 별도 swap_total 필드 없음.

1.3 네트워크 주소: `net_interface`에 `addresses[]`{address, prefix, family} + `gateway` + `kind` 편입. `ip_external`(array|null) inventory top-level 복원. MAC 은 키(id)로 유지, 주소는 노드에.

1.4 서비스 카탈로그: `services[]`{unit, sub, pid, exe}(열거 불가면 null) + `listen_ports[]`{proto, addr, port, uid, pid, comm} 복원. v1 형태 유지(system.* 재설계 대상 아님).

inventory required = hostname/os_id/os_version/os_codename/kernel_version/cpu_model/cpu_cores/mem_total_bytes/ip_external/services/listen_ports/block_devices/net_interfaces.

## 2. D-items 확인 (엔진 section 3)

- D1 PSI = `pressure.stall.ratio`/`.time` + attr(resource/scope/window): 스키마·예시 그대로. 확인.
- D2 disk E축 = 단일 `disk.errors` + attr(kind/class[/member]): 예시 disk.errors 가 attr {kind: mdraid|btrfs|ext4, class: member_errors|degraded|corruption|errors_count} 형태. 확인.
- D3 block id_type vs net id_type 분리: block_device.id_type(dm/partuuid/serial/by-path/gptid/mbrsig/volguid..) 와 net_interface.id_type(mac/ifguid/by-path/name) 별도 enum. 확인.
- D4 metric device prefix 카탈로그: system.disk $comment 에 명시 — device attr = `<scheme>:<value>`, scheme ∈ {gptid,mbrsig,serial,wwid,by-path,dm,partuuid} + E축은 {md,btrfsuuid,fsuuid}(RAID 배열/fs 레벨 참조). 해소.
- D5 metric device non-null vs block_device.id best-effort: metric device attr 은 안정 id non-null(디스크는 by-path 폴백으로 보장), block_device.id 는 best-effort(nullable). 구분 확인.
- D6 디스크 폴백 체인 = 스키마: dm/uuid->partuuid->wwid->serial->by-id->by-path->name. 확인.
- D10 Windows conntrack metric 미발행: windows metrics network 에 conntrack 없음(개념 부재). 확인.

## 3. 상태

- 6종 예시(linux/windows metrics·inventory + task.result + error) 전부 갱신 스키마 검증 통과.
- inventory 완결 -> 4종 메시지 전부 완결. 엔진 v2 계약 문서(agent-data.md) 완성 가능.
- 에이전트 구현(P4 inventory)이 이 복원 필드를 포함하도록 플랜에 반영.
