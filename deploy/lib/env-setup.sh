#!/bin/sh
# env-setup.sh — POSIX-sh idempotent env-file populator.
#
# Usage: env-setup.sh <example_file> <target_env_file> <local_secret_file>
#
# Rules (mirrored by env_setup_run in windows-agent/src/installer.c):
#   - canonical key list + suggested defaults come from <example_file>
#   - <target_env_file> preserves any KEY=value with non-empty value
#     (NEVER overwrite — operator's value wins).
#   - only $PROMPT_KEYS are interactively prompted when missing. Other
#     non-secret keys silently fall back to the <example_file> default
#     — most operators run the agent on standard topology, so asking
#     20+ questions for values they'd never change is friction.
#   - secret keys (RABBITMQ_PASS, RABBITMQ_WORKER_PASS) are written to
#     <local_secret_file> with chmod 0600, entered via stty -echo so they
#     do not appear on screen.
#   - atomic writes (tmp + install -m mode TARGET)
#
# Re-running this script after a single key is missing only prompts for
# that one key. Designed for the "server is hard to ssh into, run it from
# a shell script" deployment model.

set -eu

EXAMPLE="${1:?usage: env-setup.sh EXAMPLE TARGET LOCAL}"
TARGET="${2:?usage: env-setup.sh EXAMPLE TARGET LOCAL}"
LOCAL="${3:?usage: env-setup.sh EXAMPLE TARGET LOCAL}"

# Keys we *do* interactively prompt for. Everything else in agent.env.example
# silently takes the example default. Keep this list tight — every entry is
# friction at install time.
#
# RABBITMQ_HOST                  broker address (always site-specific)
# WORKER_DOWNLOAD_ALLOWED_HOSTS  task.install whitelist — 사실상 고정이라 프롬프트하지 않고 agent.env.example
#                                기본값을 그대로 쓴다(broker host/creds 와 동일 방식: env 는 config, 소스엔 안 박음).
#                                바꿀 땐 agent.env 로 override.
PROMPT_KEYS="RABBITMQ_HOST"

SECRET_KEYS="RABBITMQ_PASS RABBITMQ_WORKER_PASS"

is_prompt() {
	case " $PROMPT_KEYS " in
		*" $1 "*) return 0 ;;
		*)        return 1 ;;
	esac
}

is_secret() {
	case " $SECRET_KEYS " in
		*" $1 "*) return 0 ;;
		*)        return 1 ;;
	esac
}

# getval FILE KEY → prints value (no surrounding quotes) or empty.
getval() {
	[ -f "$1" ] || return 0
	awk -F= -v key="$2" '
		function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t\r]+$/, "", s); return s }
		/^[ \t]*#/ { next }
		/^[ \t]*$/ { next }
		{
			line = $0
			idx = index(line, "=")
			if (idx == 0) next
			k = trim(substr(line, 1, idx - 1))
			if (k != key) next
			v = trim(substr(line, idx + 1))
			# strip surrounding quotes (single or double)
			if (length(v) >= 2) {
				c = substr(v, 1, 1)
				if ((c == "\"" || c == "'\''") && substr(v, length(v), 1) == c) {
					v = substr(v, 2, length(v) - 2)
				}
			}
			print v
			exit
		}
	' "$1" 2>/dev/null || true
}

prompt() {
	# Non-interactive (high-latency SSH / no copy-paste): a value preset in the
	# environment wins outright — `RABBITMQ_HOST=broker ./install.sh` needs zero
	# round-trips.
	eval "preset=\${$1:-}"
	if [ -n "$preset" ]; then
		printf '%s' "$preset"
		return
	fi
	# No TTY (piped install / SSH bare exec): take the default, never block.
	if [ ! -t 0 ]; then
		printf '%s' "$2"
		return
	fi
	if [ -n "$2" ]; then
		printf "%s [%s]: " "$1" "$2" >&2
	else
		printf "%s: " "$1" >&2
	fi
	IFS= read -r ans || ans=""
	[ -z "$ans" ] && ans="$2"
	printf '%s' "$ans"
}

