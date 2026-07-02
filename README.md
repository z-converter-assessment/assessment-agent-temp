# assessment-agent-temp

Assessment 수집 에이전트(C)의 릴리즈 빌드 전용 트리. 단일 소스에서 OS 세대별 5종 바이너리를 산출한다. 전송 wire 스키마는 5종 동일하다.

## 릴리즈 산출물 (5종)

| 파일 | 타깃 OS | ABI / 런타임 | 프로파일 |
|------|---------|--------------|----------|
| assessment-agent-linux-x86_64 | Ubuntu 18.04+, EL7-9, Amazon Linux 2/2023, SUSE 12/15 | glibc 2.17 / kernel 3.10 | manylinux2014 |
| assessment-agent-linux-x86_64-glibc2.12 | CentOS/EL 6 | glibc 2.12 / kernel 2.6.32 | manylinux2010 |
| assessment-agent-win2016-x64.exe | Server 2016/2019/2022/2025, Win10/11 | NT 10.0 x64, OpenSSL 3.x | modern |
| assessment-agent-win2008r2-x64.exe | Server 2008R2/2012/2012R2, Win7/8/8.1 | NT 6.1 x64, OpenSSL 3.x | win7 |
| assessment-agent-win2003-x86.exe | Server 2003 / XP (32-bit) | NT 5.2 i686, OpenSSL 1.0.2u | legacy32 |

운영 서버에는 OS에 맞는 바이너리 1개만 설치한다. 5종 모두 정적 링크라 런타임 의존성(OpenSSL/curl 등)을
따로 설치할 필요가 없다 — 파일 하나가 곧 에이전트다. 5종 모두 `install` 서브커맨드가 설치부터 설정
프롬프트, 서비스 등록까지 처리한다. 아래에서 대상 OS 항목 하나만 그대로 따라가면 되고, 각 항목은 자기완결적이다.

구분법: Linux는 `ldd --version` 첫 줄 glibc가 2.17 미만이면 glibc2.12 빌드, 이상이면 x86_64 빌드. Windows는
NT 세대로 고른다. 설치 시 브로커 주소(RABBITMQ_HOST), worker 다운로드 허용 호스트(WORKER_DOWNLOAD_ALLOWED_HOSTS),
비밀번호를 대화형으로 물어보고 나머지 값은 기본값을 쓴다.

각 항목의 무인 설치 한 줄은 필수 값(브로커 주소, 비밀번호)만 채우면 된다. 끝의 `< /dev/null`(Windows는 `< NUL`)이
stdin을 닫아, worker 허용 호스트 등 프롬프트로 안 채운 값은 기본값으로 자동 수락한다(허용 호스트 기본값은
`192.168.3.92,192.168.3.94`). 다른 값을 쓰려면 해당 환경변수를 한 줄에 추가한다.

## 권한 요약

5종 모두 install에 관리자/root 권한이 필요하고, 서비스도 최고 권한 계정으로 실행된다. 에이전트는 다른 유저
소유 프로세스의 실행 경로(`/proc/<pid>/exe`)·소켓 소유 PID·comm 등을 읽어 인벤토리를 채우는데, 이 접근이
root/LocalSystem에서만 가능하기 때문이다. 비특권으로 돌리면 sshd/nginx/mysql처럼 root나 전용 계정으로 뜨는
데몬의 리슨 포트->PID 매핑(`listen_ports[].pid`/`comm`)과 `services[].exe`가 대부분 `null`이 되어 수집의
핵심이 빠진다. 그래서 설계상 install과 런타임 실행 계정을 전부 최고 권한으로 통일했다.

| 빌드 | install 권한 | 런타임 실행 계정 | 서비스 방식 |
|------|--------------|------------------|-------------|
| linux-x86_64 | root (`sudo`) | root | systemd system (`/etc/systemd/system`) |
| linux-x86_64-glibc2.12 | root (`sudo`) | root | SysV init (`/etc/init.d`) |
| win2016 / win2008r2 / win2003 | Administrator (관리자 명령프롬프트) | LocalSystem | Windows 서비스 (SCM) |

