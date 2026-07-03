#!/usr/bin/env python3
"""wire 페이로드 하나를 schema/wire.schema.json 으로 검증한다.

사용: <payload JSON> | validate-wire.py <schema.json>
      validate-wire.py <schema.json> <payload.json>

에이전트 `emit` dry-run 출력이나 캡처 샘플을 입력으로 받아, 필드/타입/null 의미론이
계약과 일치하는지 검사한다. 실패 시 위치와 사유를 출력하고 비정상 종료한다.
"""
import json
import sys


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: validate-wire.py <schema.json> [payload.json]", file=sys.stderr)
        return 2
    try:
        from jsonschema import Draft202012Validator
    except ImportError:
        print("jsonschema 미설치: pip install 'jsonschema>=4.18'", file=sys.stderr)
        return 2

    with open(sys.argv[1], encoding="utf-8") as f:
        schema = json.load(f)
    Draft202012Validator.check_schema(schema)  # 스키마 자체 유효성

    src = open(sys.argv[2], encoding="utf-8") if len(sys.argv) > 2 else sys.stdin
    data = json.load(src)

    validator = Draft202012Validator(schema)
    errors = sorted(validator.iter_errors(data), key=lambda e: list(e.path))
    if errors:
        mt = data.get("message_type", "?") if isinstance(data, dict) else "?"
        print(f"[contract] FAIL ({mt}): {len(errors)} violation(s)", file=sys.stderr)
        for e in errors:
            loc = "/".join(str(p) for p in e.path) or "(root)"
            print(f"    {loc}: {e.message}", file=sys.stderr)
        return 1

    mt = data.get("message_type") if isinstance(data, dict) else "?"
    print(f"[contract] OK ({mt})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
