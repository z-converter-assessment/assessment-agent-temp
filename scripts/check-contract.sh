#!/usr/bin/env bash
# 바이너리의 emit dry-run 출력을 wire 계약(schema/wire.schema.json)으로 검증한다.
# 실제 직렬화 코드를 태워, 필드/타입/null 의미론이 계약과 일치하는지 리눅스·윈도우
# 두 바이너리 모두에 동일 스키마로 강제한다(트리 간 드리프트 + 자기계약 위반 포착).
#
# 사용:
#   check-contract.sh <binary> [runner...]
#   네이티브:   check-contract.sh dist/assessment-agent-linux-x86_64
#   윈도우Wine: check-contract.sh dist/assessment-agent-windows-x86.exe wine
#
# Wine 은 로컬 시스템을 잘못된 값으로 비추지만, 검증 대상은 값이 아니라 페이로드의
# 구조(필드/타입/null)이므로 실제 윈도우 직렬화 경로를 태우는 것으로 충분하다.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SCHEMA="$HERE/../schema/wire.schema.json"
BIN="${1:?usage: check-contract.sh <binary> [runner...]}"; shift || true
RUNNER=("$@")   # 비면 네이티브, 아니면 앞에 붙임(예: wine)

# windows-latest 러너는 python3 대신 python 이다.
PY="${PYTHON:-python3}"; command -v "$PY" >/dev/null 2>&1 || PY=python

run() {
	if [ "${#RUNNER[@]}" -eq 0 ]; then "$BIN" "$@"; else "${RUNNER[@]}" "$BIN" "$@"; fi
}

rc=0
for kind in inventory metrics task.result error; do
	echo "[contract] ${RUNNER[*]:-native} $(basename "$BIN") emit $kind"
	if ! run emit "$kind" 2>/dev/null | "$PY" "$HERE/validate-wire.py" "$SCHEMA"; then
		rc=1
	fi
done

if [ "$rc" -eq 0 ]; then
	echo "[contract] PASS: $(basename "$BIN")"
else
	echo "[contract] FAIL: $(basename "$BIN")" >&2
fi
exit "$rc"
