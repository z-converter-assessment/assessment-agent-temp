# Architecture Decision Records

이 프로젝트의 load-bearing 설계 결정을 원자적 결정 기록으로 남긴다. [architecture.md](../architecture.md)가 시스템을 설명한다면, ADR은 개별 결정을 "맥락 -> 결정 -> 결과(+대안)"로 참조 가능하게 고정한다.

작성 규약:
- 현재형 결정 기록이다. 과거·변화 이력("이전엔 X였다")을 쓰지 않는다 — 지금 서 있는 결정과 그 근거·트레이드오프만 적는다.
- 상태는 채택(Accepted)만 쓴다. 결정이 뒤집히면 해당 ADR을 폐기(Superseded) 표기하고 새 번호로 대체한다.
- 번호는 4자리 증가. 파일명 `NNNN-kebab-title.md`.

## 목록

| ADR | 결정 |
| --- | --- |
| [0001](0001-two-tree-duplication.md) | OS별 2트리를 크로스플랫폼 추상화 대신 대칭 중복으로 유지 |
| [0002](0002-stateless-agent-raw-counters.md) | 에이전트는 raw 누적 카운터만 발행, rate/delta는 엔진 |
| [0003](0003-value-measured-null-unmeasurable.md) | value=실측(0 포함), null=측정불가. 가짜값 금지 |
| [0004](0004-single-binary-per-os.md) | OS 계열당 단일 바이너리(Linux musl static / Windows 단일 i686 + 런타임 세대분기) |
| [0005](0005-nt52-load-guard.md) | NT6+ 전용 API 하드임포트 금지(NT5.2 로드 가드) |
| [0006](0006-ephemeral-publish-connections.md) | 발행마다 단명 연결, 워커는 basic.get 폴링 |
| [0007](0007-root-localsystem-privilege.md) | install·런타임을 root/LocalSystem으로 통일 |
| [0008](0008-device-stable-id-keying.md) | device 시계열 키를 이름이 아닌 안정 id(id_type)로 |
