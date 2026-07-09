#!/usr/bin/env python3
# 시계열 정합성 검증(Linux): 부하 하 t0/t1 델타 -> 파생값(util/await/throughput/cpu-util) -> 불변식 PASS/FAIL.
# 엔진 없이 counter=delta 로직을 직접 검증. base 단위(seconds/bytes/ratio).
import time, os, glob, subprocess, json

def read(p):
    try:
        with open(p) as f: return f.read()
    except Exception: return ""

CLK=os.sysconf('SC_CLK_TCK')

def snap():
    s={"t":time.time()}
    # cpu.time (aggregate cpu line) per state -> seconds
    for line in read("/proc/stat").splitlines():
        f=line.split()
        if f and f[0]=="cpu":
            s["cpu"]={k:int(f[i+1])/CLK for i,k in enumerate(
                ["user","nice","system","idle","iowait","irq","softirq","steal"]) if i+1<len(f)}
    s["ncpu"]=sum(1 for l in read("/proc/stat").splitlines() if l.startswith("cpu") and l.split()[0]!="cpu")
    # diskstats: io_time(f10 ms), op_time r/w(f4/f8 ms), ops r/w(f1/f5), bytes r/w(f3/f7 *512)
    s["disk"]={}
    for line in read("/proc/diskstats").splitlines():
        f=line.split()
        if len(f)<14: continue
        name=f[2]
        if not (name.startswith("vd") or name.startswith("sd") or name.startswith("dm-")): continue
        if name[-1].isdigit() and name.startswith(("vd","sd")): continue  # 파티션 제외, 디스크만
        # diskstats: f3 rd_ios f4 rd_merge f5 rd_sec f6 rd_ms | f7 wr_ios f8 wr_merge f9 wr_sec f10 wr_ms | f12 io_ticks
        s["disk"][name]={"io_time":int(f[12])/1000.0,"rt":int(f[6])/1000.0,"wt":int(f[10])/1000.0,
                         "rops":int(f[3]),"wops":int(f[7]),"rby":int(f[5])*512,"wby":int(f[9])*512}
    # net
    s["net"]={}
    for line in read("/proc/net/dev").splitlines():
        if ":" not in line: continue
        d,_,r=line.partition(":"); d=d.strip(); f=r.split()
        if d=="lo" or len(f)<16: continue
        s["net"][d]={"rx":int(f[0]),"tx":int(f[8])}
    # tcp retrans
    l=read("/proc/net/snmp").splitlines()
    for i,x in enumerate(l):
        if x.startswith("Tcp:") and "RetransSegs" in x:
            h=x.split(); v=l[i+1].split(); s["retrans"]=int(v[h.index("RetransSegs")])
    return s

# device 안정 id (t0/t1 안정성 확인용)
def dev_ids():
    m={}
    lj=subprocess.run("lsblk -J -b -o NAME,KNAME,TYPE 2>/dev/null",shell=True,capture_output=True,text=True).stdout
    def walk(n):
        k=n.get("kname")
        dmu=read(f"/sys/block/{k}/dm/uuid").strip()
        ser=read(f"/sys/block/{k}/serial").strip() or read(f"/sys/block/{k}/device/serial").strip()
        m[k]=dmu or ser or None
        for c in n.get("children",[]) or []: walk(c)
    try:
        for n in json.loads(lj).get("blockdevices",[]): walk(n)
    except Exception: pass
    return m

# ---- 부하 (disk direct I/O + cpu) ----
load=subprocess.Popen("for i in $(seq 1 40); do dd if=/dev/zero of=/tmp/tsload bs=1M count=64 oflag=direct 2>/dev/null; done; "
                      "timeout 8 bash -c 'while :; do :; done' &", shell=True, preexec_fn=os.setsid)
time.sleep(0.5)
id0=dev_ids()
a=snap(); time.sleep(5); b=snap()
id1=dev_ids()
try: os.remove("/tmp/tsload")
except Exception: pass

dt=b["t"]-a["t"]; nc=a["ncpu"] or 1
fail=[]; ok=[]
def chk(cond,msg):
    (ok if cond else fail).append(msg)

print(f"=== 시계열 정합성 (dt={dt:.2f}s, ncpu={nc}) ===")

# CPU: Σ Δcpu.time ≈ dt*nc ; util ∈ [0,1] ; 단조
dcpu={k:b["cpu"][k]-a["cpu"][k] for k in a["cpu"]}
tot=sum(dcpu.values()); util=1-dcpu["idle"]/tot if tot>0 else None
chk(all(v>=-1e-6 for v in dcpu.values()),"cpu.time 단조(Δ>=0)")
chk(abs(tot-dt*nc)/(dt*nc)<0.15, f"Σcpu.time≈dt*nc (Σ={tot:.2f} vs {dt*nc:.2f})")
chk(0<=util<=1, f"cpu util∈[0,1] (={util:.3f})")
print(f"  cpu: ΣΔ={tot:.2f}s util={util:.3f}")

# DISK: 단조, %util=Δio_time/dt∈[0,~1], await=Δop_time/Δops sane, throughput
for name,d1 in b["disk"].items():
    d0=a["disk"][name]
    dio=d1["io_time"]-d0["io_time"]; drops=(d1["rops"]-d0["rops"])+(d1["wops"]-d0["wops"])
    dopt=(d1["rt"]-d0["rt"])+(d1["wt"]-d0["wt"]); dby=(d1["wby"]-d0["wby"])+(d1["rby"]-d0["rby"])
    putil=dio/dt if dt>0 else 0
    await_ms=(dopt/drops*1000) if drops>0 else 0
    chk(dio>=-1e-6 and drops>=0 and dby>=0, f"[{name}] disk counters 단조")
    chk(0<=putil<=1.5, f"[{name}] %util∈[0,1.x] (={putil:.3f})")
    chk(0<=await_ms<10000, f"[{name}] await sane (={await_ms:.2f}ms)")
    if drops>0 or dby>0:
        print(f"  disk[{name}]: %util={putil:.3f} await={await_ms:.2f}ms tput={dby/dt/1e6:.1f}MB/s ops={drops}")

# NET 단조 + rate
for d,n1 in b["net"].items():
    n0=a["net"][d]
    chk(n1["rx"]-n0["rx"]>=0 and n1["tx"]-n0["tx"]>=0, f"[{d}] net counters 단조")

# retrans 단조
if "retrans" in a and "retrans" in b:
    chk(b["retrans"]-a["retrans"]>=0, f"tcp retrans 단조 (Δ={b['retrans']-a['retrans']})")

# device id 안정성 (t0==t1)
stable=all(id0.get(k)==id1.get(k) for k in set(id0)|set(id1))
chk(stable, "device 안정 id 가 t0/t1 동일(시계열 자연키 안정)")

print("\n--- 불변식 결과 ---")
for m in ok: print(f"  PASS  {m}")
for m in fail: print(f"  FAIL  {m}")
print(f"\n>>> {len(ok)} PASS / {len(fail)} FAIL")
