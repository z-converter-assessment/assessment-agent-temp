CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -Wpedantic -O2 -std=c11
LDFLAGS ?=
# payload의 agent_version 필드값. 릴리즈는 CI가 git 태그(release.yml)에서 주입한다.
# 로컬/dev 빌드는 이 fallback. 스키마 계약 버전이 아니라 "어느 에이전트 빌드가 발행했나"의 빌드 정체성.
AGENT_VERSION ?= 0.0.0-dev

# USE_VENDORED=1 -> static link against vendor/ (release). default: pkg-config (dev).
PKGS               := librabbitmq libcjson libcurl libarchive

VENDOR_DIR         := vendor
CJSON_VERSION      := v1.7.18
RABBITMQ_C_VERSION := v0.15.0
CURL_VERSION       := curl-8_10_1
LIBARCHIVE_VERSION := v3.7.7
OPENSSL_VERSION    := openssl-3.0.15
ZLIB_VERSION       := v1.3.1

CJSON_DIR          := $(VENDOR_DIR)/cJSON
RABBITMQ_C_DIR     := $(VENDOR_DIR)/rabbitmq-c
CURL_DIR           := $(VENDOR_DIR)/curl
LIBARCHIVE_DIR     := $(VENDOR_DIR)/libarchive
OPENSSL_DIR        := $(VENDOR_DIR)/openssl
ZLIB_DIR           := $(VENDOR_DIR)/zlib

CJSON_LIB          := $(CJSON_DIR)/build/libcjson.a
RABBITMQ_C_LIB     := $(RABBITMQ_C_DIR)/build/librabbitmq/librabbitmq.a
CURL_LIB           := $(CURL_DIR)/build/lib/libcurl.a
LIBARCHIVE_LIB     := $(LIBARCHIVE_DIR)/build/libarchive/libarchive.a
OPENSSL_SSL        := $(OPENSSL_DIR)/install/lib/libssl.a
OPENSSL_CRYPTO     := $(OPENSSL_DIR)/install/lib/libcrypto.a
ZLIB_LIB           := $(ZLIB_DIR)/libz.a

ifeq ($(USE_VENDORED),1)
  CFLAGS  += -I$(CJSON_DIR) \
             -I$(RABBITMQ_C_DIR)/include \
             -I$(RABBITMQ_C_DIR)/build/include \
             -I$(CURL_DIR)/include \
             -I$(LIBARCHIVE_DIR)/libarchive \
             -I$(OPENSSL_DIR)/install/include \
             -I$(ZLIB_DIR)
  LDLIBS  := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(LIBARCHIVE_LIB) \
             $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
             -lpthread -ldl -lrt
  # musl fully-static release link. musl absorbs libpthread/libdl/librt into
  # libc.a, so those -l flags resolve to stubs. -static links musl itself into
  # the binary — no dynamic libc, glibc-version independent. Runs on any x86_64
  # Linux with kernel >= 2.6.32 (verified on EL6/2.6.32 and SLES11/3.0.13),
  # which covers every Glance image in one binary.
  LDFLAGS += -static -static-libgcc -Wl,-z,relro,-z,now
else
  CFLAGS += $(shell pkg-config --cflags $(PKGS))
  LDLIBS := $(shell pkg-config --libs $(PKGS))
endif

CFLAGS  += -Iinclude -DAGENT_VERSION=\"$(AGENT_VERSION)\"

# Incremental-build correctness. -MMD -MP emit a per-object .d file recording the
# headers each object includes, so a header edit recompiles its dependents. The
# .cflags sentinel (below) forces a recompile when the compile command itself
# changes — e.g. AGENT_VERSION. A plain `src/%.o: src/%.c` rule tracks neither,
# so a stale object silently ships old headers/flags (a class of "phantom" bugs).
CFLAGS  += -MMD -MP
CFLAGS_SENTINEL := build/.cflags

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
BIN := assessment-agent

# Embedded sh / systemd unit / env example via `ld -r -b binary`. Inputs are
# staged into build/embed/ with flat names so symbols are `_binary_<name>_start`,
# then objcopy moves the bytes from .data to .rodata.
LD       ?= ld
OBJCOPY  ?= objcopy
EMBED_DIR := build/embed
EMBED_OBJ := $(EMBED_DIR)/embed.o

