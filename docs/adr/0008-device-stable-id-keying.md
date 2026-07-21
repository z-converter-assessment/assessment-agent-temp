# ADR-0008: device 시계열 키를 안정 id로

상태: 채택

## 맥락

디스크·네트워크 지표는 장치마다 시계열이 갈린다. 이 시계열을 묶는 키로 커널이 붙인 이름(`sda`, `dm-0`, `C:`, perflib `0 C:`)을 쓰면, 재부팅·디스크 추가·재검출로 이름이 바뀔 때 엔진이 다른 장치로 인식해 14일 창 누적이 끊긴다.

## 결정

시계열 자연키는 이름이 아니라 바뀌지 않는 안정 id다. 계층 폴백으로 고른다.

- 디스크: Linux dm/uuid -> serial -> by-path -> name, 파티션 partuuid -> name. Windows gptid -> mbrsig -> serial -> name, 볼륨 volguid.
- 네트워크: MAC. 폴백 Windows ifguid / Linux by-path -> name.

이름은 표시용으로만 남긴다. metric의 device attr는 `<scheme>:<value>` 결합형(`serial:...`, `mac:...`, 시스템 전역 `aggregate:system`), 인벤토리 토폴로지(block_devices/net_interfaces)는 id/id_type 분리형으로 같은 안정키를 노출한다. parent도 이름이 아니라 부모의 id 값으로 링크한다.

## 결과

- 재부팅·재검출에도 엔진 시계열·토폴로지가 안정된다.
- id_type enum에 wwid/by-id/fsuuid도 있으나 현재 producer 미발행(방어적 완전성). name은 최후 폴백.
- 부모가 여럿(RAID 멤버 N개, LVM PV N개)이면 자식 노드를 부모 수만큼 반복 발행해 다대다를 표현한다.

관련: [storage-layout.md](../storage-layout.md), [payload-contract.md](../payload-contract.md) device 안정키.
