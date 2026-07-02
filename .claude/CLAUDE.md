# 프로젝트: assessment-agent-temp

## 도구 호출 형식 (최우선 규칙)
- 도구(함수) 호출은 반드시 정식 function_calls 블록 형식으로만 생성한다. `<call>`/`<invoke>` 같은 잘못된 태그를 절대 쓰지 않는다. 잘못된 태그는 파싱 실패로 작업이 끊긴다.
- 도구 호출 직전에는 설명 텍스트를 최소화하고, 호출 구문 형식을 먼저 확인한 뒤 전송한다.

## 목적
Assessment 수집 에이전트(C)의 릴리즈 빌드 전용 트리. 단일 소스에서 OS 세대별 5종 바이너리를 산출한다. 빌드 방법 상세는 README.md.

## 릴리즈 산출물 (5종)
| 파일 | 타깃 | 프로파일 | 툴체인 |
| --- | --- | --- | --- |
| assessment-agent-linux-x86_64 | EL7+/Ubuntu18+/SUSE12+ (glibc 2.17) | modern | manylinux2014 |
| assessment-agent-linux-x86_64-glibc2.12 | EL6 (glibc 2.12) | legacy | manylinux2010 |
| assessment-agent-win2016-x64.exe | Server 2016+/Win10+ (NT 10.0) | modern | mingw-w64 x86_64 |
| assessment-agent-win2008r2-x64.exe | Server 2008R2+/Win7+ (NT 6.1) | win7 | mingw-w64 x86_64 |
| assessment-agent-win2003-x86.exe | Server 2003/XP (NT 5.2 i686) | legacy32 | debian:bookworm (gcc 12.2, CRT v10) |

## 빌드 제약 (반드시 준수)
- legacy32(win2003-x86)는 debian:bookworm 컨테이너로만 빌드한다. bookworm의 mingw-w64 CRT runtime 10.x가 NT5.2(XP/2003) startup을 유지한다. ubuntu 최신 등 CRT 12+ 툴체인으로 빌드하면 verify는 통과해도 XP/2003 실기 로드가 안 된다.
- legacy32 빌드 시 Makefile이 rabbitmq-c win32/threads를 NT5.2 패치(`windows-agent/patches/nt52-threads/`, SRWLock -> CRITICAL_SECTION)로 자동 교체한다.
- Linux release는 manylinux 컨테이너(native amd64)에서 빌드한다. glibc ABI 하한 = 빌드 호스트/컨테이너 glibc.
- win2003 실기 배포는 hive ComputerName + Tcpip MTU=1280 운영 설정을 사용한다.
- C 소스(src/, windows-agent/src/, include/)는 로직 변경 없이 유지가 기본. 빌드가 깨질 때만 최소 수정하고 5종 전부 재검증한다.

## 권한 모델 (root 전제 통일)
- install과 런타임 실행 계정을 전부 최고 권한으로 통일한다. 비특권 실행 모델은 두지 않는다. 근거: 수집기가 다른 유저 소유 프로세스의 `/proc/<pid>/exe`·`fd`·`comm`을 읽어야 `listen_ports[].pid`/`comm`과 `services[].exe`가 채워지는데, 이 접근이 root/LocalSystem에서만 가능하다. 비특권이면 sshd/nginx/mysql 등 대부분 데몬의 포트->PID 매핑이 null이 된다.
- Linux systemd: `/etc/systemd/system/assessment-agent.service`(User=root, WantedBy=multi-user.target), `systemctl`로 관리. install은 root 아니면 거부.
- Linux SysV(EL6): `/etc/init.d/assessment-agent`가 바이너리를 root로 직접 실행. 전용 유저(assessment-agent)·su 래핑 없음.
- Windows 3종: Administrator(elevated)로 install, LocalSystem 자동시작 서비스로 실행.
- 배포 스크립트/유닛(deploy/, scripts/image-prep.sh)은 이 모델을 전제로 유지한다. `systemctl --user`/lingering/XDG/`~/.local` 경로를 되살리지 않는다.
- 이 스크립트/유닛은 objcopy로 바이너리에 embed되므로, deploy/·scripts/image-prep.sh를 고치면 Linux 2종을 재빌드해야 반영된다.

## 릴리즈 (CI)
- `.github/workflows/release.yml`: `v*` 태그 push -> 5종 빌드 -> 해당 태그 GitHub Release 게시(softprops upsert).
- 릴리즈는 v1.0.0 하나로 유지한다. 재릴리즈는 `git tag -f v1.0.0 && git push -f origin v1.0.0`으로 덮어쓴다. 버전을 올릴 때만 새 태그를 쓴다.
- required: linux-x86_64, win2016-x64 (없으면 릴리즈 실패). 나머지 3종은 best-effort(있으면 붙이고 없으면 skip).
- 재릴리즈 전 이전에 실행 중인 GitHub Actions의 성공/실패를 확인하고 대기한다.

## 저장소
- github: `z-converter-assessment/assessment-agent-temp` (public)
- 로컬 빌드 아티팩트(vendor/, dist/, build/, *.o, *.exe, *.res)는 .gitignore 대상. 추적 파일만 커밋한다.

## 컨텍스트 관리
- 문서(README/CLAUDE.md)는 현황 선언형으로만 작성한다. 과거·변화 이력 서술("이전엔 X였는데 바뀌었다") 금지 — 헷갈림만 유발한다.
- 결정/제약은 이 문서 또는 README에 누적 기록. auto-memory 미사용(글로벌 정책).
