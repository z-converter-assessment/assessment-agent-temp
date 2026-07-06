# assessment-agent-temp

Assessment 수집 에이전트. 운영 서버에 설치하면 시스템 인벤토리와 자원 지표를 수집해 브로커로 보낸다. OS에 따라 Linux용, Windows용 바이너리 중 맞는 하나만 설치한다.

## 릴리즈 산출물 (2종)

| 파일 | 설치 대상 |
|------|-----------|
| assessment-agent-linux-x86_64 | 전 x86_64 Linux (커널 2.6.32 이상, 배포판/glibc 무관) |
| assessment-agent-windows-x86.exe | Windows 전 세대 (Server 2003/XP ~ 2025, Win7~11, 32/64비트 공통) |

운영 서버에는 OS에 맞는 바이너리 하나만 설치한다. 둘 다 정적 링크라 OpenSSL/curl 같은 런타임 의존성을
따로 깔 필요가 없다. Linux 바이너리는 배포판이나 glibc 버전과 무관하게 커널 2.6.32 이상이면 그대로 돌고,
Windows 바이너리는 한 파일로 2003/XP부터 최신 Windows까지 커버한다(64비트 Windows에서도 그대로 동작하며,
실행 시 OS 세대를 감지해 그 OS가 지원하는 범위의 데이터를 수집한다 — 상세는 아래 Windows 항목). 둘 다
`install` 서브커맨드가 설치부터 설정 프롬프트, 서비스 등록까지 처리한다. 아래 대상 OS 항목 하나만 그대로
따라가면 되고, 각 항목은 자기완결적이다.

설치 시 브로커 주소(RABBITMQ_HOST), worker 다운로드 허용 호스트(WORKER_DOWNLOAD_ALLOWED_HOSTS),
비밀번호를 대화형으로 물어보고 나머지 값은 기본값을 쓴다.

각 항목의 무인 설치 한 줄은 필수 값(브로커 주소, 비밀번호)만 채우면 된다. 끝의 `< /dev/null`(Windows는 `< NUL`)이
stdin을 닫아, worker 허용 호스트 등 프롬프트로 안 채운 값은 기본값으로 자동 수락한다. worker 허용 호스트는
콤마로 여럿 이어 쓴다(기본값 `192.168.3.92,192.168.3.94` — 이미 2개). 다른 값을 쓰려면 해당 환경변수를 한 줄에 추가한다.

## 권한 요약

2종 모두 install에 관리자/root 권한이 필요하고, 서비스도 최고 권한 계정으로 실행된다. 에이전트가 다른 유저
소유 프로세스의 실행 경로와 소켓 소유 정보를 읽어 인벤토리를 채우는데, 이 접근이 root/LocalSystem에서만
가능하기 때문이다. 비특권으로 돌리면 sshd/nginx/mysql처럼 root나 전용 계정으로 뜨는 데몬의 포트-프로세스
매핑과 서비스 실행 경로가 대부분 비어서 수집의 핵심이 빠진다. 그래서 install과 런타임 실행 계정을 전부
최고 권한으로 통일한다.

Linux는 단일 바이너리지만 install이 호스트 init 시스템을 감지해 서비스 방식을 자동으로 고른다(systemd 호스트는
system 유닛, EL6/SLES11 같은 SysV 호스트는 init.d).

| 빌드 | install 권한 | 런타임 실행 계정 | 서비스 방식 |
|------|--------------|------------------|-------------|
| linux-x86_64 (systemd 호스트) | root (`sudo`) | root | systemd system (`/etc/systemd/system`) |
| linux-x86_64 (SysV 호스트: EL6/SLES11) | root (`sudo`) | root | SysV init (`/etc/init.d`) |
| windows-x86 (Server 2003/XP ~ 2025, Win7~11) | Administrator (관리자 명령프롬프트) | LocalSystem | Windows 서비스 (SCM) |

## (1) assessment-agent-linux-x86_64 — 전 x86_64 Linux (kernel >= 2.6.32)

정적 링크된 단일 바이너리라 배포판이나 glibc 버전과 무관하게 커널 2.6.32 이상이면 그대로 동작한다.
아래는 systemd 호스트 기준이고, SysV 호스트(EL6/SLES11)는 이어지는 절을 본다 — 같은 바이너리, 같은 명령이다.

