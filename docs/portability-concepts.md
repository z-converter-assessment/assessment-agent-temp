# 빌드와 이식성 개념 — ISA / ABI / libc / 커널 (Linux vs Windows)

공부용 정리. 한 번 컴파일한 바이너리가 "어느 머신에서 도는가"를 결정하는 층들을, 특정 프로젝트와 무관하게 일반 개념으로 정리한다.

---

## 1. 실행 바이너리의 층 (아래 -> 위)

```
   application code
   ----------------------------------------
   libc          (glibc / musl / msvcrt / UCRT)
   ----------------------------------------
   kernel ABI    (syscall interface)
   ----------------------------------------
   ABI           (calling convention, object format)
   ----------------------------------------
   ISA           (x86-64 / IA-32 : CPU instructions)
```

- ISA (Instruction Set Architecture): CPU 명령어 집합. x86_64(64비트), i686(32비트 x86). "기계어 타깃"이 이걸 가리킨다.
- ABI (Application Binary Interface): 그 명령어로 짠 코드들이 서로 맞물리는 규약 — 호출 규약(인자 전달/스택 정리), 타입 크기·정렬, 실행/오브젝트 포맷(ELF/PE), 이름 맹글링, 링크 방식.
- kernel ABI: 유저 코드가 커널에 요청하는 규약(syscall 번호·인자 규칙). OS마다 완전히 다르다.
- libc: 유저 코드가 syscall을 직접 안 치고 표준 함수(printf/malloc/open)를 쓰게 해주는 라이브러리. 내부에서 syscall로 번역. Linux는 glibc/musl, Windows는 msvcrt/UCRT.

ISA 와 ABI 는 다른 층이다. ISA 는 "명령어 어휘", ABI 는 "그 어휘로 짠 코드들이 호출·링크되는 규칙". 하나의 ISA 위에 ABI 가 여럿 있을 수 있다(예: x86-64에 System V ABI[Linux] vs Microsoft x64 ABI[Windows] — 같은 명령어, 다른 호출 규약).

### x86 계열 이름 정밀 정리 (ISA / 패밀리 / 마이크로아키텍처)

x86_64, x86, i686 은 같은 층위의 ISA 표현이 아니다. 정밀도가 다르다.

- x86: ISA 패밀리(우산 용어). 8086에서 파생된 명령어 집합 계열 전체. 문맥에 따라 좁게는 32비트만, 넓게는 x86_64까지 포함 -> 그 자체로는 특정 ISA 하나가 아니다.
- x86_64: 구체적 ISA. x86의 64비트 확장. 동일한 ISA 를 벤더/툴마다 다르게 부른다: x86_64 / x86-64(GNU·Linux), AMD64(AMD, 설계 주체), Intel 64 / EM64T(Intel).
- i686: 별개 ISA 가 아니라 IA-32(32비트 x86 ISA) 안의 명령어 레벨/베이스라인. 원래 Pentium Pro(P6) 세대 이름인데 컴파일러 타깃 레벨로 겸용된다(`-march=i686` = P6 세대까지의 명령을 써도 됨).

정식 ISA 명칭:
- 32비트 = IA-32 (GNU 툴체인은 i386 으로 부름)
- 64비트 = x86-64 (= AMD64 = Intel 64)
- x86 = 위 둘을 아우르는 패밀리 이름

즉 "정확히 ISA 하나"에 해당하는 건 IA-32(32비트)와 x86-64(64비트)다. x86 은 패밀리, i686 은 IA-32 안의 레벨.