$(EMBED_DIR)/install.sh:               deploy/install.sh
$(EMBED_DIR)/uninstall.sh:             deploy/uninstall.sh
$(EMBED_DIR)/image-prep.sh:            scripts/image-prep.sh
$(EMBED_DIR)/detect-os.sh:             deploy/lib/detect-os.sh
$(EMBED_DIR)/env-setup.sh:             deploy/lib/env-setup.sh
$(EMBED_DIR)/assessment-agent.service: deploy/systemd/assessment-agent.service
$(EMBED_DIR)/assessment-agent.sysv:    deploy/sysv/assessment-agent
$(EMBED_DIR)/agent.env.example:        deploy/systemd/agent.env.example

$(EMBED_DIR):
	mkdir -p $@

$(EMBED_DIR)/%: | $(EMBED_DIR)
	cp $< $@

EMBED_STAGED := \
    $(EMBED_DIR)/install.sh \
    $(EMBED_DIR)/uninstall.sh \
    $(EMBED_DIR)/image-prep.sh \
    $(EMBED_DIR)/detect-os.sh \
    $(EMBED_DIR)/env-setup.sh \
    $(EMBED_DIR)/assessment-agent.service \
    $(EMBED_DIR)/assessment-agent.sysv \
    $(EMBED_DIR)/agent.env.example

$(EMBED_OBJ): $(EMBED_STAGED)
	cd $(EMBED_DIR) && $(LD) -r -b binary -o embed.raw.o $(notdir $(EMBED_STAGED))
	$(OBJCOPY) --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	    $(EMBED_DIR)/embed.raw.o $@
	rm -f $(EMBED_DIR)/embed.raw.o

all: $(BIN)

