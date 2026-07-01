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
  CFLAGS  += -fPIE \
             -I$(CJSON_DIR) \
             -I$(RABBITMQ_C_DIR)/include \
             -I$(RABBITMQ_C_DIR)/build/include \
             -I$(CURL_DIR)/include \
             -I$(LIBARCHIVE_DIR)/libarchive \
             -I$(OPENSSL_DIR)/install/include \
             -I$(ZLIB_DIR)
  LDLIBS  := $(RABBITMQ_C_LIB) $(CJSON_LIB) $(CURL_LIB) $(LIBARCHIVE_LIB) \
             $(OPENSSL_SSL) $(OPENSSL_CRYPTO) $(ZLIB_LIB) \
             -lpthread -ldl -lrt
  # -lrt: clock_gettime on glibc < 2.17 (EL6). --as-needed drops it on modern.
  LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,--as-needed -static-libgcc
else
  CFLAGS += $(shell pkg-config --cflags $(PKGS))
  LDLIBS := $(shell pkg-config --libs $(PKGS))
endif

CFLAGS  += -Iinclude -DAGENT_VERSION=\"$(AGENT_VERSION)\"

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

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

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
	rm -f $(OBJ) $(BIN) $(LEGACY_BIN)
	rm -rf $(EMBED_DIR)

# verify — GLIBC / dyn-dep / forbidden-API ABI compliance.
#   modern (default): manylinux2014 / glibc 2.17 / CentOS 7. Forbids GLIBC_2.18+.
#   legacy          : manylinux2010 / glibc 2.12 / CentOS 6. Forbids GLIBC_2.13+.
ALLOWED_DLLS := linux-vdso libc libpthread libdl libm libresolv librt ld-linux-x86-64

VERIFY_BIN        ?= $(BIN)
VERIFY_PROFILE    ?= manylinux2014
GLIBC_FORBID_RE   ?= GLIBC_2\.(1[89]|[2-9][0-9])
GLIBC_CEILING_MSG ?= GLIBC 2.18+
FORBID_API_RE     ?=  U (getrandom|statx|memfd_create|renameat2|copy_file_range|pidfd_)

define verify_body
	@echo "[verify] $(VERIFY_BIN) — $(VERIFY_PROFILE) ABI compliance"
	@bad=$$(objdump -T $(VERIFY_BIN) 2>/dev/null | awk '/GLIBC_/ {print $$5}' \
	         | grep -E '$(GLIBC_FORBID_RE)' || true); \
	 if [ -n "$$bad" ]; then echo "[verify] FAIL: $(GLIBC_CEILING_MSG) symbols found (breaks $(VERIFY_PROFILE) ABI):"; echo "$$bad"; exit 1; fi
	@deps=$$(ldd $(VERIFY_BIN) | awk '/=>/ {print $$1} /linux-vdso/ {print $$1}' \
	         | sed -E 's/\.so.*$$//' | sort -u); \
	 for d in $$deps; do \
	   case " $(ALLOWED_DLLS) " in *" $$d "*) ;; \
	     *) echo "[verify] FAIL: unexpected dynamic dep '$$d.so'"; \
	        echo "[verify]   allowed: $(ALLOWED_DLLS)"; exit 1 ;; \
	   esac; \
	 done
	@bad=$$(nm -D $(VERIFY_BIN) 2>/dev/null | grep -E '$(FORBID_API_RE)' || true); \
	 if [ -n "$$bad" ]; then echo "[verify] FAIL: forbidden APIs ($(VERIFY_PROFILE) ceiling):"; echo "$$bad"; exit 1; fi
	@echo "[verify] OK: $(VERIFY_PROFILE) clean, dyn-dep whitelist matches, no forbidden APIs"
endef

verify: $(BIN)
	$(verify_body)

verify-legacy: $(LEGACY_BIN)
	$(MAKE) verify \
	  VERIFY_BIN=$(LEGACY_BIN) \
	  VERIFY_PROFILE=manylinux2010 \
	  GLIBC_CEILING_MSG="GLIBC 2.13+" \
	  GLIBC_FORBID_RE='GLIBC_2\.(1[3-9]|[2-9][0-9])' \
	  FORBID_API_RE=' U (getrandom|statx|memfd_create|renameat2|copy_file_range|pidfd_|secure_getenv|getauxval)'

# release — dist/assessment-agent-linux-x86_64 + SHA256SUMS (manylinux2014 host).
DIST_DIR := dist
DIST_BIN := $(DIST_DIR)/assessment-agent-linux-x86_64

release: $(BIN) verify
	@mkdir -p $(DIST_DIR)
	cp $(BIN) $(DIST_BIN)
	cd $(DIST_DIR) && sha256sum assessment-agent-linux-x86_64 > SHA256SUMS
	@echo "[release] packaged $(DIST_BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

# release-legacy — dist/assessment-agent-linux-x86_64-glibc2.12 (manylinux2010 host).
# Same sources/CFLAGS/static-link set as release; only the build HOST glibc (2.12)
# and the verify ceiling differ. Bytes are identical to $(BIN) on a 2.12 host.
LEGACY_BIN      := assessment-agent-legacy
LEGACY_DIST_BIN := $(DIST_DIR)/assessment-agent-linux-x86_64-glibc2.12

$(LEGACY_BIN): $(BIN)
	cp $(BIN) $(LEGACY_BIN)

release-legacy: $(LEGACY_BIN) verify-legacy
	@mkdir -p $(DIST_DIR)
	cp $(LEGACY_BIN) $(LEGACY_DIST_BIN)
	cd $(DIST_DIR) && sha256sum $(notdir $(LEGACY_DIST_BIN)) >> SHA256SUMS
	@echo "[release-legacy] packaged $(LEGACY_DIST_BIN)"
	@cat $(DIST_DIR)/SHA256SUMS

.PHONY: all clean \
        vendor-fetch vendor-build vendor-clean \
        vendor-build-openssl vendor-build-zlib vendor-build-cjson \
        vendor-build-rabbitmq vendor-build-curl vendor-build-libarchive \
        verify verify-legacy release release-legacy
