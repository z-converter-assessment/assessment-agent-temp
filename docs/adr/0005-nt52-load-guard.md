# ADR-0005: NT6+ 전용 API 하드임포트 금지 (NT5.2 로드 가드)

상태: 채택

## 맥락

Windows는 시스템 DLL(ntdll/kernel32 등)을 정적 링크할 수 없고, PE import 테이블의 심볼을 로더가 로드 시점에 해소한다. NT6+ 전용 API를 import 테이블에 하드링크하면, 그 심볼이 없는 XP/2003(NT5.2)에서는 실행 전 로드 자체가 거부된다. [0004](0004-single-binary-per-os.md)의 단일 i686 바이너리가 NT5.2에서 로드되려면 이 표면을 지켜야 한다.

## 결정

NT6+ 전용 API(GetIfTable2/FreeMibTable/QueryFullProcessImageNameW/inet_ntop/GetTickCount64/BCrypt* 등)를 하드임포트하지 않는다. 전부 GetProcAddress로 런타임 해소하고, 없으면(구세대) 대체 경로나 null로 폴백한다. 벤더 라이브러리는 `_WIN32_WINNT=0x0502`로 빌드해 NT6 API(SRWLock 등)를 피하고, rabbitmq-c 스레드는 NT5.2 패치(SRWLock -> CRITICAL_SECTION)로 교체한다.

## 결과

- verify가 (1) import DLL이 시스템 DLL뿐인지 (2) NT6+ 심볼 하드임포트가 없는지를 검사해 위반 시 빌드를 실패시킨다.
- 실기 하한은 XP SP2 / Server 2003 SP1이다 — 수집기가 GetExtendedTcpTable/GetSystemTimes를 하드임포트하는데 이들이 그 SP부터 export되기 때문. NT6+ 심볼만 보는 verify denylist로는 이 SP 하한을 못 잡아, 실기 검증을 병행한다.
- secure-CRT(strncpy_s 등)도 2003 msvcrt에 없어 `nt52_compat.h` 로컬 구현으로 대체한다.

관련: [BUILD.md](../BUILD.md) 빌드 규칙, [architecture.md](../architecture.md) 4.2절.
