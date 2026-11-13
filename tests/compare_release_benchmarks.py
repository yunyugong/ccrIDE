"""Compare the archived unoptimized 0.3.5 harness and the current Release harness.
Both version and configuration differ; the result is not a source-only speedup claim.
"""
from pathlib import Path
import argparse,subprocess,json,platform,hashlib,winreg,datetime
p=argparse.ArgumentParser();p.add_argument('--baseline',required=True);p.add_argument('--current',required=True);p.add_argument('--output',required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1]
expected=json.loads((root/'docs/performance-data.json').read_text(encoding='utf-8'))['builds']['0.3.5']['exeSha256']
if hashlib.sha256(Path(a.baseline).read_bytes()).hexdigest()!=expected:raise SystemExit('Baseline is not the archived 0.3.5 harness')
with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:cpu=winreg.QueryValueEx(key,'ProcessorNameString')[0].strip()
data={'measuredAt':datetime.datetime.now(datetime.timezone.utc).isoformat(),'environment':{'os':platform.platform(),'cpu':cpu,'compiler':'GCC 11.5.0','plugins':[]},'workload':'Identical tests/core_benchmark.cpp: SetText + RebuildLines + CodeModel Ensure; 7 samples per size','limits':['Synthetic warm core operations only','Historical version and build flags both differ; no source-only attribution','Maximum observation is not a statistically established p95','Excludes cold startup, UI latency, filesystem search and clangd'],'builds':{}}
for version,path,flags in [('0.3.5-unoptimized',a.baseline,'-std=gnu++17'),('0.3.8-Release',a.current,'-O3 -DNDEBUG -std=gnu++17')]:
 exe=Path(path);run=subprocess.run([str(exe)],capture_output=True,text=True,check=True)
 measurements=[]
 for line in run.stdout.splitlines():
  if line.startswith('{'):
   item=json.loads(line);item['maximumMs']=item.pop('p95Ms');measurements.append(item)
 data['builds'][version]={'exeSha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'cxxFlags':flags,'measurements':measurements}
out=Path(a.output);out.parent.mkdir(parents=True,exist_ok=True);out.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');print(out)
