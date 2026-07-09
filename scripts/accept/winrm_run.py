import os, sys, winrm
host=sys.argv[1]; transport=sys.argv[2] if len(sys.argv)>2 else 'ntlm'
# 자격은 환경변수로만 받는다(레포에 평문 credential 금지). 값은 인프라 레포/운영자에서.
user=os.environ.get('WINRM_USER'); pw=os.environ.get('WINRM_PASS')
if not user or not pw:
    sys.stderr.write("WINRM_USER/WINRM_PASS 환경변수 필요(자격은 인프라 레포/운영자에서 확보)\n"); sys.exit(3)
ps=sys.stdin.read()
try:
    s=winrm.Session(f'http://{host}:5985/wsman', auth=(user, pw),
                    transport=transport, server_cert_validation='ignore')
    r=s.run_ps(ps)
    sys.stdout.write(r.std_out.decode('utf-8','replace'))
    err=r.std_err.decode('utf-8','replace').strip()
    if err: sys.stderr.write("[STDERR] "+err[:500]+"\n")
    sys.exit(r.status_code)
except Exception as e:
    print(f"[{transport}] FAIL: {type(e).__name__}: {str(e)[:200]}")
    sys.exit(2)