ISA vs 마이크로아키텍처 (이 혼동의 근원):
- ISA = 프로그램이 보는 명령어 인터페이스(어떤 명령·레지스터가 있나). 추상 규격.
- 마이크로아키텍처(microarchitecture) = 그 ISA 를 구현한 물리 CPU 설계(P6, Skylake, Zen 등).
- iN86 시리즈(i386/i486/i586/i686)는 원래 CPU 세대(마이크로아키텍처) 이름이다. 각 세대가 명령을 조금씩 늘려서(i686/P6 은 CMOV 등 추가) IA-32 의 "명령어 레벨" 표기로도 쓰이게 됐다. 그래서 i686 은 마이크로아키텍처 이름이 ISA 베이스라인 타깃으로 겸용된 경우라, "ISA냐 마이크로아키텍처냐"가 애매하게 느껴진다.

### 프로세스 ABI vs 커널 ABI

ABI 는 "명령어를 호출하는 인터페이스"가 아니라, 따로 컴파일된 바이너리 조각들이 맞물리는 약속(규약)이다. 넘는 경계에 따라 둘로 나뉜다.

- 프로세스 ABI(유저스페이스 ABI): 유저 코드 ↔ 유저 코드(함수/라이브러리) 규약. 호출 규약(인자 레지스터/스택, 스택 정리, caller/callee-saved), 타입 크기·정렬, 실행/오브젝트 포맷(ELF/PE), 동적 링크 방식. 수평 경계.
- 커널 ABI(syscall ABI): 유저 코드 ↔ 커널 규약. 커널 진입 명령(x86-64 Linux `syscall`, 32비트 `int 0x80`/`sysenter`), syscall 번호 레지스터(x86-64 Linux rax), 인자 레지스터(rdi/rsi/rdx/r10/r8/r9), 반환·에러 규약, 번호 체계. 특권 전환(수직 경계).

같은 ISA 라도 둘의 규약이 다르다. x86-64 Linux 예:
- 함수 호출(System V ABI): 인자 rdi, rsi, rdx, rcx, r8, r9
- syscall(커널 ABI): 번호 rax, 인자 rdi, rsi, rdx, r10, r8, r9 (rcx 대신 r10 — `syscall` 명령이 rcx 를 덮어써서)

호출 사슬로 보면:

```
   app code
     |   process ABI  (calling convention, type layout, ...)
     v
   libc   (e.g. printf -> write)
     |   kernel ABI   (syscall number + arg regs + trap instruction)
     v
   kernel
```

보통 앱은 raw syscall 을 직접 안 치고 libc 함수(open/write)를 부르고, libc 가 프로세스 ABI 로 받은 걸 커널 ABI 로 번역해 syscall 을 친다. libc 가 두 ABI 를 잇는 다리다.

### C 바이너리는 커널에 어떻게 도달하나 (libc vs raw syscall)

- 기본 경로: C 코드가 libc 함수(open/read/socket/malloc/printf)를 부르고, 그 함수가 내부에서 syscall 을 친다.
- Linux 는 syscall ABI 가 공개·안정이라 libc 없이 raw 로 직접 칠 수도 있다(인라인 asm 으로 번호·인자 세팅 후 `syscall` 명령). 단 `syscall()` 함수는 libc 가 주는 얇은 래퍼라, 그걸 쓰면 여전히 libc 를 거친다(명령 `syscall` 과 함수 `syscall()` 은 다르다).
- static 링크 != libc 없음: "musl static" 은 libc(musl) 코드를 바이너리에 박아 넣은 것이다. 여전히 musl 함수를 쓰고 musl 이 syscall 을 친다. 런타임에 libc.so 를 안 찾을 뿐, libc 를 건너뛰는 게 아니다.
- 완전 libc 없이(freestanding/nolibc)는 `_start` 엔트리, argc/argv/env·TLS 셋업, malloc/stdio, errno 번역(raw syscall 은 실패 시 -errno 반환)까지 전부 직접 해야 해서 실애플리케이션엔 비현실적이다. 아주 작은 특수 바이너리에만 쓴다.
- Windows: raw syscall 을 안정적으로 못 친다(syscall 번호가 비공개·버전마다 변경). ntdll/kernel32 시스템 DLL 을 반드시 거쳐야 커널에 도달한다 -> CRT(libc 자리)를 빼도 시스템 DLL 을 못 피한다.

