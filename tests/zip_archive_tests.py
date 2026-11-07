from pathlib import Path
import tempfile,zipfile,subprocess,sys,shutil,stat,struct
extractor=Path(sys.argv[1]);passed=0
with tempfile.TemporaryDirectory(prefix='ccride-zip-test-') as directory:
    root=Path(directory)
    def run(name,entries,success=True,compression=zipfile.ZIP_DEFLATED,cancel=False):
        global passed
        archive=root/(name+'.zip');destination=root/name
        with zipfile.ZipFile(archive,'w',compression) as z:
            for key,value in entries:z.writestr(key,value)
        result=subprocess.run([str(extractor),str(archive),str(destination)]+(['cancel'] if cancel else []),capture_output=True)
        assert (result.returncode==0)==success,(name,result.stderr)
        passed+=1;return archive,destination
    for compression in (zipfile.ZIP_STORED,zipfile.ZIP_DEFLATED):
        archive,dest=run('unicode'+str(compression),[('plugin/中文.txt','hello\n中文'),('plugin/empty',''),('plugin/big',b'x'*300000)],compression=compression)
        assert (dest/'plugin/中文.txt').read_text(encoding='utf-8')=='hello\n中文';passed+=1
    for name in ('../escape','/absolute','C:/drive','a/../../escape','a\\..\\escape','plugin/NUL.txt','plugin/trailing.','plugin/a:b'):
        run('bad'+str(passed),[(name,b'unsafe')],False)
    run('duplicate',[('a.txt','first'),('A.txt','second')],False)
    link=zipfile.ZipInfo('plugin/link');link.create_system=3;link.external_attr=(stat.S_IFLNK|0o777)<<16
    run('symlink',[(link,'../outside')],False)
    run('cancel',[('large',b'x'*1000000)],False,cancel=True)
    archive,destination=run('overwrite',[('a','original')])
    result=subprocess.run([str(extractor),str(archive),str(destination)],capture_output=True)
    assert result.returncode!=0 and (destination/'a').read_text()=='original';passed+=1
    archive,destination=run('crc',[('data',b'CRC-check-data')],compression=zipfile.ZIP_STORED)
    data=bytearray(archive.read_bytes());index=data.find(b'CRC-check-data');data[index]^=1;archive.write_bytes(data)
    result=subprocess.run([str(extractor),str(archive),str(root/'crc-broken')],capture_output=True)
    assert result.returncode!=0;passed+=1
print(f'ZIP container: {passed} passed')
