# Windows 수집 필드 실기 검증 현황

Windows 3세대(modern NT10 / win7 NT6.1 / legacy32 NT5.2) 실기(OpenStack fleet VM) payload를 verify-cap으로 대조 검증한 결과와 결정 사항. Linux 2종은 별도 코드경로로 이미 검증 완료·무관.

## 상태 요약

collect.c Windows 수집 필드 수정 5건 커밋 완료(ec4973f). 3세대 전수 실기 검증 통과. 릴리즈 바이너리 3종 FLEET_BINDIR 재스테이징 완료. 아래 "열린 후속 이슈"만 남는다.

## collect.c 수정 (커밋 ec4973f)

1. disks: `\\.\PhysicalDriveN` 열기 access=0 -> GENERIC_READ. IOCTL_DISK_GET_LENGTH_INFO가 access=0에서 ERROR_ACCESS_DENIED(5). (enumerate_physical_disks)
2. net_io: MIB_IF_ROW2 FilterInterface skip + HardwareInterface 아니면 kind=virtual. WFP/QoS 필터 드라이버가 기반 NIC 카운터를 복제해 physical 4~5배 중복 집계하던 것 정리. (enumerate_net_io, GetIfTable2)
3. legacy32 exe/comm: NT5.2 경로를 Toolhelp32(CreateToolhelp32Snapshot)로 채움. QueryFullProcessImageName은 Vista+라 2003에서 services.exe/listen_ports.comm 전부 null이던 것 해결. (fill_comm_for_pid `#else`)
4. saturation: perflib raw. cpu_run_queue(System\Processor Queue Length), mem_paging_rate(Memory\Pages/sec 누적), disk_queue[](PhysicalDisk\Current Disk Queue Length). (fill_saturation)
5. disk_io: 1차 소스 = NtQuerySystemInformation(SystemPerformanceInformation) 시스템 전역 누적 I/O. (query_system_io / enumerate_disk_io)

## disk_io 소스 결정 (되돌리지 말 것)

Windows disk_io는 NtQuerySystemInformation(SystemPerformanceInformation)의 시스템 전역 누적 I/O를 1차 소스로 쓴다. 단일 엔트리 device=PhysicalDrive0로 보고. NtQuery 불가 시에만 perflib PhysicalDisk per-disk(disk_io_perflib)로 폴백.

근거(실기 진단으로 확정):
- perflib PhysicalDisk 카운터(Disk Reads/sec 등)는 diskperf 성능 통계 수집에 의존한다. 카운터 정의·오프셋·인스턴스는 정상 해석되지만 raw 값이 환경에 따라:
  - 2012R2+virtio: raw=0 고정(diskperf 필터 미부착. IOCTL_DISK_PERFORMANCE도 ERROR_INVALID_FUNCTION(1), LogicalDisk도 0).
  - win2003/win2016: 부팅 후 리셋·비단조(예: reads 1500 -> 927). 시계열 델타가 음수/노이즈.
- NtQuery IoRead/WriteOperationCount·IoRead/WriteTransferCount는 I/O 매니저가 직접 유지 -> 단조증가·provider 독립·NT5.2~NT10 동작. 구조체 prefix 오프셋(+8/+16/+32/+36)은 32/64비트 공통.
- 트레이드오프: 시스템 전역 집계라 물리 디스크별 분해 없음(단일 디스크 fleet엔 무영향), 파일 I/O(네트워크 리다이렉터 등) 포함. 신뢰성 우선.

## 실기 검증 결과 (verify-cap 시계열, 단조증가 확인)

- modern (VM-WIN2016): disk_io reads 301806 -> 303875 단조증가. disks/net_io 필터/saturation/cpu_stat/net_io/mem 정상.
- win7 (VM-WIN2012R2V11): disk_io reads 9660 -> 17858 단조증가. net_io 필터 정리(WFP 클론 virtual, tap physical), saturation·cpu_stat·services.exe·listen_ports.comm 정상. os_version 빈 문자열(2012R2 build 9600 정답).
- legacy32 (VM-WIN2003): disk_io reads 1363 -> 4568 단조증가(NtQuery가 NT5.2에서 동작 확인). Toolhelp32 exe/comm 정상(services 32개 exe 채움, listen_ports comm null 0). saturation·cpu_stat 정상. gateway=null(NT5.2는 GAA_FLAG_INCLUDE_GATEWAYS 미지원이라 설계상 null). os_version 빈 문자열(3790=2003 정답).

검증 원칙: 누적 카운터(cpu_stat, disk_io, net_io, mem_paging_rate)는 시계열 델타로 단조증가 확인(스냅샷 한 장 금지). 순간값(cpu_run_queue, disk_queue)은 idle이면 0 타당. 재부팅 경계 넘으면 리셋되니 새 인스턴스 샘플만 비교.