## (1) assessment-agent-linux-x86_64 — Ubuntu 18.04+, RHEL/CentOS 7-9, Amazon Linux 2/2023, SUSE 12/15

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
설정은 `/etc/assessment-agent/agent.env`(비밀은 `agent.env.local`, 0600)에 놓이고
`/etc/systemd/system/assessment-agent.service`(User=root)로 등록되어 `systemctl`로 상시 구동된다.
`WantedBy=multi-user.target`이라 부팅 시 자동 시작한다.

운영:

```bash
systemctl status assessment-agent               # 상태
journalctl -u assessment-agent -f                # 로그
# 재설정: /etc/assessment-agent/agent.env 수정 후
sudo systemctl restart assessment-agent
sudo ./assessment-agent-linux-x86_64 uninstall   # 제거
```

VM 이미지로 복제 배포하는 경우 골든 이미지 봉인 직전에 `sudo ./assessment-agent-linux-x86_64 prep-image`를
실행해 머신 식별자를 초기화한다(클론마다 고유 ID 확보).

## (2) assessment-agent-linux-x86_64-glibc2.12 — RHEL/CentOS 6

systemd가 없는 세대라 `/etc/init.d`에 서비스를 등록한다. install과 런타임 모두 root다.

```bash
chmod +x assessment-agent-linux-x86_64-glibc2.12
sudo ./assessment-agent-linux-x86_64-glibc2.12 install
```

무인 설치(플레이스홀더를 실제 값으로 바꿔 한 줄 실행):

```bash
sudo env RABBITMQ_HOST='<브로커주소>' RABBITMQ_PASS='<브로커비밀번호>' RABBITMQ_WORKER_PASS='<worker비밀번호>' ./assessment-agent-linux-x86_64-glibc2.12 install < /dev/null
```

`install`이 RABBITMQ_HOST / worker 허용 호스트 / 비밀번호를 프롬프트로 받는다. 바이너리는 `/usr/local/bin`,
설정은 `/etc/assessment-agent/agent.env`(비밀은 `agent.env.local`), 서비스 스크립트는
`/etc/init.d/assessment-agent`(chkconfig/update-rc.d로 부팅 등록).

운영:

```bash
service assessment-agent status                  # 상태
tail -f /var/log/assessment-agent.log             # 로그
# 재설정: /etc/assessment-agent/agent.env 수정 후
sudo service assessment-agent restart
sudo ./assessment-agent-linux-x86_64-glibc2.12 uninstall   # 제거
```

VM 복제 배포 시 골든 이미지 봉인 직전 `assessment-agent prep-image`로 머신 식별자를 초기화한다.

## (3) assessment-agent-win2016-x64.exe — Server 2016/2019/2022/2025, Win10/11

관리자 명령프롬프트에서 1회 실행한다.

```bat
assessment-agent-win2016-x64.exe install
```

무인 설치(플레이스홀더를 실제 값으로 바꿔 한 줄 실행):

```bat
set "RABBITMQ_HOST=<브로커주소>" & set "RABBITMQ_PASS=<브로커비밀번호>" & set "RABBITMQ_WORKER_PASS=<worker비밀번호>" & assessment-agent-win2016-x64.exe install < NUL
```

`install`이 브로커 주소/자격증명을 프롬프트로 받고 `%ProgramData%\assessment-agent`에 설치한 뒤,
LocalSystem 자동시작 Windows 서비스(assessment-agent)로 등록하고 시작한다(부팅 시 자동 실행).
설정 파일은 `%ProgramData%\assessment-agent\agent.env`(비밀은 `agent.env.local`).

운영:

```bat
sc query assessment-agent                          :: 상태
sc stop  assessment-agent                          :: 중지
sc start assessment-agent                          :: 시작(재설정 후)
assessment-agent-win2016-x64.exe uninstall         :: 제거
```

