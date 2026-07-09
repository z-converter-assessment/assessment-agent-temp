# 자원 적정성 분류 — 신호별 근거 + 에이전트가 예상한 소비 맥락 (에이전트 -> 엔진)

> 성격: 협의 입력. 에이전트는 자원 적정성 "분류 자체"를 하지 않고 근거 자료를 발행한다. 이 문서는
> 우리가 각 신호를 넣은 이유와, 그 신호가 어떤 분류 맥락으로 쓰일 것을 예상했는지(우리 의견)를 밝힌다.
> 엔진의 실제 분류가 이 예상과 다르면, 그 자체가 "우리가 다른 걸 수집해야 하나 / 신호 해석을 맞춰야 하나"의
> 재논의 신호다.

## 0. 우리가 예상한 분류 프레임

에이전트가 신호를 고른 전제 = USE Method 로 자원별 [부족(under)/과다(over)/유휴(idle)/정상] 분류 + 목표 사양 산출.
- Utilization(이용률) = 용량 대비 사용. 분모(capacity)가 있어야 %가 나온다 -> over/idle 판정.
- Saturation(포화) = 처리 한계 초과 대기. under(증설) 판정의 핵심.
- Errors(오류) = 사이징 숫자로 안 감. confidence(신뢰도 오염 게이트) + attention(운영 경보)으로.
- 출력 = 분류 + confidence + attention + 목표 사양(예 mem->24GB), 스토리지는 3계층(배정/파일시스템/확장여력).

핵심 설계 원칙(왜 raw counter·적분인가):
- 에이전트는 stateless -> emit 시점 raw 누적만 싣고 rate/delta 는 엔진. counter 는 rate 의 시간적분이라,
  엔진이 어떤 창(14일)이든 정확한 창평균을 Δcounter/Δwall 로 얻는다. 점표본(gauge avg) 은 샘플 사이를
  놓치고 평활 편향이 있어, 창평균 판정엔 적분(counter)이 정확하다. 이 원칙이 아래 stall.time 선택의 근거다.

## 1. Utilization 축 — 추가/변경 이유 + 예상 소비

- memory.usage state=available (MemAvailable / Windows Available): v1 은 used% 로 압박을 쟀는데 캐시가
  회수 가능분이라 used% 는 압박을 과대평가한다. available 은 회수 반영 실가용. 예상 소비: 압박 = 1 -
  available/limit (used% 아님). 미발행 구커널(centos6 2.6.32)만 MemFree+Buffers+Cached 계산 폴백.
- network.link.speed: v1 은 처리량(kB/s)만 있어 "한계 대비 %"를 못 냈다. 링크속도 분모를 넣어 util =
  throughput/link_speed. 예상 소비: 네트워크 이용률. 단 virtio 는 링크속도 null -> 이용률 미산출(수용).
- disk.io_time (%util): v1 은 await(포화)만 있고 장치 이용률(busy 시간 비율)이 없었다. USE 의 U축을 채운다.
  예상 소비: 디스크 이용률 = busy 시간분율. (await/queue 는 S축, 별개.)

## 2. Saturation 축 — 추가/변경 이유 + 예상 소비 (적분값 강조)

- PSI (pressure.stall): 왜 넣었나 = 현대 saturation 정본(Gregg 권장). cpu/mem/io 를 한 ratio 축으로 통일하고,
  swapless Linux 의 메모리 포화 사각(page-out 이 구조적 0)을 근본 해소한다. 예상 소비: saturation 1차 신호.
  - 적분값을 추가한 이유(핵심): PSI 를 두 형태로 발행한다.
    - pressure.stall.ratio (gauge, avg10/60/300) = 지수감쇠 점표본.
    - pressure.stall.time (counter, 누적 stall 초 = 시간적분).
    14일 창 분류엔 stall.time(적분)이 정확하다. avg10 을 60초마다 표본하면 표본 사이 stall 을 놓치고 평활
    편향이 생기지만, 누적 stall 초를 counter 로 실으면 엔진이 Δstall.time/Δwall 로 임의 창의 정확한 시간가중
    평균 압박을 얻는다(표본 손실 없음). 그래서 14일 saturation 의 canonical 소스는 stall.time(적분)이고
    ratio 는 실시간 참고용. 예상 소비: saturation(14일) = Δstall.time/Δwall.
- cpu.run_queue (procs_running): 실행 큐 길이 = USE 고전 CPU 포화. loadavg 대체(loadavg 는 D-state IO
  블록 오염). 예상 소비: run_queue/cores 로 CPU 포화.
