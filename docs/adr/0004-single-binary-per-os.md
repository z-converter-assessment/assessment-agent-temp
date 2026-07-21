# ADR-0004: OS 계열당 단일 바이너리

상태: 채택

## 맥락

fleet은 구세대부터 최신까지 폭이 넓다 — Linux는 EL6(커널 2.6.32, glibc 2.12)부터, Windows는 XP SP2/Server 2003 SP1(NT5.2)부터. OS별로 세대마다 산출물을 나누면 릴리즈·검증 매트릭스가 폭발한다.

## 결정

OS 계열당 정확히 하나의 바이너리를 산출하고, 두 OS가 같은 목표를 다른 방법으로 푼다.

- Linux: musl libc로 완전 static 링크(`assessment-agent-linux-x86_64`). 동적 로더·libc 의존이 없어 glibc 버전·배포판과 무관하게 커널 2.6.32 이상이면 로드된다. 커버는 링크 단계에서 끝난다.
- Windows: 단일 i686 바이너리(`assessment-agent-windows-x86.exe`)가 런타임에 세대를 분기한다. i686이라 32비트 XP/2003 네이티브 + 모든 x64 Windows WOW64를 한 바이너리로 만족한다. 세대 판별은 `agent_is_nt6`(RtlGetVersion) + GetProcAddress.

## 결과

- Linux는 "로드는 어디서나 되고 기능 유무만 갈린다"(커널 하한 미달 -> null, [0003](0003-value-measured-null-unmeasurable.md)).
- Windows는 "로드부터 지켜야 하고([0005](0005-nt52-load-guard.md)) 기능은 런타임 분기"다.
- 빌드 툴체인이 이 결정에 묶인다(Linux alpine musl / Windows debian:bookworm mingw i686 CRT v10). 상세 [BUILD.md](../BUILD.md).

관련 개념: [study/portability-concepts.md](../study/portability-concepts.md).
