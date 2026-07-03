# assessment-agent (Windows) — MinGW-w64 static build, single i686 binary.
# One binary (assessment-agent-windows-x86.exe) covers NT5.2 (Server 2003 / XP) through
# NT10 (Server 2016/2019/2022/2025, Win10/11). NT generation is chosen at RUNTIME
# (collect.c agent_is_nt6 + GetProcAddress), not at compile time, so every NT6+ API is
# dynamically resolved and nothing NT6-only is hard-imported (verify enforces this).
#
#   make vendor-fetch                        # clone vendor deps
#   make vendor-build                        # build static libs (cmake required)
#   make release                             # build + verify + dist/ + SHA256SUMS
#
# Build host: debian:bookworm (i686-w64-mingw32, gcc 12.2, CRT runtime v10) ONLY.
# CRT v10 keeps NT5.2 startup; CRT 12+ (ubuntu latest) passes verify but drops NT5.2 startup.
# OpenSSL 1.0.2u (EOL, keeps XP/2003 TLS); win32/threads NT5.2 patch auto-applied.

CMAKE   ?= cmake
JOBS    ?= 4

# 단일 i686 바이너리: NT5.2(2003/XP)부터 NT10(2016+/Win10-11)까지 하나로 커버한다.
# 세대 분기는 컴파일 타임이 아니라 런타임(collect.c agent_is_nt6 + GetProcAddress)에서 한다.
# 빌드는 debian:bookworm(i686-w64-mingw32, CRT v10)로만 — CRT v10이 NT5.2 startup을 유지한다.
# CRT 12+(ubuntu 최신)로는 verify는 통과해도 실기 startup이 드롭되므로 반드시 bookworm.
CC                  := i686-w64-mingw32-gcc
AR                  := i686-w64-mingw32-ar
WINDRES             := i686-w64-mingw32-windres
# cross objdump(binutils-mingw-w64-i686에 항상 포함) — host objdump 부재 시 verify가
# 공허하게 통과하는 걸 막는다. verify가 이 도구로 PE import 테이블을 읽는다.
OBJDUMP             := i686-w64-mingw32-objdump
OPENSSL_TARGET      := mingw
OPENSSL_CROSS       := --cross-compile-prefix=i686-w64-mingw32-
# agent 소스는 0x0600으로 컴파일해 NT6 구조체(MIB_IF_ROW2, GAA gateway/prefix)를 선언한다.
# NT6 전용 "함수"는 하드임포트하지 않고 전부 런타임 해소하므로 NT5.2 로드는 유지된다(verify가 강제).
WINNT_VER           := 0x0600
OPENSSL_VERSION     ?= OpenSSL_1_0_2u
CURL_VERSION        ?= curl-8_10_1
RABBITMQ_C_VERSION  ?= v0.10.0
BIN                 := assessment-agent-windows-x86.exe
# bcrypt는 util.c에서 런타임 해소 -> 하드링크 금지(-lbcrypt 시 NT5.2 로드 실패).
BCRYPT_LIB          :=
# OpenSSL 1.0.2u엔 build_libs는 있으나 install_dev 타깃이 없다 -> 전체 빌드 + install_sw.
# (1.0.2 apps는 .rc가 없어 windres를 쓰지 않으므로 전체 빌드해도 이중 prefix 문제 없음)
OPENSSL_MAKE_TGT    :=
OPENSSL_INSTALL_TGT := install_sw
OPENSSL_CONFIG_OPTS := no-shared no-dso \
    -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
    -Wno-error=int-conversion -Wno-error=implicit-int -fcommon
EXTRA_SYS_LIBS      := -lgdi32 -lssp
# 벤더 라이브러리(curl/rabbitmq)는 NT5.2로 유지 — SRWLock 등 NT6 전용 API를 끌어오지 않게 한다.
VENDOR_CFLAGS       := -D_WIN32_WINNT=0x0502 -DWINVER=0x0502
ALLOWED_DLLS        := kernel32|advapi32|user32|gdi32|ws2_32|iphlpapi|crypt32|secur32|ole32|oleaut32|shell32|msvcrt|ntdll
# NT5.2 로드 실패 유발 심볼 — import 테이블에 하드링크되면 2003/XP에서 로드가 거부된다. 두 부류:
# (1) NT6+ 전용 API: collect.c/util.c에서 GetProcAddress로 런타임 해소해야 한다.
# (2) secure-CRT(strncpy_s 등): 2003 msvcrt.dll에 없다 -> nt52_compat.h 로컬 구현으로 대체해야 한다.
# verify가 둘 다 강제 차단한다.
NT6_FORBID_SYMS     := GetIfTable2|FreeMibTable|QueryFullProcessImageNameW|QueryFullProcessImageNameA|GetTickCount64|BCryptGenRandom|inet_ntop|inet_pton|InitializeSRWLock|AcquireSRWLockExclusive|AcquireSRWLockShared|ReleaseSRWLockExclusive|ReleaseSRWLockShared|InitializeConditionVariable|strncpy_s|strtok_s|_putenv_s|strcpy_s|strcat_s|sprintf_s|_vsnprintf_s|vsprintf_s|memcpy_s|wcscpy_s|wcsncpy_s|wcstok_s

CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11 \
           -D_WIN32_WINNT=$(WINNT_VER) -DWINVER=$(WINNT_VER) -DUNICODE -D_UNICODE \
           -DCURL_STATICLIB -DAMQP_STATIC
LDFLAGS ?= -static -static-libgcc -Wl,--no-insert-timestamp

# payload의 agent_version 필드값. 릴리즈는 CI가 git 태그(release.yml)에서 주입한다.
# 로컬/dev 빌드는 이 fallback. 스키마 계약 버전이 아니라 "어느 에이전트 빌드가 발행했나"의 빌드 정체성.
AGENT_VERSION ?= 0.0.0-dev

# Vendor static libraries (gitignored — built locally before agent build).
VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION ?= v0.15.0
ZLIB_VERSION       := v1.3.1
# CURL_VERSION / OPENSSL_VERSION are pinned per profile above.

CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
CURL_DIR           := $(VENDOR_DIR)/curl
OPENSSL_DIR        := $(VENDOR_DIR)/openssl
ZLIB_DIR           := $(VENDOR_DIR)/zlib

CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a
CURL_LIB           := $(CURL_DIR)/build/lib/libcurl.a
OPENSSL_SSL        := $(OPENSSL_DIR)/install/lib/libssl.a
OPENSSL_CRYPTO     := $(OPENSSL_DIR)/install/lib/libcrypto.a
ZLIB_LIB           := $(ZLIB_DIR)/libz.a

OPENSSL_TARGET     ?= mingw64
OPENSSL_CROSS      ?=
# curl >= 7.77 exposes the static lib as `libcurl_static`; older curl only `libcurl`.
CURL_STATIC_TARGET ?= libcurl_static

CFLAGS += -I$(CJSON_DIR) \
          -I$(RABBITMQ_C_DIR)/include \
          -I$(RABBITMQ_C_DIR)/build/include \
          -I$(RABBITMQ_C_DIR)/librabbitmq \
          -I$(RABBITMQ_C_DIR)/build/librabbitmq \
          -I$(CURL_DIR)/include \
          -I$(OPENSSL_DIR)/install/include \
          -I$(ZLIB_DIR) \
          -Iinclude \
          -DAGENT_VERSION=\"$(AGENT_VERSION)\"

# Incremental-build correctness. -MMD -MP emit per-object .d files (header deps)
# so a header edit recompiles its dependents. The .cflags sentinel forces a
# recompile when the compile command itself changes — notably AGENT_VERSION.
# A plain `src/%.o: src/%.c` rule tracks neither, so a stale object silently
# ships old headers/flags (this bit us: a stale main.o kept an old AGENT_VERSION).
CFLAGS += -MMD -MP
CFLAGS_SENTINEL := build/.cflags

# Static vendor archives + Windows system DLLs (ship with every supported Windows).
# bcrypt는 링크하지 않는다(BCRYPT_LIB 비움) — compat_rand_bytes가 런타임 해소한다.
# 하드링크하면 NT5.2(2003/XP)에서 로드가 실패한다.
LDLIBS := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
          -lws2_32 -liphlpapi -ladvapi32 -lkernel32 -luser32 \
          -lcrypt32 -lsecur32 $(BCRYPT_LIB) -lole32 -loleaut32 -lshell32 -lpthread $(EXTRA_SYS_LIBS)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
RES := src/resources.res

all: $(BIN)

$(BIN): $(OBJ) $(RES)
	$(CC) $(OBJ) $(RES) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c $(CFLAGS_SENTINEL)
	$(CC) $(CFLAGS) -c $< -o $@

# Rewrite the sentinel only when the effective compile command changed, so a
# changed AGENT_VERSION/CFLAGS invalidates every object without a manual clean.
$(CFLAGS_SENTINEL): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CFLAGS)' | cmp -s - $@ 2>/dev/null || printf '%s\n' '$(CC) $(CFLAGS)' > $@
FORCE:

-include $(OBJ:.o=.d)

# PE resource — embeds deploy/agent.env.example as RT_RCDATA so `install` runs
# with no external files on the target. `-I .` resolves the .rc reference against
# windows-agent/ root.
$(RES): src/resources.rc deploy/agent.env.example
	$(WINDRES) -O coff -I . -i $< -o $@

vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR)      || git clone --depth 1 --branch $(CJSON_VERSION)      https://github.com/DaveGamble/cJSON.git  $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git $(RABBITMQ_C_DIR)
	@test -d $(CURL_DIR)       || git clone --depth 1 --branch $(CURL_VERSION)       https://github.com/curl/curl.git         $(CURL_DIR)
	@test -d $(OPENSSL_DIR)    || git clone --depth 1 --branch $(OPENSSL_VERSION)    https://github.com/openssl/openssl.git   $(OPENSSL_DIR)
	@test -d $(ZLIB_DIR)       || git clone --depth 1 --branch $(ZLIB_VERSION)       https://github.com/madler/zlib.git       $(ZLIB_DIR)

vendor-build: vendor-fetch vendor-build-openssl vendor-build-zlib vendor-build-cjson vendor-build-rabbitmq vendor-build-curl

vendor-build-openssl:
	# --libdir=lib: OpenSSL 3.x may install to lib64/lib/<host>; pin to install/lib.
	cd $(OPENSSL_DIR) && ./Configure $(OPENSSL_TARGET) $(OPENSSL_CONFIG_OPTS) $(OPENSSL_CROSS) \
	    --libdir=lib --prefix=$(CURDIR)/$(OPENSSL_DIR)/install
	# 1.0.2 install_sw mkdir is not idempotent — wipe prefix first.
	rm -rf $(OPENSSL_DIR)/install
	# 라이브러리(libssl/libcrypto)만 필요 — apps(openssl.exe)는 빌드하지 않는다.
	# 기본 타깃 build_sw는 apps/openssl.rc를 RC(=$(CROSS_COMPILE)windres)로 컴파일하는데,
	# --cross-compile-prefix가 windres 이름에도 적용되어 이중 prefix
	# (x86_64-w64-mingw32-x86_64-w64-mingw32-windres)가 되어 실패한다.
	# 3.0.15엔 no-apps 옵션이 없으므로 build_libs + install_dev 로 apps 를 건너뛴다.
	$(MAKE) -C $(OPENSSL_DIR) -j$(JOBS) $(OPENSSL_MAKE_TGT)
	$(MAKE) -C $(OPENSSL_DIR) $(OPENSSL_INSTALL_TGT)

vendor-build-zlib:
	# zlib win32/Makefile.gcc defines ARFLAGS=rcs itself — pass AR command only.
	$(MAKE) -C $(ZLIB_DIR) -f win32/Makefile.gcc CC=$(CC) AR=$(AR) libz.a

vendor-build-cjson:
	$(CMAKE) -S $(CJSON_DIR) -B $(CJSON_DIR)/build \
	      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC) \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(CJSON_DIR)/build --target cjson -- -j$(JOBS)

vendor-build-rabbitmq:
	# rabbitmq-c win32/threads가 SRWLock(Vista+)을 써서 NT5.2에 undefined reference가 난다.
	# CRITICAL_SECTION 패치본으로 교체(patches/nt52-threads/). 단일 i686 빌드라 항상 적용.
	cp patches/nt52-threads/rabbitmq-threads.h $(RABBITMQ_C_DIR)/librabbitmq/win32/threads.h
	cp patches/nt52-threads/rabbitmq-threads.c $(RABBITMQ_C_DIR)/librabbitmq/win32/threads.c
	@echo "[patch] NT5.2 win32/threads applied (SRWLock -> CRITICAL_SECTION)"
	# GCC 14+ promotes -Wincompatible-pointer-types to error (rabbitmq-c v0.15.0
	# amqp_socket.c ioctlsocket mismatch) — demote until upstream fix.
	$(CMAKE) -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build \
	      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC) \
	      -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types $(VENDOR_CFLAGS)" \
	      -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	      -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
	      -DENABLE_SSL_SUPPORT=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DCMAKE_PREFIX_PATH=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_INCLUDE_DIR=$(CURDIR)/$(OPENSSL_DIR)/install/include \
	      -DOPENSSL_SSL_LIBRARY=$(CURDIR)/$(OPENSSL_DIR)/install/lib/libssl.a \
	      -DOPENSSL_CRYPTO_LIBRARY=$(CURDIR)/$(OPENSSL_DIR)/install/lib/libcrypto.a \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static -- -j$(JOBS)
	# mingw cmake emits liblibrabbitmq.<N>.a — alias to librabbitmq.a for the fixed path.
	cp $(RABBITMQ_C_DIR)/build/librabbitmq/liblibrabbitmq.*.a \
	   $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a

