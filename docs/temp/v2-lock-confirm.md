# wire v2 계약 — 엔진 락 확정 (엔진 -> 에이전트)

> 성격: 협의 회신. `v2-lock-reply.md`(에이전트 락 조건 해소) + `PENDING-classification-rationale.md`(신호 근거·예상 소비)에 대한 엔진 확정.
> 검증: 갱신 `wire.schema.v2.json`(task.result/error body·network MAC·disk by-path 반영)·`v2-example-messages.json`(6종) 확인. 우리 repo docs/temp 동기화 완료.

## 0. 결론 — 엔진 측 락 확정

락 조건 2건 해소 확인, 네트워크 MAC 통일 수용, 분류 근거 예상 소비 전면 정합. 계약 락. 미결 없음.

## 1. 락 조건 해소 확인

1.1 task.result / error body — 확인. v2 envelope 아래 v1 body 유지 + schema_version + task_policy(bool|null, exit_code 우선) 스키마 명시됨. exit_code/signal_no 상호배타·Windows signal_no null·boot_time/agent_started_at nullable(워커 컨텍스트)·task_id 매칭 전부 엔진 task lifecycle 과 정합. error 의 failed_component(자유 문자열) 포함. 예시 2종 검증 통과.

1.2 device 자연키 — 확인 + 네트워크 MAC 통일 수용.
- 디스크: by-path(PCI) 폴백으로 metric-bearing 디스크 id non-null 보장 확인(serial 없는 virtio-blk 도 by-path 존재).
- 네트워크: name -> MAC(id_type mac, 폴백 ifguid/by-path) 통일 수용. 엔진은 원래 네트워크 키를 인터페이스 name 으로 두려 했으나 Windows name 불안정(실측)이라 MAC 이 더 낫다 — 전 device(디스크/네트워크)가 안정 id 한 패턴이면 시계열 자연키(server_id, device=id, collected_at)가 균일해진다. 엔진 implication 수용: 네트워크 시계열 device 축 = MAC. name 은 inventory `net_interfaces[].name` 표시용.

## 2. 분류 근거 정합 (PENDING-classification-rationale 회신)

에이전트 예상 소비와 엔진 실제 분류(ADR 0052 rollup_host)를 축별로 대조 — 전면 일치. 6절 요청대로 명시 합의:

- PSI 14일 소비 (명시 합의 요청 항목): saturation 14일 canonical = `pressure.stall.time`(counter) Δstall.time/Δwall 시간가중 평균. `pressure.stall.ratio`(avg10)는 실시간 참고용, 14일 창엔 미사용. 에이전트 적분 논거(점표본 손실·평활 편향 회피) 수용 — 확정.
- E축 -> confidence + attention, 사이징 미반영 (명시 합의): disk fault(mdraid/btrfs/ext4/ioerr)·hardware_corrupted -> 해당 자원 U/S 신뢰도 하향(엔진 steal_biased 와 동형 오염 게이트). tcp.retransmits -> net 품질, conntrack -> net 포화. 사이징 숫자(목표 사양)엔 E축 미반영 — 확정.
- 메모리 U 분모 = available (1 - available/limit), used% 아님. 엔진은 현행 used% -> available 로 전환(v2 개선). 미발행 구커널(centos6 2.6.32)만 MemFree+Buffers+Cached 폴백 — 수용.
- 메모리 S = PSI-first, 없으면 page-out(swap 有)/commit ratio(swapless·구커널) 폴백. swapless 사각 근본 해소 — 확정.
- cpu.blocked(D-state) -> disk->cpu 인과 게이트: 엔진 rollup_host 의 인과 근본원인(메모리->디스크I/O->CPU, procs_blocked 로 disk_io 발 CPU 로드 억제)과 정확히 일치. PSI io 없는 커널/Windows 폴백 신호 — 확정.
- cpu.run_queue(procs_running) -> CPU 포화(run_queue/cores), loadavg 대체 — 확정(엔진 이미 procs_running 기반).
- disk %util(disk.io_time): 엔진 현행 disk_io 는 saturation(await/queue) 단독 — v2 %util 로 U축 신설(over/idle 판정 가능). 에이전트 예상(U=busy 분율, await/queue 는 S 별개)과 일치, 엔진 신규 축으로 배선.
- 스토리지 3계층(block_devices parent-by-id + lvm_vgs free_bytes): 배정/파일시스템/확장여력(VG free) 실측 3계층. major/minor 조인 추론 폐기 — 확정(프로비저닝 계획 티어3 해금).

예상과 실제 divergence 0 — 재조정 필요 신호 없음. E축을 사이징에 반영하지 않고, 14일 창에 stall.time(적분)을 쓰는 두 핵심 합의 명시.

## 3. 락 이후 흐름 (합의대로)

- 에이전트: 양 트리 v2 수집 구현 + `schema/wire.schema.json` 정본 교체 + check-contract.sh v2 + Windows 디스크 perflib 전환(IOCTL 성능경로 revert) + testbed 매트릭스 재검증.
- 엔진: v2 마이그레이션(티어) 착수 — ingest DTO/파싱(datapoint-array) -> DB 스키마·단위·device MAC 키 마이그레이션 -> recommendation PSI-first 배선 + 신규 축(disk %util·commit·PSI) -> 신규 신호 표시(#E9). dual-read 불요(pre-prod, flag-day cutover). GA 시점 schema_version 기반 dual-read 재검토.
- 계약 문서: 락 확정 후 `agent-data.md`(엔진 측 계약) v2 갱신은 엔진 마이그레이션 착수 시점에.
