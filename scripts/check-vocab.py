#!/usr/bin/env python3
"""wire 페이로드의 metric 이름 + attr 키가 어휘 정본(schema/metric-vocab.json)의
부분집합인지 검증한다.

사용: <payload JSON> | check-vocab.py <metric-vocab.json>
      check-vocab.py <metric-vocab.json> <payload.json>

wire.schema.json 이 구조(네임스페이스/타입/단위/null)를 강제하는 것과 별개로, 이 검사는
producer 이름 드리프트를 잡는다: metric 이름이 소비자 키와 한 글자라도 어긋나거나 attr 키에
오타가 나면 스키마 검증은 통과하지만 소비자가 조용히 드롭한다. emit 출력의 system.* 를
훑어 (1)미지 네임스페이스 (2)그 네임스페이스에 없는 metric 이름 (3)어휘 밖 attr 키를 잡아낸다.

부분집합만 강제한다 — OS/커널 조건부로 어휘 중 일부가 미발행되는 것은 정상이다. 검사 대상은
metrics/inventory 뿐이고(system.* 를 싣는 메시지), task.result/error 는 통과한다.

출력은 ASCII 영어로 고정한다. windows-latest 러너의 Python stdout 이 cp1252 라, 비ASCII
문자를 print 하면 UnicodeEncodeError 로 CI 가 죽는다(로직 무관 크래시). 방어로 stdout/stderr
도 UTF-8 로 재설정한다.
"""
import json
import sys

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check-vocab.py <metric-vocab.json> [payload.json]", file=sys.stderr)
        return 2

    with open(sys.argv[1], encoding="utf-8") as f:
        vocab = json.load(f)
    ns_vocab = vocab.get("namespaces", {})
    attr_vocab = set(vocab.get("attr_keys", []))

    src = open(sys.argv[2], encoding="utf-8") if len(sys.argv) > 2 else sys.stdin
    data = json.load(src)

    mt = data.get("message_type") if isinstance(data, dict) else None
    if mt not in ("metrics", "inventory"):
        print(f"[vocab] OK ({mt}): no system.* namespace, not applicable")
        return 0

    violations = []
    for key, metrics in data.items():
        if not key.startswith("system."):
            continue
        if metrics is None:
            continue  # 네임스페이스 null (측정 불가) 은 정상
        if key not in ns_vocab:
            violations.append(f"{key}: unknown namespace (not in vocab)")
            continue
        allowed = set(ns_vocab[key])
        if not isinstance(metrics, dict):
            continue
        for mname, metric in metrics.items():
            if mname not in allowed:
                violations.append(f"{key}/{mname}: metric name not in vocab (drift?)")
            if not isinstance(metric, dict):
                continue
            for pt in metric.get("points", []) or []:
                for ak in (pt.get("attr") or {}):
                    if ak not in attr_vocab:
                        violations.append(f"{key}/{mname}: attr key not in vocab: '{ak}'")

    if violations:
        print(f"[vocab] FAIL ({mt}): {len(violations)} violation(s)", file=sys.stderr)
        for v in sorted(set(violations)):
            print(f"    {v}", file=sys.stderr)
        return 1

    print(f"[vocab] OK ({mt})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