vendor-build-curl:
	# libcurl static, OpenSSL backend, all optional deps forced OFF (MSYS2 auto-detects
	# libpsl/libssh2/idn2/brotli/zstd/nghttp2 otherwise -> unresolved symbols).
	$(CMAKE) -S $(CURL_DIR) -B $(CURL_DIR)/build \
	      -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=$(CC) \
	      -DCMAKE_C_FLAGS="$(VENDOR_CFLAGS)" \
	      -DBUILD_SHARED_LIBS=OFF -DCURL_STATICLIB=ON \
	      -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
	      -DCURL_USE_OPENSSL=ON -DCMAKE_USE_OPENSSL=ON \
	      -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_FTP=ON \
	      -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON \
	      -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_POP3=ON \
	      -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON \
	      -DCURL_USE_LIBPSL=OFF  -DCURL_USE_LIBSSH2=OFF -DCURL_USE_LIBSSH=OFF \
	      -DCURL_USE_LIBIDN2=OFF -DCURL_USE_GSASL=OFF   -DCURL_USE_GSSAPI=OFF \
	      -DCURL_BROTLI=OFF       -DCURL_ZSTD=OFF        -DUSE_NGHTTP2=OFF \
	      -DUSE_NGTCP2=OFF        -DUSE_QUICHE=OFF       -DUSE_LIBRTMP=OFF \
	      -DCURL_DISABLE_IDN=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_LibPSL=ON  \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Libssh2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_LibSSH=ON  \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Libidn2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_LibIDN2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Brotli=ON  \
	      -DCMAKE_DISABLE_FIND_PACKAGE_Zstd=ON    \
	      -DCMAKE_DISABLE_FIND_PACKAGE_NGHTTP2=ON \
	      -DCMAKE_DISABLE_FIND_PACKAGE_nghttp2=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DZLIB_INCLUDE_DIR=$(CURDIR)/$(ZLIB_DIR) \
	      -DZLIB_LIBRARY=$(CURDIR)/$(ZLIB_LIB) \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	$(CMAKE) --build $(CURL_DIR)/build --target $(CURL_STATIC_TARGET) -- -j$(JOBS)
	@if [ ! -f $(CURL_DIR)/build/lib/libcurl.a ]; then \
		cp $$(ls $(CURL_DIR)/build/lib/lib*curl*.a | head -n1) \
		   $(CURL_DIR)/build/lib/libcurl.a ; \
	fi

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(RES) $(BIN) $(CFLAGS_SENTINEL) build-deps.txt build-forbidden.txt

# verify — (1) only Windows system DLLs in the import table, (2) no NT6+ symbol hard-imported.
# (2) is the NT5.2 load guard: a single NT6-only import makes the exe fail to load on 2003/XP.
verify: $(BIN)
	@command -v $(OBJDUMP) >/dev/null || { echo "[verify] FAIL: $(OBJDUMP) not found (install binutils-mingw-w64-i686)"; exit 1; }
	@echo "[verify] checking DLL dependencies of $(BIN)..."
	@$(OBJDUMP) -p $(BIN) | awk '/DLL Name:/ {print tolower($$3)}' | sort -u > build-deps.txt
	@cat build-deps.txt
	@if [ ! -s build-deps.txt ]; then \
		echo "[verify] FAIL: import table empty — $(OBJDUMP) could not read $(BIN) (aborting, no vacuous pass)"; \
		exit 1; \
	fi
	@if grep -vE '^($(ALLOWED_DLLS))\.dll$$' build-deps.txt; then \
		echo "[verify] FAIL: non-system DLL dependency listed above"; \
		exit 1; \
	fi
	@echo "[verify] checking for NT6+ hard-imported symbols (NT5.2 load guard)..."
	@$(OBJDUMP) -p $(BIN) | grep -woE '($(NT6_FORBID_SYMS))' | sort -u > build-forbidden.txt || true
	@if [ -s build-forbidden.txt ]; then \
		echo "[verify] FAIL: NT6+ symbols hard-imported (would fail to load on NT5.2/2003):"; \
		cat build-forbidden.txt; \
		echo "[verify] -> resolve these via GetProcAddress in collect.c/util.c, do not link them."; \
		exit 1; \
	fi
	@echo "[verify] OK: no NT6+ hard imports; only Windows system DLLs referenced"

DIST_DIR := dist

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_DIR)/
	cd $(DIST_DIR) && sha256sum $(BIN) > SHA256SUMS
	@echo "[release] packaged $(DIST_DIR)/$(BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean vendor-fetch vendor-build vendor-build-openssl vendor-build-zlib \
        vendor-build-cjson vendor-build-rabbitmq vendor-build-curl vendor-clean verify release
