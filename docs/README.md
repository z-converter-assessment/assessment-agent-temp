# 문서 인덱스

assessment-agent 문서를 역할별로 정리한 진입점이다. 각 문서는 독립적으로 읽을 수 있고, 아래는 "무엇을 찾을 때 어디를 보는가"의 지도다.

문서 규약: 현황 선언형(과거·변화 이력 서술을 넣지 않는다). 결정과 제약은 이 트리 또는 [.claude/CLAUDE.md](../.claude/CLAUDE.md)에 누적 기록한다.

## 시작점

| 문서 | 무엇 |
| --- | --- |
| [../README.md](../README.md) | 프로젝트 개요, 릴리즈 산출물, 설치·운영(OS별) |
| [architecture.md](architecture.md) | 아키텍처 개요 — 큰 그림부터 OS별 전략 차이까지, 이 프로젝트를 처음 이해할 때 |

## 설명 (왜 이렇게 설계했나)

| 문서 | 무엇 |
| --- | --- |
| [architecture.md](architecture.md) | 세 아키텍처 원칙(단일 바이너리 커버 / stateless 에이전트 / value=실측·null=측정불가), OS별 커버 전략, 발행·권한 모델 |
| [classification-rationale.md](classification-rationale.md) | 자원 적정성 분류의 신호별 근거와 예상 소비 맥락(엔진과의 협의 입력) |
| [adr/](adr/) | 결정 기록(Architecture Decision Records) — load-bearing 설계 결정을 결정형으로 |
| [study/portability-concepts.md](study/portability-concepts.md) | 공부용 — ISA/ABI/libc/커널 이식성 개념(프로젝트 무관 일반론) |

## 레퍼런스 (정확한 필드·계약)

| 문서 | 무엇 |
| --- | --- |
| [../schema/wire.schema.json](../schema/wire.schema.json) | wire 계약 기계검증 정본(JSON Schema, schema_version 1.0) |
| [../schema/metric-vocab.json](../schema/metric-vocab.json) | system.* metric 이름·attr 키 어휘 정본(CI 화이트리스트) |
| [payload-contract.md](payload-contract.md) | wire 계약 산문 설명 — 메시지 4종, 인코딩, 필드셋, USE 매핑 |
| [storage-layout.md](storage-layout.md) | block_devices 스토리지 레이아웃 파싱(Linux sysfs / Windows IOCTL) |
| [wire-examples.json](wire-examples.json) | 대표 페이로드 예시 |

## 방법 (빌드·릴리즈)

| 문서 | 무엇 |
| --- | --- |
| [BUILD.md](BUILD.md) | 소스 빌드(2종), CI 태그 릴리즈, 계약 conformance, 저장소 트리 |
| [../deploy/SUPPORTED_OS.md](../deploy/SUPPORTED_OS.md) | 지원 OS 매트릭스(install OS 게이트 정본) |

## repo 간 문서 교환

| 문서 | 무엇 |
| --- | --- |
| [temp/README.md](temp/README.md) | 에이전트 repo <-> 엔진 repo 간 inbox 메커니즘. 임시 파일은 흡수 후 삭제 |

`docs/temp/`는 상대 repo가 나에게 쓰는 일회성 메시지함이다. 지속 가치가 있는 결정은 위 영구 문서(또는 [adr/](adr/))로 격상하고, 임시 파일은 지운다. 이 인덱스와 [.claude/CLAUDE.md](../.claude/CLAUDE.md) 인덱스에는 등록하지 않는다.
