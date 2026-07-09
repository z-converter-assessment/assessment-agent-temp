import sys, winrm
host=sys.argv[1]; transport=sys.argv[2] if len(sys.argv)>2 else 'ntlm'
ps=sys.stdin.read()
try:
    s=winrm.Session(f'http://{host}:5985/wsman', auth=('Administrator','<redacted>'),
                    transport=transport, server_cert_validation='ignore')
    r=s.run_ps(ps)
    sys.stdout.write(r.std_out.decode('utf-8','replace'))
    err=r.std_err.decode('utf-8','replace').strip()
    if err: sys.stderr.write("[STDERR] "+err[:500]+"\n")
    sys.exit(r.status_code)
except Exception as e:
    print(f"[{transport}] FAIL: {type(e).__name__}: {str(e)[:200]}")
    sys.exit(2)
