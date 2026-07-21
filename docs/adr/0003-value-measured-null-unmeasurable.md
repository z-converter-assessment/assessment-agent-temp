# ADR-0003: value=실측, null=측정불가

상태: 채택

## 맥락

사이징 정확성은 "0(측정된 없음)"과 "측정 불가(개념 부재/커널 하한 미달)"를 구분하는 데 달렸다. 못 뽑는 필드를 0이나 추정치로 채우면 엔진이 그걸 실측으로 오인해 분류가 오염된다.

## 결정

값(0 포함)은 실측이고, OS에 개념이 없거나 커널·세대가 못 뽑는 필드는 null로 둔다. 절대 가짜값으로 채우지 않는다. namespace 전체가 측정 불가면(예: Windows system.pressure) 그 네임스페이스 값이 null이다.

## 결과

- 구세대·구커널에서 null이 많이 남을 수 있으나(PSI 4.20+, MemAvailable 3.14+, oom_kill 4.13+, virtio-net link speed, EDAC VM 미등록, NTFS inode), 이는 수용한다.
- number/bool은 0도 실측이라 값에서 부재를 추론하지 않고 have 플래그로 판정한다(`wire_num_or_null`/`wire_bool_or_null`).
- Windows saturation의 perflib는 diskperf 의존이라 raw!=0 & 단조일 때만 값·아니면 null(가짜 0 금지).
- CI 스키마가 os_family 조건부 null을 강제한다(saturation=Windows null 등).

관련: [0002](0002-stateless-agent-raw-counters.md), [payload-contract.md](../payload-contract.md) 값 의미론.