- cpu.blocked (procs_blocked, D-state): 인과 분리용. 디스크 I/O 발 CPU 로드에서 "디스크가 root, CPU 는 증상"을
  가른다. 예상 소비: disk->cpu 인과 게이트(PSI io 없는 커널/Windows 에서 특히).
- memory.commit.usage/.limit (commit ratio): Windows + swapless Linux 의 메모리 포화 대리. committed/limit 이
  pagefile 사용량보다 정확(pagefile 은 여유 RAM 에도 baseline 이라 오도). 예상 소비: PSI 없을 때 메모리 포화 폴백.
- paging.operations (page-out): 스와핑 = 메모리 포화(Gregg). 예상 소비: 포화 보조. swapless 는 0 -> PSI/commit 이 커버.
- disk.operation_time + disk.pending_operations (await + queue): 디스크 포화(지연 + 큐). await =
  Δoperation_time/Δoperations (이 역시 적분 기반 델타). 예상 소비: 디스크 I/O 포화. Windows 는 IOCTL 폐기,
  perflib 델타 await 로 양 OS 통일.

## 3. Errors 축 — 추가 이유 + 예상 소비 (사이징 아님)

발행하는 E축(disk.errors=mdraid mismatch + ext4 errors_count, network errors/dropped/tcp.retransmits/conntrack,
memory.hardware_corrupted, memory.edac[VM null])을 넣은 이유와 예상 소비를 명확히 한다 — 이건 사이징 숫자로
가지 않는다. 미발행 소스(btrfs error_stats, scsi ioerr_cnt, cpu MCE)는 간단한 카운터 부재/미구현이라 null 대신
아예 발행하지 않는다(메모리 에러는 hardware_corrupted 가 대체 커버).

- 예상 소비 1) confidence 오염 게이트: 자원에 fault 가 있으면 그 자원의 U/S 읽기가 오염됐을 수 있다 ->
  분류를 막지 말고 신뢰도만 하향(엔진의 steal_biased 패턴과 동형). 예: disk fault(mdraid degraded/btrfs
  corruption/ext4 errors) -> 디스크 U/S 신뢰도 하향.
- 예상 소비 2) monitoring/attention: 운영 신호로 노출. tcp.retransmits -> 네트워크 품질(net_retrans_pct 계층3),
  conntrack usage/limit -> 네트워크 포화(테이블 고갈 ratio), disk fault -> "배열 degraded" 경보.
- 왜 완전체로 넣었나: OpenStack VM 에서 구조적으로 불가능한 건 메모리 ECC(EDAC mc 미등록)뿐이고 그 자리는
  HardwareCorrupted 가 대체 커버. 나머지는 fault 시 실신호라(소프트RAID 멤버 faulty, corruption_errs,
  retrans, discards) 관례상 파싱하는 게 맞다. 사이징을 오염시키지 않으면서 신뢰도/경보로만 소비.

## 4. 스토리지 3계층 — 추가 이유 + 예상 소비

block_devices(parent-by-id) + lvm_vgs(free_bytes): v1 의 major/minor 조인은 dm/LVM 에서 끊긴다. parent-by-id 로
fs->물리디스크 확정 매핑, VG free_bytes 로 "확장 여력"(3번째 계층)을 실측. 예상 소비: 용량 사이징을
배정 블록 / 파일시스템 / 확장 가능-미할당(VG free) 3계층으로. Windows 확장여력은 디스크크기-파티션합(미할당).

## 5. 단위/키의 근거

- base 단위(s/By/ratio): jiffies/sectors/100ns/% 를 걷어 단위 혼동 제거. 엔진 임계·집계가 단위 걱정 없이 소비.
- device 안정 id(디스크 dm/uuid..by-path, 네트워크 MAC): 시계열 자연키 안정 -> 창 통계가 device 재열거에
  안 끊긴다. 이름 표본이 아닌 안정 id 가 14일 창 누적의 전제.

## 6. 예상과 실제가 다를 때

이 문서는 "우리가 이렇게 쓰일 것을 예상하고 골랐다"는 의견이다. 엔진의 실제 분류 로직이 위와 다르면
(예: E축을 사이징에 반영, 또는 avg10 을 14일 창에 사용) 알려달라 — 신호 선택/해석을 재조정한다. 특히
stall.time(적분) vs ratio(점표본) 의 14일 창 소비 방식은 명시적으로 합의하고 싶다.
