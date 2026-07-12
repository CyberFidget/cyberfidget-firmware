# -*- coding: utf-8 -*-
import serial, time, json, zlib, sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
PORT="COM30"; BAUD=921600
s=serial.Serial(PORT,BAUD,timeout=0.25); rx=b""
def reset(): s.dtr=False; s.rts=True; time.sleep(0.1); s.rts=False; time.sleep(0.05)
def drain(t=0.9):
    global rx; end=time.time()+t; buf=rx; rx=b""
    while time.time()<end:
        b=s.read(4096)
        if b: buf+=b; end=time.time()+0.2
    return buf.decode("utf-8","replace")
def line(l): s.write(l.encode()+b"\n")
def crc(b): return format(zlib.crc32(b)&0xFFFFFFFF,"08x")
def rl(t=4):
    global rx; end=time.time()+t
    while time.time()<end:
        while b"\n" in rx:
            ln,rx=rx.split(b"\n",1); v=ln.decode("utf-8","replace").strip()
            if v.startswith("[cmd]") or v.startswith("[err]"): return v
        c=s.read(4096)
        if c: rx+=c
    return ""
def ferry(path,data,corrupt=False):
    line(f"fwrite {path} {len(data)} {crc(data)}");
    if ".ok" not in rl(): return "fwrite FAIL"
    off=0
    while off<len(data):
        part=data[off:off+4096]; snd=part
        if corrupt and off==0: snd=bytes([part[0]^0xFF])+part[1:]
        line(f"fwdata {off} {len(part)} {crc(part)}"); s.write(snd); rr=rl()
        if "[err]" in rr and not (corrupt and off==0): return "fwdata FAIL "+rr
        off+=4096
    line("fwcommit"); return rl(10)
def to_menu():
    for _ in range(8):
        line("btn 5 tap"); drain(0.4); line("menutree"); mt=drain(0.8)
        if "roots=" in mt and "roots=0" not in mt: return mt
        time.sleep(0.9)
    return "no menu"
def add_blob(bid,name,path):
    doc=json.dumps({"ops":[{"op":"add","entry":{"id":bid,"name":name,"category":"Games","format":"blob","blobPath":path,"version":"1.0.0","abi":"1"}}]})
    line(f"lapply {len(doc)} {crc(doc.encode())}"); s.write(doc.encode()); return drain(1.5).strip()

reset(); time.sleep(0.3); b=drain(3.0)
print("boot:", next((l for l in b.splitlines() if 'fw=' in l), '?')[:70])
to_menu()

wasm=open(r"d:/_Steele/Code_Sandbox/cyberfidget/fw-worktree-integration/test/bench/fixtures/breakout.wasm","rb").read()
print("\n[A1] ferry good wasm:", ferry("/apps/breakout.wasm",wasm))
print("[A1] add blob manifest:", add_blob("breakout-wasm","WASM Breakout","/apps/breakout.wasm"))

# return to menu so buildNestedMenu re-reads the manifest, then dump
line("launch menu"); drain(1.2)
line("menutree"); mt=drain(1.2)
blobrows=[l for l in mt.splitlines() if 'blob=1' in l]
print("[A3] menutree blob rows:", blobrows if blobrows else "NONE")

print("\n[A1] launch the blob:", (lambda: (line("launch breakout-wasm"), drain(1.5).strip())[1])())
time.sleep(3.0); line("wasmstat"); ws=drain(0.8).strip()
print("[A1] wasmstat:", ws[:180])
line("app"); print("[A1] active app:", [l for l in drain(0.8).splitlines() if 'name' in l][:1])
line("launch menu"); drain(1.0)

# A2: corrupt-but-CRC-valid blob (truncate to break wasm parse, recompute crc so it commits)
bad=wasm[:2000]  # a valid-CRC but structurally broken module
print("\n[A2] ferry truncated/garbage wasm:", ferry("/apps/bad.wasm",bad))
print("[A2] add bad blob:", add_blob("bad-wasm","Bad WASM","/apps/bad.wasm"))
line("launch bad-wasm"); print("[A2] launch bad:", drain(1.5).strip()[:120])
time.sleep(1.5); line("wasmstat"); print("[A2] wasmstat (expect error, NO reboot):", drain(0.8).strip()[:180])
# prove no reboot: version must answer without a fresh boot banner
line("version"); vr=drain(0.8)
print("[A2] still alive, no reboot:", "fw=" not in vr and "version=" in vr, "|", next((l for l in vr.splitlines() if 'version=' in l),'')[:50])
line("launch menu"); drain(1.0)

# cleanup
for bid in ("breakout-wasm","bad-wasm"):
    doc=json.dumps({"ops":[{"op":"remove","id":bid}]}); line(f"lapply {len(doc)} {crc(doc.encode())}"); s.write(doc.encode()); drain(0.8)
line("fdelete /apps/breakout.wasm"); drain(0.5); line("fdelete /apps/bad.wasm"); drain(0.5)
print("\ncleanup done")
s.close()