정리: Linux 는 libc 를 건너뛴 raw syscall 이 이론상 가능하지만, 실애플리케이션은 libc 를 쓴다(static 이면 동봉). Windows 는 시스템 DLL 경유가 강제라 이 선택지 자체가 없다.

---

## 2. "이 바이너리가 저 머신에서 도는가" 판정 4요소

1. ISA 호환: CPU가 그 명령어를 실행할 수 있나. (i686 코드는 i686+ CPU에서. 64비트 CPU도 32비트 하위호환.)
2. 실행 포맷/ABI: OS 로더가 그 포맷을 이해하나. Linux ELF, Windows PE. 포맷이 다르면 아예 안 뜬다.
3. kernel/syscall: 바이너리가 쓰는 syscall/OS API가 그 커널·OS 버전에 있나.
4. libc/의존 라이브러리: 동적 링크면 호스트에 맞는 버전이 있나. 정적 링크면 의존이 없다.

이식성 문제는 대개 3, 4에서 터진다. 1, 2는 아키텍처/OS를 정하면 대체로 고정.

---

## 3. Linux 관점

### 다양성의 원천
배포판·버전마다 glibc 버전과 커널 버전이 제각각이다. 예: EL6(glibc 2.12, 커널 2.6.32) 부터 최신(glibc 2.38+, 커널 6.x)까지.

### 커널 ABI 는 안정적이다
Linux 는 "don't break userspace" 원칙으로 syscall ABI 를 하위호환 유지한다. 옛 바이너리가 새 커널에서 그대로 돈다. 반대로 새 syscall 을 쓰는 바이너리는 그 syscall 이 도입된 커널 이상이 필요하다(= 커널 하한).

### libc: glibc vs musl
- glibc: 심볼 버전징(symbol versioning)을 쓴다. 예를 들어 `GLIBC_2.34` 버전의 심볼을 참조하면, 그보다 낮은 glibc 를 가진 호스트에선 로드 실패("version GLIBC_2.34 not found"). 동적 링크가 기본이라 glibc 버전 종속이 크다.
- musl: 심볼 버전징이 없고 단순하며 정적 링크 친화적으로 설계됐다.

### 이식성 해법: 정적 링크로 의존 제거
- musl 로 완전 static 링크 -> libc 의존이 아예 없다(ELF 에 PT_INTERP 없음 = 동적 로더 ld.so 를 안 탐). 호스트에 필요한 syscall 만 있으면 배포판·glibc 버전 무관하게 돈다.
- glibc static 은 함정이 있다: NSS(getpwnam/DNS 등 name service switch)가 내부에서 dlopen 을 써서 "완전 static"이 깨진다. 그래서 완전 이식 static 바이너리엔 musl 이 정석.

정리: Linux 이식성 = 링크 시점에 의존을 제거(static musl) + 커널 하한만 지키면 끝. 로드 시점 얘기가 거의 없다(정적이라 의존이 없음).

---

## 4. Windows 관점

### 다양성의 원천
NT 커널 세대마다 쓸 수 있는 API 가 다르다. NT5.x(XP/2003) ~ NT6.x(Vista/7/2008/2012) ~ NT10(2016/10/11). 세대가 오르며 API 가 추가된다.

### "커널"을 정적 링크하지 않는다
Windows 는 ntdll.dll / kernel32.dll 등 시스템 DLL 이 항상 동적 링크된다(정적 링크 불가). 바이너리의 PE import table 에 필요한 함수가 나열되고, 로더가 로드 시점에 그 심볼을 해소한다.
- 결과: 최신 API 를 import table 에 하드링크했는데 그 API 가 옛 Windows 에 없으면 -> 실행 전에 로드 자체가 실패(함수 심볼을 못 찾음).

이게 Linux 와 결정적으로 다른 지점이다. Linux 는 정적 링크로 의존을 없앨 수 있지만, Windows 는 시스템 DLL 의존을 없앨 수 없어서 "무엇을 import table 에 넣느냐"가 로드 가능 여부를 좌우한다.

