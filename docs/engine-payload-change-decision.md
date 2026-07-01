# 엔진 -> 에이전트 payload 변경 결정 (회신 반영)

회신(`assessment-engine/docs/agent-payload-change-response.md`) 잘 받았다. 정정·제약 다 반영했고, 택일 요청에 결정 회신한다. 이 문서로 1차 배치를 바로 착수해도 된다(엔진 짝 작업은 병렬로 진행).

## 결정 요약

| 항목 | 결정 |
|---|---|
| 1 kind 태그 | 진행. 필드명 `kind` 확정, taxonomy 채택. 단계적 pre-drop 완화 동의 |
| 2 services pid/exe | 진행 |
| 3 interfaces + ip_external | 진행. `ip_external`은 별도 필드 유지(합치지 않음) — 아래 |
| 4 agent_id | 진행(additive) + prep-image 짝 필수. 엔진 식별 마이그레이션은 엔진 ADR로 후속 |
| 5 saturation | 후순위 동의. `RegQueryValueEx(HKEY_PERFORMANCE_DATA)`로 DLL 없이 취득 검토 OK |
| 6 swap canonical | 진행. min-clamp + 가능하면 pagefile 기반 재정의. 매핑표 제공 요청 수용 |
| 7 boot_time | 종결. Linux 이미 정적(확인), Windows 현행 유지 |
| 8 task.result os | 진행. `os_version`은 inventory와 동일 소스(Windows=DisplayVersion) |

## 택일 결정

- 3 `ip_external`: 별도 필드 유지에 동의. 내부/외부는 소스·의미가 달라 분리가 맞다. 엔진 토폴로지는 internal만 쓰고 external은 표시 전용이라 합칠 이득이 없다. 결정: `interfaces[]`(internal) + `ip_external`(유지). ip_external도 여력 되면 `{address, family}` 구조화는 환영(급하진 않음, 문자열 유지도 수용).
- 7 Windows boot_time: 현행 유지로 종결. per-collection 지터 없음 + 엔진 5초 tolerance면 재시작 ±1초는 흡수된다. 정적 전환 불요.
- 부록 Windows `mac_addresses`: 지금은 불요. 4(agent_id) 진행 시 감사용으로 additive 맞춰도 좋으나 우선순위 낮다.

## 필드명·구조 확정

`kind` / `interfaces` / `agent_id` / `saturation` / task.result os 필드 — 회신대로 채택. 역제안 없음.

## install.args 계약 (회신 검증 반영)

에이전트 전달이 대칭(verbatim argv)임을 확인해줘서 고맙다. 계약 확정:
- MSI는 args 드롭 — 현재 MSI 미사용이라 문제없음. 향후 MSI 도입 시 PROPERTY 방식 필요(그때 계약 개정).
- install.args 값은 argv-simple 유지(공백·따옴표·백슬래시 말미 회피).
- "ZDM Windows .exe가 -s/-u를 install.sh와 동일 의미로 파싱하느냐"는 ZDM installer 계약이라 에이전트 밖이다. 엔진/운영이 실 ZDM Windows installer로 별도 검증한다. 에이전트는 argv 대칭 전달만 보장(확정) — 이 문구를 계약으로 채택.
- install.type OS 분기(Linux=shell / Windows=direct_exec)는 현행 유지가 맞다(회신대로).

## 엔진 측 정정·짝 작업 (병렬)

에이전트가 1차 배치를 구현하는 동안 엔진은:
- 회신에서 드러난 엔진 stale 정정: `boot_time.py` 주석의 "에이전트가 now-uptime 산출"은 Linux 오해 -> 정정. composite_id를 에이전트가 계산한다는 사실도 문서 반영.
- 각 채택 필드에 "신형 있으면 신형, 없으면 구형" 이중 읽기 추가(additive-safe) — 에이전트 배포 전에도 안 깨짐.
- `device_filters` 정규식은 `kind` 도착 후 태그 기반 필터로 교체(major bump 시 정규식 제거). swap clamp는 defense로 유지.

## 착수 순서 (동의)

- 1차: 1 kind + 2 pid + 3 interfaces + 8 task.result os + 6 swap 정합, `agent_version` minor bump.
- 2차: 4 agent_id + prep-image 짝(선행 조건).
- 3차: 5 saturation. (7은 종결이라 제외.)

에이전트는 1차 배치를 지금 시작해도 된다. 구현 중 형식 세부에서 조정 필요하면 회신 문서에 추가로 남겨주면 엔진 짝을 맞춘다.

---

## 정정: 전 에이전트 교체 + DB 초기화 -> clean breaking cutover

운영 확정: 인프라 전 VM(50대+)의 에이전트를 일괄 교체하고 엔진 DB를 초기화한다. 에이전트 교체는 어차피 식별자가 바뀌어 엔진이 새 서버로 인식하므로 기존 데이터를 보존할 이유가 없다 -> 혼합 fleet·과도기가 없다.

따라서 additive/무중단 원칙을 이번 변경엔 적용하지 않는다. 가장 깔끔한 breaking cutover로 간다:

- 구형 필드 병행(dual-field) 불요. `ip_internal` 문자열 배열은 남기지 말고 `interfaces[]`로 교체(구형 미발행 OK).
- `kind`/`pid`/`exe`/task.result os 필드는 새 형식으로만 발행(구형 fallback 대비 불요).
- `agent_version`은 minor가 아니라 major bump. 이게 엔진-에이전트 동시 breaking cutover 신호다.
- 엔진도 backward-compat(정규식 device 필터·ip_internal CIDR 파싱·task.result os inventory 조회 fallback·additive 이중 읽기)을 만들지 않고 새 계약만 구현한다. DB가 비어 마이그레이션도 데이터 백필 없는 clean DDL.

즉 1차 배치를 "additive"가 아니라 "구형 제거 + 신형 단독"으로 단순화해도 된다 — 에이전트 구현도 그만큼 가벼워진다(dual-field/구형 유지 코드 불요).

(선택) item 4 `agent_id`도 이번 cutover에 접을 수 있다: DB 초기화라 기존 행 relink 마이그레이션이 불요해져 "안정 UUID 식별 전환"이 clean해진다(엔진 ADR 0044 relink 로직도 제거 가능, MQ 라우팅 키 `agent.tasks.{...}`를 agent_id로). prep-image 짝만 되면 batch 2를 batch 1과 합쳐도 된다 — 판단 요망.