## 릴리즈 바이너리 / 스테이징

FLEET_BINDIR(`/home/debian/z-converter-assessment/agent`) 재스테이징 완료(SHA256SUMS 갱신, 임시 diskdiag.exe 삭제):
- assessment-agent-win2016-x64.exe: 85b31559a6dd8a624c853cd6cfe45fe1248df092378b952d68539878400d537a
- assessment-agent-win2008r2-x64.exe: 5faf71d29ebcb66216b661083465b23fde82ad637f246694b921fc8f212a36a1
- assessment-agent-win2003-x86.exe: 1f5e7027d3b2910a01192603410193a44e670a43a1329fff263c5f13f5086ba4
- linux 2종은 불변.

빌드 트리(vendor 재활용): `~/parallel-builds/{win-modern,win-win7,win-legacy32}/windows-agent`. 명령은 docs/BUILD.md. AGENT_VERSION=1.0.0 명시(로컬 빌드).

## 열린 후속 이슈

1. 커밋 ec4973f는 로컬 전용(origin/main 뒤처짐). 릴리즈 반영 필요 시 push + `git tag -f v1.0.0 && git push -f origin v1.0.0`(CI 재릴리즈). 미실행.
2. inventory가 부팅 직후(DHCP 리스 전) 1회 수집돼 interfaces IP/gateway가 미완성으로 잡힘 — win7은 IPv6 link-local만, win2003은 169.254 APIPA, gateway null. 1회성 수집이라 이후 갱신 안 됨. disk_io 수정과 무관한 별개 이슈. 대응 후보: 부팅 후 네트워크 up 대기 또는 inventory 주기 재발행.
3. saturation.disk_queue는 diskperf 미수집 환경(2012R2+virtio 등)에서 0 고정 가능(perflib PhysicalDisk 카운터 한계). 엔진 Windows 디스크 saturation 축이 그 환경에선 blind. disk_io(NtQuery)는 정상이므로 축을 disk_io 델타 기반으로 옮기는 것 검토 가능.
4. vm-win2016-verify(10.50.50.38, ACTIVE)는 별도 verify VM으로 구 빌드일 수 있음. hostname이 VM-WIN2016으로 겹쳐 verify-cap에서 vm-win2016(clean)과 섞여 보인다. 정리하려면 삭제하거나 clean으로 재배포.

## 인프라 / 배포·검증 방법 (재현용)

이 호스트가 bastion-099. openstack CLI/ntfs-3g/hivex/sudo -n 사용 가능. 프로비저닝 직접 수행.

배포(파괴적 — VM 삭제 후 boot-from-volume 재생성, 볼륨은 delete_on_termination=False라 보존):
```
export OS_CLOUD=openstack
cd /home/debian/z-converter-assessment/assessment-fleet-temp/provision
WIN_BIN=<modern|win7|legacy32> ./win-vol-inject.sh <glance-image> <exe-path> <vm-name>
```
- 이미지: win2016_x64_uefi_20G / win2012R2_x64_bios_20G_v1.1 / win2003_x86_bios_40G
- 배포 전 `openstack server show vm-<name>`의 root 볼륨이 `vol-<name>-edit`와 일치하는지 확인(스크립트 가정). 3종 모두 일치 확인됨.
- NTFS 마운트가 bastion 공유자원이라 배포는 순차(병렬 금지).
- win2003 부팅은 느림(첫 발행까지 수 분).

verify-cap(payload 캡처, 비소비):
- 브로커 127.0.0.1:15672, user/pass assessment/assessment, vhost /assessment, exchange assessment(direct).
- 큐 없으면 재생성:
```
curl -s -u assessment:assessment -H "content-type:application/json" -XPUT "http://127.0.0.1:15672/api/queues/%2Fassessment/verify-cap" -d '{"durable":false,"arguments":{"x-max-length":400}}'
for rk in server.inventory server.metrics; do curl -s -u assessment:assessment -H "content-type:application/json" -XPOST "http://127.0.0.1:15672/api/bindings/%2Fassessment/e/assessment/q/verify-cap" -d "{\"routing_key\":\"$rk\"}"; done
```
- peek: `POST /api/queues/%2Fassessment/verify-cap/get` body `{"count":400,"ackmode":"ack_requeue_true","encoding":"auto"}`.
- 진단이 필요하면 collect_metrics_payload에 임시 `_disk_diag` 필드를 넣어 배포(관측 후 제거)하는 방식으로 원인 규명함 — perflib 카운터 정의/raw/오프셋, NtQuery status/값, IOCTL err 등을 payload로 실어 대조.
