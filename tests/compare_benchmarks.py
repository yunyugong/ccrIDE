"""Run the same core-operation harness against two builds; not a cold-start benchmark."""
from pathlib import Path
import argparse,subprocess,json,platform,hashlib,winreg,ctypes
p=argparse.ArgumentParser();p.add_argument("--baseline",required=True);p.add_argument("--current",required=True);p.add_argument("--output",required=True);a=p.parse_args()
with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:cpu=winreg.QueryValueEx(key,"ProcessorNameString")[0]
class Memory(ctypes.Structure):
 _fields_=[("length",ctypes.c_uint32),("load",ctypes.c_uint32)]+[(name,ctypes.c_uint64) for name in ("total","available","pageTotal","pageAvailable","virtualTotal","virtualAvailable","extended")]
memory=Memory();memory.length=ctypes.sizeof(memory);ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory))
data={"environment":{"os":platform.platform(),"cpu":cpu,"ramBytes":memory.total,"compiler":"MinGW-w64 GCC 11.5.0","cmakeBuildType":"","cxxFlags":"","plugins":[],"workload":"SetText + RebuildLines + CodeModel Ensure; repeated int declarations; 7 samples; maximum reported as p95 for n=7","excludes":["cold startup","idle CPU","disk open","UI latency","clangd","project indexing"]},"builds":{}}
for label,path in (("0.3.4",a.baseline),("0.3.5",a.current)):
 exe=Path(path);run=subprocess.run([str(exe)],capture_output=True,text=True,check=True)
 data["builds"][label]={"exeSha256":hashlib.sha256(exe.read_bytes()).hexdigest(),"measurements":[json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]}
out=Path(a.output);out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding="utf-8")
print(out)
