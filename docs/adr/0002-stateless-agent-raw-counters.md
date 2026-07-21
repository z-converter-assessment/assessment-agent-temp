# ADR-0002: 에이전트는 raw 누적 카운터만 발행

상태: 채택

## 맥락

용량 계획(right-sizing)은 14일 같은 긴 창의 시간가중 평균을 요구한다. 에이전트가 rate/utilization을 계산하려면 직전 샘플 상태를 들어야 하고, 재시작·재부팅에 그 상태가 날아가면 정확도가 깨진다. 또 계산 로직이 에이전트 버전마다 다르면 fleet 전역에 드리프트가 생긴다.

## 결정

에이전트는 raw 누적 카운터(monotonic)와 순간 게이지만 싣는다. rate/delta/utilization/await는 전부 엔진이 두 샘플의 차이로 계산한다. metric `type`은 counter와 gauge 둘뿐이다. OS별 원시 단위(jiffies, sector, 100ns, %)는 에이전트가 base 단위(s/By/ratio 0..1)로만 정규화한다.

## 결과

- 에이전트가 stateless라 재시작·재부팅에 정확도 손실이 없다(직전 샘플 불요).
- counter는 rate의 시간적분이라 엔진이 임의 창을 Δcounter/Δwall로 정확히 복원한다. gauge 점표본은 표본 사이 스파이크를 놓쳐 창 통계에 부정확하므로, 창 판정용 신호는 counter로 싣는다(예: PSI를 ratio gauge와 stall.time counter 두 형태로 발행, 14일 canonical은 counter).
- 시계열 계산이 엔진 한 곳에 모여 일관된다.
- 엔진이 base 단위를 재해석할 필요가 없다.

관련: [classification-rationale.md](../classification-rationale.md)의 적분값 근거.
