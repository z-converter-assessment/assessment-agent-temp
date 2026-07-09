#!/usr/bin/env python3
# E축(Errors) 최대 파싱 시도 — node_exporter 관례의 canonical 에러 소스 전부. 파싱값/null 리포트.
import os, glob, json
def read(p):
    try:
        with open(p) as f: return f.read().strip()
    except Exception: return None
def readint(p):
    v=read(p)
    try: return int(v)
    except Exception: return None

out={}

# 1. mdraid (node_md_*) — degraded/mismatch/멤버 state·errors
md={}
for mdp in glob.glob("/sys/block/md*/md"):
    name=mdp.split("/")[3]
    members=[]
    for dev in glob.glob(mdp+"/dev-*"):
        members.append({"disk":os.path.basename(dev)[4:],
                        "state":read(dev+"/state"),"errors":readint(dev+"/errors")})
    md[name]={"degraded":readint(mdp+"/degraded"),
              "mismatch_cnt":readint(mdp+"/mismatch_cnt"),
              "sync_action":read(mdp+"/sync_action"),
              "raid_level":read(mdp+"/level"),
              "members":members}
out["disk.mdraid"]=md or None

# 2. SCSI ioerr_cnt (vioscsi/scsi 디스크)
ioerr={}
for p in glob.glob("/sys/block/*/device/ioerr_cnt"):
    dev=p.split("/")[3]; ioerr[dev]=read(p)
out["disk.ioerr_cnt"]=ioerr or None

# 3. net /proc/net/dev — errs/drop/fifo/frame (node_network_*_total)
net={}
for line in (read("/proc/net/dev") or "").splitlines():
    if ":" not in line: continue
    d,_,r=line.partition(":"); d=d.strip(); f=r.split()
    if d=="lo" or len(f)<16: continue
    # rx: bytes packets errs drop fifo frame compressed multicast | tx: bytes packets errs drop fifo colls carrier compressed
    net[d]={"rx_errs":int(f[2]),"rx_drop":int(f[3]),"rx_fifo":int(f[4]),"rx_frame":int(f[5]),
            "tx_errs":int(f[10]),"tx_drop":int(f[11]),"tx_fifo":int(f[12]),"tx_colls":int(f[13]),"tx_carrier":int(f[14])}
out["network.iface_errors"]=net or None

# 4. tcp 재전송/에러 (node_netstat_Tcp_*)
tcp={}
lines=(read("/proc/net/snmp") or "").splitlines()
for i in range(len(lines)-1):
    if lines[i].startswith("Tcp:") and "RetransSegs" in lines[i]:
        h=lines[i].split(); v=lines[i+1].split()
        for k in ("RetransSegs","InErrs","OutSegs","InSegs"):
            if k in h: tcp[k]=int(v[h.index(k)])
# TcpExt (세부 재전송)
lines=(read("/proc/net/netstat") or "").splitlines()
for i in range(len(lines)-1):
    if lines[i].startswith("TcpExt:") and "TCPLostRetransmit" in lines[i]:
        h=lines[i].split(); v=lines[i+1].split()
        for k in ("TCPLostRetransmit","TCPSynRetrans","TCPFastRetrans"):
            if k in h: tcp[k]=int(v[h.index(k)])
out["network.tcp"]=tcp or None

# 5. conntrack (node_nf_conntrack_entries)
out["network.conntrack"]={"count":readint("/proc/sys/net/netfilter/nf_conntrack_count"),
                          "max":readint("/proc/sys/net/netfilter/nf_conntrack_max")}

# 6. memory HardwareCorrupted (poisoned pages, VM에도 존재) + EDAC
hc=None
for line in (read("/proc/meminfo") or "").splitlines():
    if line.startswith("HardwareCorrupted:"): hc=int(line.split()[1])*1024
edac={}
for mc in glob.glob("/sys/devices/system/edac/mc/mc*"):
    edac[os.path.basename(mc)]={"ce":readint(mc+"/ce_count"),"ue":readint(mc+"/ue_count")}
out["memory.errors"]={"hardware_corrupted_bytes":hc,"edac":edac or None}

# 7. CPU MCE (machinecheck 존재? mcelog?)
mce={"machinecheck_banks":len(glob.glob("/sys/devices/system/machinecheck/machinecheck*")),
     "mcelog_dev":os.path.exists("/dev/mcelog")}
out["cpu.mce"]=mce

# 리포트: 파싱됨(non-null·비어있지않음) vs null
def status(v):
    if v is None: return "NULL"
    if isinstance(v,dict) and not v: return "empty"
    return "OK"
print(json.dumps(out, indent=1, ensure_ascii=False))
print("\n=== E축 소스 파싱 상태 ===")
for k,v in out.items(): print(f"  {k:26} {status(v)}")