prompt_secret() {
	# Preset secret in the environment → use it, no prompt.
	eval "preset=\${$1:-}"
	if [ -n "$preset" ]; then
		printf '%s' "$preset"
		return
	fi
	# No TTY and no preset → cannot capture a secret without blocking; leave
	# empty so the install completes. Operator fills agent.env.local later.
	if [ ! -t 0 ]; then
		printf ''
		return
	fi
	printf "%s (입력 시 화면에 표시되지 않습니다): " "$1" >&2
	# stty -echo on a tty hides input.
	stty -echo 2>/dev/null || true
	IFS= read -r ans || ans=""
	stty echo 2>/dev/null || true
	printf "\n" >&2
	printf '%s' "$ans"
}

TMP=$(mktemp)
TMP_LOCAL=$(mktemp)
trap 'rm -f "$TMP" "$TMP_LOCAL"' EXIT INT TERM

# Replay EXAMPLE structure (preserves comments + key order)
while IFS= read -r line || [ -n "$line" ]; do
	# strip \r for Windows-shared env files
	line=$(printf '%s' "$line" | tr -d '\r')
	stripped=$(printf '%s' "$line" | sed 's/^[ \t]*//')
	case "$stripped" in
		''|\#*)
			printf '%s\n' "$line" >> "$TMP"
			continue
			;;
	esac

	key=$(printf '%s' "$line" | awk -F= '
		{ k = $1; sub(/^[ \t]+/, "", k); sub(/[ \t]+$/, "", k); print k }
	')
	if [ -z "$key" ]; then
		printf '%s\n' "$line" >> "$TMP"
		continue
	fi
	if is_secret "$key"; then
		printf '# %s  -- managed in agent.env.local by env-setup.sh\n' "$key" >> "$TMP"
		continue
	fi

	existing=$(getval "$TARGET" "$key" || true)
	default=$(getval "$EXAMPLE" "$key" || true)
	if [ -n "$existing" ]; then
		printf '%s=%s\n' "$key" "$existing" >> "$TMP"
		continue
	fi

	if is_prompt "$key"; then
		entered=$(prompt "$key" "$default")
		printf '%s=%s\n' "$key" "$entered" >> "$TMP"
	else
		# Silent default — operator can still edit /etc/assessment-agent/agent.env
		# after install if a non-standard value is required.
		printf '%s=%s\n' "$key" "$default" >> "$TMP"
	fi
done < "$EXAMPLE"

# Atomic install + mode. The agent runs as root, so config is owned root:root.
# 0640 (not world-readable): non-secret config, but no reason to expose it.
if [ "$(id -u)" -eq 0 ]; then
	install -o root -g root -m 0640 "$TMP" "$TARGET"
else
	install -m 0640 "$TMP" "$TARGET"
fi
printf '[env-setup] wrote %s\n' "$TARGET" >&2

# Secret pass — write only if at least one secret captured / present.
wrote_secrets=0
for key in $SECRET_KEYS; do
	existing=$(getval "$LOCAL" "$key" || true)
	if [ -n "$existing" ]; then
		printf '%s=%s\n' "$key" "$existing" >> "$TMP_LOCAL"
		wrote_secrets=1
		continue
	fi
	val=$(prompt_secret "$key")
	if [ -n "$val" ]; then
		printf '%s=%s\n' "$key" "$val" >> "$TMP_LOCAL"
		wrote_secrets=1
	fi
done
if [ "$wrote_secrets" = "1" ]; then
	# Secrets (RABBITMQ_PASS/…) — the agent runs as root and reads them directly,
	# so 0600 root:root is enough and tightest.
	if [ "$(id -u)" -eq 0 ]; then
		install -o root -g root -m 0600 "$TMP_LOCAL" "$LOCAL"
	else
		install -m 0600 "$TMP_LOCAL" "$LOCAL"
	fi
	printf '[env-setup] wrote %s (mode 0600)\n' "$LOCAL" >&2
fi
