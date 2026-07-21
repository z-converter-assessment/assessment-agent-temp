# ADR-0007: install·런타임을 root/LocalSystem으로 통일

상태: 채택

## 맥락

인벤토리의 핵심은 포트-프로세스 매핑(`listen_ports[].pid`/`comm`)과 서비스 실행 경로(`services[].exe`)다. 이 값들은 타 유저 소유 프로세스의 `/proc/<pid>/exe`·`fd`·`comm`(Linux)이나 프로세스 이미지 경로(Windows)를 읽어야 채워진다. 비특권 실행이면 sshd/nginx/mysql처럼 root나 전용 계정으로 뜨는 대부분 데몬의 매핑이 null이 되어 수집의 핵심이 빠진다.

## 결정

install과 런타임 실행 계정을 전부 최고 권한으로 통일한다. 비특권 실행 모델은 두지 않는다.

- Linux: systemd(`User=root`) 또는 SysV(EL6, `/etc/init.d`에서 root 직접).
- Windows: LocalSystem 자동시작 서비스.

## 결과

- 대상 호스트가 전부 우리 소유·관리 fleet이라는 전제 위에서만 성립한다. 이 에이전트는 방어적·운영 목적의 IT 자산관리 도구다 — 표준 인벤토리(netstat/ss/tasklist와 같은 범주)를 위한 특권이지 감시·횡적 이동·탐지 회피 용도가 아니다.
- `systemctl --user`/lingering/XDG/`~/.local` 같은 비특권 경로를 되살리지 않는다.
- agent_id 등 state는 install dir과 분리된 machine-wide 경로(Linux `/var/lib`, Windows `%ProgramData%`)에 둔다.

관련: [architecture.md](../architecture.md) 9절.