### libc 대응: CRT (C runtime)
- msvcrt.dll: 오래된 시스템 C 런타임. 아주 옛 Windows 부터 전부 존재. mingw 가 전통적으로 이걸 링크.
- UCRT (Universal CRT): 모던(Win10+ 기본 탑재, 재배포 가능).
- 정적 CRT 도 가능. 옛 Windows 까지 커버하려면 msvcrt 링크가 흔하다.

### 이식성 해법: 하드임포트 회피 + 런타임 해소
- 최신 API 를 import table 에 안 넣고, 실행 중에 LoadLibrary/GetProcAddress 로 찾는다.
  - 있으면(신세대) 그걸 쓰고, 없으면(구세대) null 반환 -> 폴백 경로.
- 이러면 바이너리가 구세대에서도 로드는 되고, 기능만 런타임에 분기한다.
- 32비트(i686)로 빌드하면 64비트 Windows 도 WOW64 로 실행 -> 32/64비트를 하나로 커버.

정리: Windows 이식성 = 로드/실행 시점에 최신 API 를 동적 해소(하드임포트 금지) + 32비트로 아키텍처 폭 확보.

---

## 5. 핵심 대비 (개념의 축이 다르다)

| 관점 | Linux | Windows |
| --- | --- | --- |
| ISA | x86_64 (또는 i686) | i686 (32비트로 64비트까지 WOW64 커버) |
| 실행 포맷 | ELF | PE |
| 커널 진입 | syscall (ABI 안정, 하위호환) | 시스템 DLL(ntdll/kernel32) 경유, 정적 링크 불가 |
| libc | glibc(심볼 버전징) / musl(static 친화) | msvcrt(구) / UCRT(신) |
| 이식성 급소 | glibc 버전 종속 | 최신 API import -> 로드 실패 |
| 이식성 해법 | 정적 링크로 의존 제거(static musl) | 하드임포트 회피 + GetProcAddress 런타임 해소 |
| 해결 시점 | 링크 시점 | 로드/실행 시점 |
| 버전 하한 표현 | 커널 하한(필요 syscall) | 최소 API 레벨(_WIN32_WINNT) + 로드 가드 |

핵심 통찰:
- Linux 는 의존을 링크 때 없애서 이식성을 얻는다(static musl).
- Windows 는 의존을 실행 때 조건부로 풀어서 이식성을 얻는다(GetProcAddress 런타임 분기).
- 그래서 두 OS 를 나란히 말할 때 Linux 키워드는 라이브러리(musl)이고 Windows 키워드는 아키텍처+로딩(i686 + 런타임 분기)이라, 층위가 안 맞아 보인다. 실제로 다른 층의 문제를 풀고 있어서 그렇다.

---

## 6. i686 개념 흐름 (지금까지 얘기 정리)

1. i686 = IA-32(32비트 x86 ISA) 안의 P6 세대 명령어 레벨. 별개 ISA 도, 라이브러리도, ABI 도 아니다(원래는 Pentium Pro/P6 마이크로아키텍처 이름). 정식 ISA 명칭은 32비트=IA-32, 64비트=x86-64.
2. 32비트라 32/64비트 Windows 모두 실행된다(64비트는 WOW64). 아키텍처 커버 폭이 최대.
3. ABI(호출규약/PE/링크)는 i686 위에 얹히는 별개 층. 같은 i686 이라도 Linux ABI 와 Windows ABI 가 달라 서로 호환 안 된다.
4. 하나의 i686 exe 로 여러 NT 세대를 커버하려면 -> 최신 API 하드임포트 금지 + GetProcAddress 런타임 분기(= 로드 가드).
5. libc 자리엔 msvcrt(시스템 CRT). 벤더 라이브러리(OpenSSL 등)는 정적 링크하고, 전부 i686 으로 빌드한다.

