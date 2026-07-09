# accept — v2 값 정합/시계열 불변식 acceptance 하네스

check-contract.sh(구조: 필드/타입/null)와 별개로, emit 값이 맞는지·시계열 델타가 정합적인지를
검증한다. 구현 페이즈마다 아래 probe를 testbed에서 돌려 게이트로 쓴다(scratchpad 아닌 추적 경로).

## probe

- `ts_integrity_linux.py` — Linux 부하 하 t0/t1 델타 -> 파생(%util/await/throughput/cpu-util) 불변식.
  단조성, %util∈[0,1], Σcpu.time≈dt×cores, device id t0/t1 안정. 대상 host에서 `sudo python3` 로.
- `devkey_linux.py` — device 안정키 계층 폴백(dm/uuid->serial->by-path). stripe/thin/RAW에서 id 채워지나.
- `eaxis_linux.py` — E축 canonical 소스(mdraid/btrfs/ext4/tcp/conntrack/hardware_corrupted/mce) 파싱.
- `winrm_run.py <ip> [ntlm]` — Windows testbed에 PS 전송(stdin). perflib raw-vs-cooked 대조용.

## 페이즈 -> 불변식 매핑 (게이트)

| 페이즈 | probe | 불변식 |
|---|---|---|
| P1 disk | ts_integrity_linux(st-lvm 부하) | disk.io_time->%util∈[0,1], disk.operation_time/operations->await sane, 단조, device serial/dm id |
| P1 disk(Win) | winrm_run(dp-win2016) | perflib raw!=0 & 단조, raw-derived %util/await ≈ Get-Counter cooked, await divisor(PerfFreq vs 1e7) 확정 |
| P2 cpu/mem | ts_integrity_linux | Σcpu.time≈dt×cores, per-cpu 수=nproc, memory available 대조, centos6 MemAvailable null |
| P3 net/psi | (emit vs /proc) | MAC 키 t0/t1 안정, PSI 값 대조, virtio speed null |
| P4 storage | devkey_linux(st-nested/st-lvm) | 7계층 parent 체인, stripe/thin dm/uuid, centos6 sysfs 폴백 |
| P5 errors | eaxis_linux(st-raid/st-btrfs/st-mpath) | mdraid state/errors, btrfs error_stats, ext4 errors_count 파싱 |

## testbed

검증 게스트 목록/구성과 접근 자격은 인프라 레포(assessment-infra-temp)를 단일 진실로 둔다.
이 레포엔 credential/IP 를 평문으로 두지 않는다.

- Linux: SSH 키(운영자/인프라 레포에서). storage-testbed-net 계열 + centos6(rescue).
- Windows: WinRM 5985(ntlm). 자격은 `WINRM_USER`/`WINRM_PASS` 환경변수로 주입.

## 사용 예

```
ssh -i <key> ubuntu@<linux-guest> 'sudo python3 -' < scripts/accept/ts_integrity_linux.py
WINRM_USER=... WINRM_PASS=... python3 scripts/accept/winrm_run.py <win-guest> ntlm < some.ps1
```