$(BIN): $(OBJ) $(EMBED_OBJ)
	$(CC) $(OBJ) $(EMBED_OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

src/%.o: src/%.c $(CFLAGS_SENTINEL)
	$(CC) $(CFLAGS) -c $< -o $@

# Rewrite the sentinel only when the effective compile command changed, so a
# changed AGENT_VERSION/CFLAGS invalidates every object without a manual clean.
$(CFLAGS_SENTINEL): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(CFLAGS)' | cmp -s - $@ 2>/dev/null || printf '%s\n' '$(CC) $(CFLAGS)' > $@
FORCE:

-include $(OBJ:.o=.d)

# Vendoring — fetch deps into vendor/, build static libs.
#   ./scripts/build-linux.sh                        # containerized, one shot
#   sudo bash scripts/build-prep.sh                 # native host prereqs
#   make vendor-fetch && make vendor-build && make USE_VENDORED=1 release
# Release artifacts must come from native amd64 Linux. Build order: OpenSSL +
# zlib first (curl/libarchive/rabbitmq-c link against them).
vendor-fetch:
	@mkdir -p $(VENDOR_DIR)
	@test -d $(CJSON_DIR)      || git clone --depth 1 --branch $(CJSON_VERSION)      https://github.com/DaveGamble/cJSON.git       $(CJSON_DIR)
	@test -d $(RABBITMQ_C_DIR) || git clone --depth 1 --branch $(RABBITMQ_C_VERSION) https://github.com/alanxz/rabbitmq-c.git      $(RABBITMQ_C_DIR)
	@test -d $(CURL_DIR)       || git clone --depth 1 --branch $(CURL_VERSION)       https://github.com/curl/curl.git              $(CURL_DIR)
	@test -d $(LIBARCHIVE_DIR) || git clone --depth 1 --branch $(LIBARCHIVE_VERSION) https://github.com/libarchive/libarchive.git  $(LIBARCHIVE_DIR)
	@test -d $(OPENSSL_DIR)    || git clone --depth 1 --branch $(OPENSSL_VERSION)    https://github.com/openssl/openssl.git        $(OPENSSL_DIR)
	@test -d $(ZLIB_DIR)       || git clone --depth 1 --branch $(ZLIB_VERSION)       https://github.com/madler/zlib.git            $(ZLIB_DIR)

vendor-build: vendor-fetch \
              vendor-build-openssl vendor-build-zlib \
              vendor-build-cjson   vendor-build-rabbitmq \
              vendor-build-curl    vendor-build-libarchive

vendor-build-openssl:
	cd $(OPENSSL_DIR) && ./Configure linux-x86_64 no-shared no-dso no-engine no-tests \
	    --libdir=lib -fPIC --prefix=$(CURDIR)/$(OPENSSL_DIR)/install
	$(MAKE) -C $(OPENSSL_DIR)
	$(MAKE) -C $(OPENSSL_DIR) install_sw

vendor-build-zlib:
	cd $(ZLIB_DIR) && CFLAGS=-fPIC ./configure --static
	$(MAKE) -C $(ZLIB_DIR) libz.a

vendor-build-cjson:
	cmake -S $(CJSON_DIR) -B $(CJSON_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_CJSON_TEST=OFF \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(CJSON_DIR)/build --target cjson

vendor-build-rabbitmq:
	cmake -S $(RABBITMQ_C_DIR) -B $(RABBITMQ_C_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
	      -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF \
	      -DENABLE_SSL_SUPPORT=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(RABBITMQ_C_DIR)/build --target rabbitmq-static

vendor-build-curl:
	cmake -S $(CURL_DIR) -B $(CURL_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DCURL_STATICLIB=ON \
	      -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
	      -DCURL_USE_OPENSSL=ON -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_FTP=ON \
	      -DCURL_DISABLE_FILE=ON -DCURL_DISABLE_DICT=ON -DCURL_DISABLE_TELNET=ON \
	      -DCURL_DISABLE_TFTP=ON -DCURL_DISABLE_RTSP=ON -DCURL_DISABLE_POP3=ON \
	      -DCURL_DISABLE_IMAP=ON -DCURL_DISABLE_SMTP=ON -DCURL_DISABLE_GOPHER=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DZLIB_INCLUDE_DIR=$(CURDIR)/$(ZLIB_DIR) \
	      -DZLIB_LIBRARY=$(CURDIR)/$(ZLIB_LIB) \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(CURL_DIR)/build --target libcurl_static

vendor-build-libarchive:
	cmake -S $(LIBARCHIVE_DIR) -B $(LIBARCHIVE_DIR)/build \
	      -DBUILD_SHARED_LIBS=OFF -DENABLE_TEST=OFF \
	      -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF \
	      -DENABLE_OPENSSL=ON \
	      -DOPENSSL_ROOT_DIR=$(CURDIR)/$(OPENSSL_DIR)/install \
	      -DOPENSSL_USE_STATIC_LIBS=TRUE \
	      -DZLIB_INCLUDE_DIR=$(CURDIR)/$(ZLIB_DIR) \
	      -DZLIB_LIBRARY=$(CURDIR)/$(ZLIB_LIB) \
	      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
	cmake --build $(LIBARCHIVE_DIR)/build --target archive_static

vendor-clean:
	rm -rf $(VENDOR_DIR)

clean:
	rm -f $(OBJ) $(OBJ:.o=.d) $(BIN) $(CFLAGS_SENTINEL)
	rm -rf $(EMBED_DIR)

# verify — musl fully-static compliance. There is no glibc ABI ceiling to
# enforce (musl is linked in statically); instead assert the binary is truly
# static, carries no dynamic interpreter, and pulls in zero GLIBC-versioned
# symbols. A binary that passes this runs on any x86_64 Linux kernel >= 2.6.32
# regardless of the host libc.
verify: $(BIN)
	@echo "[verify] $(BIN) — musl fully-static"
	@file $(BIN) | grep -q "statically linked" \
	  || { echo "[verify] FAIL: not statically linked"; file $(BIN); exit 1; }
	@if readelf -l $(BIN) 2>/dev/null | grep -q INTERP; then \
	  echo "[verify] FAIL: has PT_INTERP (dynamic executable)"; exit 1; fi
	@n=$$(objdump -T $(BIN) 2>/dev/null | grep -c GLIBC || true); \
	 if [ "$$n" != "0" ]; then \
	   echo "[verify] FAIL: $$n GLIBC-versioned symbol(s) — not glibc-free"; \
	   objdump -T $(BIN) | grep GLIBC | head; exit 1; fi
	@echo "[verify] OK: fully-static, no interpreter, zero GLIBC symbols"

# release — dist/assessment-agent-linux-x86_64 (musl fully-static, single
# binary covering all supported x86_64 Linux). Built in an Alpine/musl
# container by scripts/build-linux.sh.
DIST_DIR := dist
DIST_BIN := $(DIST_DIR)/assessment-agent-linux-x86_64

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_BIN)
	cd $(DIST_DIR) && sha256sum assessment-agent-linux-x86_64 > SHA256SUMS
	@echo "[release] packaged $(DIST_BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean \
        vendor-fetch vendor-build vendor-clean \
        vendor-build-openssl vendor-build-zlib vendor-build-cjson \
        vendor-build-rabbitmq vendor-build-curl vendor-build-libarchive \
        verify release
