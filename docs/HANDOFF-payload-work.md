# 핸드오프: 엔진 payload 개선 작업 (새 세션 이어받기용)

엔진(assessment-engine)과의 payload 계약 개선 협업 진행 상황. 새 세션은 이 문서로 바로 이어받으면 된다.

## 협업 구조

- 엔진이 개선 요청을 이 repo `docs/engine-payload-change-requests.md`로 보냄(8개 항목).
- 우리 회신 + 결정은 엔진 repo `../assessment-engine/docs/agent-payload-change-response.md`에 기록(채택 필드명/실제 발행 형식/Windows 메모리 매핑표 포함).
- 엔진의 최종 결정은 이 repo `docs/engine-payload-change-decision.md`.
- 원칙: 전부 additive(신형+구형 병행 발행). agent_version 1.0.0 -> 1.1.0.

## 구현 완료 (커밋됨, agent_version 1.1.0)

커밋: 7737a08(정리) / 71c2743(1차) / 5a28518(2차·3차) / ca7239c(services pid 버그수정). 워킹트리 clean.

| 항목 | 상태 | 내용 |
|---|---|---|
| 1 device kind | 완료 L+W | disks/disk_io/net_io/mounts에 `kind`. 공용 분류기(src/collect.c의 disk_kind/net_kind/mount_kind, windows-agent win_net_kind). Windows는 coarse. mounts metrics variant에 fstype도 추가 |
| 2 services pid/exe | 완료 L+W | services[]에 pid/exe. Windows=EnumServicesStatusEx dwProcessId, Linux=`systemctl show -p Id,MainPID` 배치 조인(순서무관 페어링) + /proc/PID/comm |
| 3 interfaces | 완료 L+W | 구조화 interfaces[](name/address/prefix/family/kind, IPv6). ip_internal 병행 유지. Linux getifaddrs, Windows GetAdaptersAddresses |
| 4 agent_id | 완료 L+W | 첫 실행 시 UUID 생성->state dir 영구저장->모든 메시지에 additive. prep-image 짝(image-prep.sh + windows prep-image가 agent-id 삭제) |
| 6 Windows swap | 완료 | swap_free를 swap_total로 clamp(불변식 보장) |
| 7 boot_time | 종결 | Linux 이미 정적(btime), Windows 현행 유지(엔진 결정) |
| 8 task.result os | 완료 L+W | os_family/os_id/os_version. Windows os_version을 DisplayVersion으로 통일 |
| 5 Windows saturation | 부분 | metrics에 saturation{disk_queue, cpu_run_queue, mem_paging_rate}. disk_queue만 채움(IOCTL_DISK_PERFORMANCE.QueueDepth). cpu/mem은 perflib 실기검증 전까지 null |

## 검증 상태

- Linux(linux-x86_64): 이 호스트에서 발행 payload를 ground truth와 전 필드 대조 완료. agent_id(영구파일 일치), boot_time(btime), interfaces(ip addr, IPv4+IPv6), disk/net/mount kind, services pid(systemctl MainPID)/exe(/proc comm), loadavg/swap/mem/cpu_stat(다중샘플+/proc 브래킷) 전부 일치. 검증 중 services pid off-by-one 버그 발견·수정(ca7239c).
- Windows 3종: 실기 미검증. PE라 이 Linux 호스트에서 실행 불가.

## 산출물

- 검증된 5종 + SHA256SUMS: `/home/debian/z-converter-assessment/agent/` (fleet deploy가 읽는 FLEET_BINDIR).
- 빌드: Linux는 프로젝트 루트에서 `sh scripts/build-linux.sh`(docker manylinux, dist/), Windows는 `cd windows-agent && make PROFILE=... release`(windows-agent/dist/). vendor 재활용하려면 ~/parallel-builds/<프로파일>/ 격리카피에서 `make clean && make USE_VENDORED=1 release`(linux) / `make PROFILE=... release`(win).

## 남은 일 (Windows 실기 필요)

1. Windows 필드 검증: 새 바이너리를 Windows VM에 배포해야 함.
   - 이 fleet(`../assessment-fleet-temp`)은 WinRM 자격증명 없음. 배포는 오프라인 vol-inject(provision/win-vol-inject.sh, 프로비저닝 담당 몫, 공유 NTFS 마운트라 동시실행 금지).
   - deploy-win.sh의 EXE 매핑이 옛 이름(assessment-agent.exe/win7.exe/legacy-x86.exe)이라 새 이름과 안 맞음. 매핑 필요: modern->assessment-agent-win2016-x64.exe, win7->assessment-agent-win2008r2-x64.exe, legacy32->assessment-agent-win2003-x86.exe.
   - 담당이 새 바이너리 배포하면, 브로커의 verify-cap 큐(아래)에서 windows 호스트 payload를 꺼내 Linux와 동일 방식으로 검증.
2. item5 perflib: cpu_run_queue(System\Processor Queue Length), mem_paging_rate(Memory\Pages/sec)를 HKEY_PERFORMANCE_DATA로 구현(pdh.dll 의존 회피). 실 Windows에서 typeperf로 2+샘플 대조 검증 후 채택. 그 전까지 null 유지(값 정합성).

## 검증 인프라 (재사용)

- 브로커: 이 호스트 docker `dev-rabbitmq-1`, amqp 127.0.0.1:5672, mgmt 15672. vhost `/assessment`, user/pass `assessment/assessment`(administrator). exchange `assessment`(direct). 에이전트는 exchange를 passive 선언 -> 캡처용 throwaway는 미리 declare해야 함.
- 캡처 큐: `verify-cap`(x-max-length 400)이 실제 `assessment` exchange의 server.inventory/server.metrics를 미러링 중. 담당이 Windows 새 바이너리 배포하면 여기서 os_family=windows payload를 꺼내 검증.
- 로컬 Linux 스모크(재현): throwaway exchange `assessment-test`+큐 `smoke-q`(server.*바인딩) 만들고 `RABBITMQ_HOST=127.0.0.1 RABBITMQ_VHOST=/assessment RABBITMQ_USER=assessment RABBITMQ_PASS=assessment RABBITMQ_EXCHANGE=assessment-test RABBITMQ_WORKER_USER= AGENT_INTERVAL_SEC=2 AGENT_EXTERNAL_IP=203.0.113.1 ./assessment-agent-linux-x86_64` (Linux는 인자 없이 실행=수집루프, `run`은 Windows 전용). mgmt API로 get: `POST http://127.0.0.1:15672/api/queues/%2Fassessment/<큐>/get`.
- 캡처 payload를 python으로 파싱 후 ground truth(`ip -j addr`, `lsblk -J`, `findmnt -J`, `/proc/stat`, `/proc/meminfo`, `systemctl show -p MainPID --value <unit>`)와 대조.

## Windows 필드 검증 체크리스트 (배포 후)

windows payload에서 확인: interfaces(GetAdaptersAddresses, kind IfType coarse), disks/mounts kind(physical/data 상수), net_io kind, services pid(dwProcessId)/exe, agent_id(%ProgramData%\assessment-agent\agent-id 일치), swap_free<=swap_total, task.result os_id/os_version(DisplayVersion), saturation.disk_queue(typeperf `\PhysicalDisk(_Total)\Current Disk Queue Length` 2+샘플 대조). Linux에서 그랬듯 실제 값이 OS 도구와 일치하는지 엄밀 대조.