빌드 타깃 triplet 이 이 층들을 필드로 분리해서 보여준다:

```
   x86_64 - linux  - gnu            i686 - w64  - mingw32
     |        |       |               |      |       |
    ISA      OS      libc/ABI        ISA   vendor  OS/ABI
                     (glibc)        (arch)        (Windows PE +
                                                   mingw runtime)
```

triplet 자체가 "arch(ISA)"와 "os/abi"를 분리한다 -> i686 은 arch 칸, ABI 는 os/abi 칸.

---

## 7. 용어집

- ISA (Instruction Set Architecture): CPU 명령어 집합(프로그램이 보는 명령·레지스터 규격). "기계어 타깃". 정식 x86 계열 ISA = IA-32(32비트) / x86-64(64비트).
- x86: ISA 패밀리(우산 용어). IA-32 와 x86-64 를 아우름. 특정 ISA 하나가 아님.
- IA-32: 32비트 x86 ISA 의 정식 명칭(Intel). GNU 툴체인 표기로는 i386.
- x86-64 (= x86_64 = AMD64 = Intel 64): 64비트 x86 ISA. 같은 ISA 의 여러 벤더/툴 표기.
- 마이크로아키텍처(microarchitecture): ISA 를 구현한 물리 CPU 설계(P6, Skylake, Zen). ISA(추상 규격)와 구분. i386/i486/i586/i686 은 원래 이 세대 이름이며 IA-32 의 명령어 레벨 표기로도 쓰인다(예: `-march=i686`).
- ABI (Application Binary Interface): 바이너리 상호운용 규약 — 호출 규약, 타입 정렬, 실행/오브젝트 포맷, 링크. 하나의 ISA 에 여러 ABI.
- syscall / kernel ABI: 유저->커널 요청 규약. OS별 상이. Linux 는 하위호환.
- libc: 표준 C 라이브러리. Linux glibc/musl, Windows msvcrt/UCRT.
- static vs dynamic linking: 정적 = 의존 코드를 바이너리에 박음(런타임 의존 없음). 동적 = 실행 시 호스트의 .so/.dll 을 로드.
- dynamic loader: 동적 링크를 실행 시 해소하는 주체. Linux ld.so(ELF PT_INTERP), Windows 로더(ntdll).
- symbol versioning: glibc 가 심볼에 버전을 붙이는 방식(GLIBC_2.x). 호스트 glibc 가 그 버전 미만이면 로드 실패.
- import table (PE): Windows 바이너리가 필요로 하는 DLL·함수 목록. 로더가 로드 시 해소. 없는 심볼이 하나라도 있으면 로드 실패.
- WOW64 (Windows 32-bit on Windows 64-bit): 64비트 Windows 에서 32비트 프로세스를 투명하게 실행하는 계층.
- target triplet: `arch-vendor-os/abi`. 예 `x86_64-linux-gnu`, `i686-w64-mingw32`.

---

## 8. 한 장 요약

- 바이너리의 이식성은 4층에서 갈린다: ISA(CPU) / ABI(포맷·규약) / kernel(syscall·API) / libc(런타임 의존).
- Linux: 커널 ABI 가 안정적이고 libc 를 정적 링크(musl)할 수 있어 -> 링크 시점에 의존을 없애면 커널 하한만 넘기면 어디서나 돈다.
- Windows: 시스템 DLL 을 정적 링크 못 하고 import 는 로드 시 해소되므로 -> 최신 API 를 하드임포트하면 옛 OS 로드 실패. 그래서 런타임에 동적 해소하고(하드임포트 금지) 32비트로 아키텍처 폭을 확보한다.
- i686 은 이 그림의 ISA 층에 놓이는 값 — IA-32(32비트 x86)의 P6 명령어 레벨일 뿐, 별개 ISA 도 라이브러리도 ABI 도 아니다. 정식 ISA 명칭은 32비트=IA-32, 64비트=x86-64, x86 은 그 패밀리.
