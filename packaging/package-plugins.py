from pathlib import Path
import json,zipfile,hashlib,argparse
p=argparse.ArgumentParser();p.add_argument('--output',required=True);a=p.parse_args()
repo=Path(__file__).resolve().parents[1];out=Path(a.output);out.mkdir(parents=True,exist_ok=True)
for ident in ('ai-assistant','completion-docs','clangd'):
    folder=repo/'build/plugins'/ident
    manifest=json.loads((folder/'plugin.json').read_text(encoding='utf-8'))
    api_version=manifest.get("apiVersion",1)
    minimum="0.3.5" if ident=="clangd" else "0.3.0"
    archive=out/f"{ident}-v{manifest['version']}.zip"
    if archive.exists():raise SystemExit(f'Refusing to replace {archive}')
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
        for file in sorted(folder.rglob('*')):
            if file.is_file():z.write(file,ident+'/'+file.relative_to(folder).as_posix())
        z.writestr(ident+'/README.txt',f"{manifest['name']} v{manifest['version']}\n需要 ccrIDE v{minimum} 或以上（插件 API v{api_version}）。\n在 IDE 中选择 插件 → 安装本地插件，再选择本 ZIP。\n停用或卸载后，普通代码补全仍然可用。\n".encode('utf-8-sig'))
    archive.with_suffix('.zip.sha256').write_text(hashlib.sha256(archive.read_bytes()).hexdigest()+'  '+archive.name+'\n')
    print(archive)