설치 (root 필요):

```bash
chmod +x assessment-agent-linux-x86_64
sudo ./assessment-agent-linux-x86_64 install
```

무인 설치(플레이스홀더를 실제 값으로 바꿔 한 줄 실행):

```bash
sudo env RABBITMQ_HOST='<브로커주소>' RABBITMQ_PASS='<브로커비밀번호>' RABBITMQ_WORKER_PASS='<worker비밀번호>' ./assessment-agent-linux-x86_64 install < /dev/null
```

`install`이 브로커 주소/worker 허용 호스트/비밀번호를 프롬프트로 받는다. 바이너리는 `/usr/local/bin`,
설정은 `/etc/assessment-agent/agent.env`(비밀은 `agent.env.local`, 0600)에 놓이고, systemd 서비스로
등록되어 `systemctl`로 상시 구동되고 부팅 시 자동 시작한다.

운영:

```bash
systemctl status assessment-agent               # 상태
journalctl -u assessment-agent -f                # 로그
# 재설정: /etc/assessment-agent/agent.env 수정 후
sudo systemctl restart assessment-agent
sudo ./assessment-agent-linux-x86_64 uninstall   # 제거
```

### SysV 호스트 (EL6 / SLES 11)

systemd가 없는 구세대(CentOS/RHEL 6, SLES 11)에서는 위와 똑같은 `install` 명령을 쓰되, install이 init 시스템을
감지해 `/etc/init.d/assessment-agent`에 서비스를 등록한다(chkconfig/update-rc.d로 부팅 등록). 바이너리도
설정 경로(`/usr/local/bin`, `/etc/assessment-agent/agent.env`)도 동일하다. 운영 명령만 SysV 계열을 쓴다:

```bash
service assessment-agent status                  # 상태
tail -f /var/log/assessment-agent.log             # 로그
# 재설정: /etc/assessment-agent/agent.env 수정 후
sudo service assessment-agent restart
sudo ./assessment-agent-linux-x86_64 uninstall    # 제거
```

## (2) assessment-agent-windows-x86.exe — Server 2003/XP ~ 2016/2019/2022/2025, Win7~11

NT 세대 구분 없이 이 바이너리 하나를 쓴다(32비트지만 64비트 Windows에서도 그대로 동작). 관리자
명령프롬프트에서 1회 실행한다.

```bat
assessment-agent-windows-x86.exe install
```

무인 설치(플레이스홀더를 실제 값으로 바꿔 한 줄 실행):

```bat
set "RABBITMQ_HOST=<브로커주소>" & set "RABBITMQ_PASS=<브로커비밀번호>" & set "RABBITMQ_WORKER_PASS=<worker비밀번호>" & assessment-agent-windows-x86.exe install < NUL
```

`install`이 브로커 주소/자격증명을 프롬프트로 받고 `%ProgramData%\assessment-agent`(Server 2003/XP에서는
`C:\Documents and Settings\All Users\Application Data\assessment-agent`)에 설치한 뒤, LocalSystem
자동시작 Windows 서비스(assessment-agent)로 등록하고 시작한다(부팅 시 자동 실행). 설정 파일은 그 폴더의
`agent.env`(비밀은 `agent.env.local`).

운영:

```bat
sc query assessment-agent                          :: 상태
sc stop  assessment-agent                          :: 중지
sc start assessment-agent                          :: 시작(재설정 후)
assessment-agent-windows-x86.exe uninstall         :: 제거
```

재설정: `agent.env` 수정 후 `sc stop`/`sc start`. 실행 로그를 눈으로 확인하려면
`assessment-agent-windows-x86.exe run`으로 콘솔에서 포그라운드 실행한다. 실행 시 OS 세대를
감지해 최신 OS에서는 64비트 카운터, IPv6 게이트웨이, 프로세스 경로 등 풍부한 데이터를, NT5.2(2003/XP)에서는
그 OS가 지원하는 범위의 데이터를 수집한다.

## 빌드 / 릴리즈

소스 빌드(2종), CI 태그 릴리즈, 저장소 트리는 [docs/BUILD.md](docs/BUILD.md) 참고.