재설정: `%ProgramData%\assessment-agent\agent.env` 수정 후 `sc stop`/`sc start`. 실행 로그를 눈으로
확인하려면 `assessment-agent-win2016-x64.exe run`으로 콘솔에서 포그라운드 실행한다. VM 복제 배포 시
골든 이미지 봉인 직전 `assessment-agent-win2016-x64.exe prep-image`로 MachineGuid를 초기화한다.

## (4) assessment-agent-win2008r2-x64.exe — Server 2008R2/2012/2012R2, Win7/8/8.1

관리자 명령프롬프트에서 1회 실행한다.

```bat
assessment-agent-win2008r2-x64.exe install
```

무인 설치(플레이스홀더를 실제 값으로 바꿔 한 줄 실행):

```bat
set "RABBITMQ_HOST=<브로커주소>" & set "RABBITMQ_PASS=<브로커비밀번호>" & set "RABBITMQ_WORKER_PASS=<worker비밀번호>" & assessment-agent-win2008r2-x64.exe install < NUL
```

`install`이 브로커 주소/자격증명을 프롬프트로 받고 `%ProgramData%\assessment-agent`에 설치한 뒤,
LocalSystem 자동시작 Windows 서비스(assessment-agent)로 등록하고 시작한다(부팅 시 자동 실행).
설정 파일은 `%ProgramData%\assessment-agent\agent.env`(비밀은 `agent.env.local`).

운영:

```bat
sc query assessment-agent                          :: 상태
sc stop  assessment-agent                          :: 중지
sc start assessment-agent                          :: 시작(재설정 후)
assessment-agent-win2008r2-x64.exe uninstall       :: 제거
```

재설정: `%ProgramData%\assessment-agent\agent.env` 수정 후 `sc stop`/`sc start`. 실행 로그를 눈으로
확인하려면 `assessment-agent-win2008r2-x64.exe run`으로 콘솔에서 포그라운드 실행한다. VM 복제 배포 시
골든 이미지 봉인 직전 `assessment-agent-win2008r2-x64.exe prep-image`로 MachineGuid를 초기화한다.

## (5) assessment-agent-win2003-x86.exe — Server 2003 / XP

관리자 명령프롬프트에서 1회 실행한다.

```bat
assessment-agent-win2003-x86.exe install
```

무인 설치(플레이스홀더를 실제 값으로 바꿔 한 줄 실행):

```bat
set "RABBITMQ_HOST=<브로커주소>" & set "RABBITMQ_PASS=<브로커비밀번호>" & set "RABBITMQ_WORKER_PASS=<worker비밀번호>" & assessment-agent-win2003-x86.exe install < NUL
```

`install`이 브로커 주소/자격증명을 프롬프트로 받고 `%ProgramData%\assessment-agent`(2003에서는
`C:\Documents and Settings\All Users\Application Data\assessment-agent`)에 설치한 뒤, LocalSystem
자동시작 Windows 서비스(assessment-agent)로 등록하고 시작한다(부팅 시 자동 실행). 설정 파일은
그 폴더의 `agent.env`(비밀은 `agent.env.local`).

운영:

```bat
sc query assessment-agent                          :: 상태
sc stop  assessment-agent                          :: 중지
sc start assessment-agent                          :: 시작(재설정 후)
assessment-agent-win2003-x86.exe uninstall         :: 제거
```

재설정: `agent.env` 수정 후 `sc stop`/`sc start`. 실행 로그를 눈으로 확인하려면
`assessment-agent-win2003-x86.exe run`으로 콘솔에서 포그라운드 실행한다. VM 복제 배포 시 골든 이미지
봉인 직전 `assessment-agent-win2003-x86.exe prep-image`로 MachineGuid를 초기화한다.

## 빌드 / 릴리즈

소스 빌드(5종), CI 태그 릴리즈, 저장소 트리는 [docs/BUILD.md](docs/BUILD.md) 참고.
