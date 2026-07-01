# assessment-agent (Windows) — MinGW-w64 static build.
# Build host: MSYS2 + mingw-w64 (native) or an x86_64/i686-w64-mingw32 cross toolchain.
#   make vendor-fetch                        # clone vendor deps
#   make vendor-build                        # build static libs (cmake required)
#   make [PROFILE=...] release               # build + verify + dist/ + SHA256SUMS
#
# Profiles (NT generation from one source tree):
#   modern   (default) : NT 10.0 x64 — Server 2016/2019/2022/2025, Win10/11. OpenSSL 3.x.
#   win7               : NT 6.1  x64 — Server 2008R2/2012/2012R2, Win7/8/8.1. OpenSSL 3.x.
#   legacy32           : NT 5.2  i686 — Server 2003 / XP (32-bit). OpenSSL 1.0.2u, no bcrypt.
#                        Needs an OLDER MinGW-w64 (i686) that still emits NT 5.2 startup code
#                        + the win32/threads NT5.2 patch (see agent-fleet/win-legacy-patch).
# The C code is identical across profiles: GetTickCount64 / BCryptGenRandom are resolved
# at runtime (monotonic_ms / compat_rand_bytes), so a lower WINNT_VER leaves no unresolved import.

CC      ?= gcc
AR      ?= ar
CMAKE   ?= cmake
WINDRES ?= windres
JOBS    ?= 4
PROFILE ?= modern

ifeq ($(PROFILE),legacy32)
  CC                  := i686-w64-mingw32-gcc
  AR                  := i686-w64-mingw32-ar
  WINDRES             := i686-w64-mingw32-windres
  OPENSSL_TARGET      := mingw
  OPENSSL_CROSS       := --cross-compile-prefix=i686-w64-mingw32-
  WINNT_VER           := 0x0502
  OPENSSL_VERSION     ?= OpenSSL_1_0_2u
  CURL_VERSION        ?= curl-8_10_1
  RABBITMQ_C_VERSION  ?= v0.10.0
  BIN                 := assessment-agent-win2003-x86.exe
  BCRYPT_LIB          :=
  OPENSSL_CONFIG_OPTS := no-shared no-dso \
      -Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration \
      -Wno-error=int-conversion -Wno-error=implicit-int -fcommon
  EXTRA_SYS_LIBS      := -lgdi32 -lssp
  VENDOR_CFLAGS       := -D_WIN32_WINNT=0x0502 -DWINVER=0x0502
  ALLOWED_DLLS        := kernel32|advapi32|user32|gdi32|ws2_32|iphlpapi|crypt32|secur32|ole32|oleaut32|shell32|msvcrt|ntdll
else ifeq ($(PROFILE),win7)
  WINNT_VER           := 0x0601
  OPENSSL_VERSION     ?= openssl-3.0.15
  CURL_VERSION        ?= curl-8_10_1
  BIN                 := assessment-agent-win2008r2-x64.exe
  BCRYPT_LIB          := -lbcrypt
  OPENSSL_CONFIG_OPTS := no-shared no-dso no-engine no-tests
  ALLOWED_DLLS        := kernel32|advapi32|user32|ws2_32|iphlpapi|crypt32|secur32|bcrypt|ole32|oleaut32|shell32|msvcrt|ntdll|api-ms-win-.+
else
  WINNT_VER           := 0x0A00
  OPENSSL_VERSION     ?= openssl-3.0.15
  CURL_VERSION        ?= curl-8_10_1
  BIN                 := assessment-agent-win2016-x64.exe
  BCRYPT_LIB          := -lbcrypt
  OPENSSL_CONFIG_OPTS := no-shared no-dso no-engine no-tests
  ALLOWED_DLLS        := kernel32|advapi32|user32|ws2_32|iphlpapi|crypt32|secur32|bcrypt|ole32|oleaut32|shell32|msvcrt|ntdll|api-ms-win-.+
endif

EXTRA_SYS_LIBS ?=
VENDOR_CFLAGS  ?=

CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11 \
           -D_WIN32_WINNT=$(WINNT_VER) -DWINVER=$(WINNT_VER) -DUNICODE -D_UNICODE \
           -DCURL_STATICLIB -DAMQP_STATIC
LDFLAGS ?= -static -static-libgcc -Wl,--no-insert-timestamp

AGENT_VERSION ?= 1.0.0

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

# Static vendor archives + Windows system DLLs (ship with every supported Windows).
# bcrypt: modern/win7 only (resolved at runtime via compat_rand_bytes, so legacy32 drops it).
LDLIBS := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
          -lws2_32 -liphlpapi -ladvapi32 -lkernel32 -luser32 \
          -lcrypt32 -lsecur32 $(BCRYPT_LIB) -lole32 -loleaut32 -lshell32 $(EXTRA_SYS_LIBS)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
RES := src/resources.res

all: $(BIN)

$(BIN): $(OBJ) $(RES)
	$(CC) $(OBJ) $(RES) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

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
	$(MAKE) -C $(OPENSSL_DIR) -j$(JOBS)
	$(MAKE) -C $(OPENSSL_DIR) install_sw

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
	rm -f $(OBJ) $(RES) $(BIN) build-deps.txt

# verify — only Windows system DLLs allowed in the import table.
verify: $(BIN)
	@echo "[verify] checking DLL dependencies of $(BIN)..."
	@objdump -p $(BIN) | awk '/DLL Name:/ {print tolower($$3)}' | sort -u > build-deps.txt
	@cat build-deps.txt
	@if grep -vE '^($(ALLOWED_DLLS))\.dll$$' build-deps.txt; then \
		echo "[verify] FAIL: non-system DLL dependency listed above (profile: $(PROFILE))"; \
		exit 1; \
	fi
	@echo "[verify] OK: only Windows system DLLs referenced (profile: $(PROFILE))"

DIST_DIR := dist

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_DIR)/
	cd $(DIST_DIR) && sha256sum $(BIN) > SHA256SUMS
	@echo "[release] packaged $(DIST_DIR)/$(BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean vendor-fetch vendor-build vendor-build-openssl vendor-build-zlib \
        vendor-build-cjson vendor-build-rabbitmq vendor-build-curl vendor-clean verify release
