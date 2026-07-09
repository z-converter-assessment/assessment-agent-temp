#!/usr/bin/env python3
# device 안정키 계층 폴백 검증 — stripe/thin LV, RAW 디스크에서 id 채워지나 + parent dm-N 정규화.
import json, os, glob, subprocess

def read(p):
    try:
        with open(p) as f: return f.read().strip()
    except Exception: return None

def sh(c):
    try: return subprocess.run(c, shell=True, capture_output=True, text=True, timeout=8).stdout
    except Exception: return None

# lsblk 전체 노드 (KNAME=커널명 dm-N, NAME=논리명, PKNAME=부모 커널명)
lj = sh("lsblk -J -b -o NAME,KNAME,TYPE,FSTYPE,MOUNTPOINT,PKNAME,UUID,PARTUUID 2>/dev/null")
nodes = []
def walk(n, pk=None):
    nodes.append(n);
    for c in n.get("children",[]) or []: walk(c, n.get("kname"))
for n in json.loads(lj).get("blockdevices",[]): walk(n)

def stable_id(n):
    """계층 폴백: dm/uuid(dm계열) -> partuuid(파티션) -> wwid/serial(디스크) -> fs uuid(리프)."""
    kname = n.get("kname"); typ = n.get("type")
    # 1) dm 계열(lvm/crypt/mpath/raid): dm/uuid = 항상 존재·안정
    dmu = read(f"/sys/block/{kname}/dm/uuid")
    if dmu: return ("dm", dmu)
    # 2) 파티션: PARTUUID
    if n.get("partuuid"): return ("partuuid", n["partuuid"])
    # 3) 디스크: wwid 또는 serial 또는 by-id
    if typ == "disk":
        wwid = read(f"/sys/block/{kname}/device/wwid") or read(f"/sys/block/{kname}/wwid")
        ser  = read(f"/sys/block/{kname}/serial") or read(f"/sys/block/{kname}/device/serial")
        if wwid: return ("wwid", wwid)
        if ser:  return ("serial", ser)
        byid = sh(f"ls -l /dev/disk/by-id/ 2>/dev/null | grep -w {kname} | head -1")
        if byid and byid.strip(): return ("by-id", byid.split()[8] if len(byid.split())>8 else byid.strip())
    # 4) fs UUID (리프 폴백)
    if n.get("uuid"): return ("fsuuid", n["uuid"])
    return (None, None)

# kname -> stable id 맵(부모 정규화용)
idmap = {}
for n in nodes:
    t, v = stable_id(n); idmap[n.get("kname")] = (t, v)

out = []
for n in nodes:
    if n.get("type") == "loop": continue
    t, v = idmap[n.get("kname")]
    pk = n.get("pkname")
    pt, pv = idmap.get(pk, (None, None)) if pk else (None, None)
    out.append({
        "name": n["name"], "kname": n.get("kname"), "type": n.get("type"),
        "mnt": n.get("mountpoint"),
        "id_type": t, "stable_id": (v[:28]+"..." if v and len(v)>31 else v),
        "parent_kname(불안정)": pk,
        "parent_stable_id": (pv[:20]+"..." if pv and len(pv)>23 else pv),
    })
print(json.dumps(out, indent=1, ensure_ascii=False))
# 요약: id 못 채운 노드
missing = [n["name"] for n in out if n["id_type"] is None]
print("\n>>> stable_id 미해결 노드:", missing if missing else "없음 (전부 해결)")
