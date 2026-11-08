"""Generate a GitHub marketplace catalog from the actual published plugin ZIPs."""
from pathlib import Path
from urllib.parse import quote
import argparse,json,hashlib,re,zipfile
p=argparse.ArgumentParser();p.add_argument('--artifacts',required=True);p.add_argument('--tag',required=True);p.add_argument('--output',required=True);p.add_argument('--repository',default='yunyugong/ccrIDE');a=p.parse_args()
if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+',a.repository):raise SystemExit('Expected OWNER/REPOSITORY')
root=Path(__file__).resolve().parents[1]
translations=json.loads((root/'locales/en.json').read_text(encoding='utf-8'))['messages']
items=[]
for ident in ('ai-assistant','clangd','completion-docs'):
    manifest=json.loads((root/'plugins'/ident/'plugin.json').read_text(encoding='utf-8-sig'))
    archive=Path(a.artifacts)/f"{ident}-v{manifest['version']}.zip"
    with zipfile.ZipFile(archive) as z:
        actual=json.loads(z.read(ident+'/plugin.json'))
    for field in ('id','version','entry','apiVersion'):
        if actual.get(field)!=manifest.get(field):raise SystemExit(f'{archive}: mismatched {field}')
    item={k:manifest[k] for k in ('id','name','version','author','description','entry','apiVersion')}
    for field in ('name','description'):item[field]=translations.get(item[field],item[field])
    if 'taskCoordinatorVersion' in manifest:item['taskCoordinatorVersion']=manifest['taskCoordinatorVersion']
    item['packageUrl']=f"https://github.com/{a.repository}/releases/download/{quote(a.tag,safe='')}/{quote(archive.name,safe='')}"
    item['sha256']=hashlib.sha256(archive.read_bytes()).hexdigest();items.append(item)
out=Path(a.output);out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(json.dumps({'schema':1,'plugins':items},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(f'{len(items)} verified package entries written to {out}')
