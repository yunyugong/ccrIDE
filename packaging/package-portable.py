"""Build an offline ccrIDE bundle from explicitly supplied local dependencies."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import zipfile
import struct

parser = argparse.ArgumentParser()
parser.add_argument('--stage', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--settings', required=True)
parser.add_argument('--version', default='0.3.6')
parser.add_argument('--exe')
parser.add_argument('--compiler', required=True, help='MinGW-w64 root directory containing bin/g++.exe')
parser.add_argument('--dependencies', default=str(Path(__file__).resolve().parents[1]/'dependencies'))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
stage = Path(args.stage).resolve()
output = Path(args.output).resolve()
gcc = Path(args.compiler).resolve()
deps = Path(args.dependencies).resolve()
if stage.exists():
    raise SystemExit(f'Refusing to overwrite stage: {stage}')
stage.mkdir(parents=True)
output.mkdir(parents=True, exist_ok=True)
bundle = stage / 'ccrIDE'
bundle.mkdir()
record = json.loads((repo/'build/releases/latest-build.json').read_text(encoding='utf-8-sig'))
app_exe = Path(args.exe or record['exe'])
packaged_exe=f"ccrIDE-v{args.version}.exe"
shutil.copy2(app_exe,bundle/packaged_exe)
(bundle/'build-info.json').write_text(json.dumps({'version': args.version, 'buildTime': record['buildTime'], 'executable': packaged_exe},indent=2),encoding='utf-8')
shutil.copy2(gcc / 'bin/libwinpthread-1.dll', bundle / 'libwinpthread-1.dll')
shutil.copytree(gcc, bundle / 'toolchain/mingw64')
(bundle / 'portable.ini').write_text('[ccrIDE]\nportable=true\n', encoding='utf-8')
data = bundle / 'data'
data.mkdir()
source = Path(args.settings)
settings = json.loads(source.read_text(encoding='utf-8-sig'))
settings['gppPath'] = 'toolchain/mingw64/bin/g++.exe'
settings['useCmdTemplate'] = False
settings['commandTemplate'] = ''
settings['runDirectory'] = ''
settings['runArguments'] = ''
settings['runEnvironment'] = ''
settings['gdbPath'] = 'toolchain/mingw64/bin/gdb.exe'
settings['debugInputFile'] = ''
settings.pop('ai',None)
# Preserve saved appearance, shortcuts and normal compiler options, not private sessions.
(data / 'settings.json').write_text(json.dumps(settings, indent=2, ensure_ascii=False), encoding='utf-8')
if (source.parent / 'imgui.ini').is_file():
    shutil.copy2(source.parent / 'imgui.ini', data / 'imgui.ini')
for name in ('clock', 'hello'):
    target = bundle / 'plugins' / name
    target.mkdir(parents=True)
    shutil.copy2(repo / 'plugins' / name / 'plugin.json', target / 'plugin.json')
sdk = bundle / 'sdk'
(sdk / 'imgui/backends').mkdir(parents=True)
(sdk / 'include').mkdir()
for header in (deps / 'imgui').glob('*.h'):
    shutil.copy2(header, sdk / 'imgui' / header.name)
for name in ('imgui.cpp', 'imgui_draw.cpp', 'imgui_tables.cpp', 'imgui_widgets.cpp'):
    shutil.copy2(deps / 'imgui' / name, sdk / 'imgui' / name)
for header in (deps / 'imgui/backends').glob('*.h'):
    shutil.copy2(header, sdk / 'imgui/backends' / header.name)
shutil.copytree(deps / 'nlohmann/nlohmann', sdk / 'nlohmann/nlohmann')
shutil.copy2(repo / 'src/plugin_api.h', sdk / 'include/plugin_api.h')
licenses = bundle / 'licenses'
licenses.mkdir()
shutil.copy2(deps / 'imgui/LICENSE.txt', licenses / 'imgui-LICENSE.txt')
shutil.copy2(deps / 'glfw/LICENSE.md', licenses / 'glfw-LICENSE.md')
shutil.copy2(repo / 'packaging/licenses/json-LICENSE.MIT', licenses / 'json-LICENSE.MIT')
(licenses / 'zlib-LICENSE.txt').write_text((gcc/'x86_64-w64-mingw32/include/zlib.h').read_text(encoding='utf-8').split('*/',1)[0]+'*/\n',encoding='utf-8')
(licenses / 'README.txt').write_text('GCC / MinGW / GDB and bundled tools: see toolchain/mingw64/licenses and build-info.txt.\nThe installer is ccrIDE native code; ZIP decompression uses zlib.\n', encoding='utf-8')
(bundle / '使用说明.txt').write_text('''ccrIDE v0.3.6 + GCC 11.5 / Windows x64 离线便携包

1. 将整个 ccrIDE 文件夹解压到个人可写目录；不要在压缩包内直接运行。
2. 双击 ccrIDE-v*.exe，新建或打开 .cpp。F10 编译，F11 编译并运行，F12 运行最近一次成功编译的程序。
3. 标准输入在终端面板输入，回车发送；停止按钮可以中止编译或运行。
4. 设置与布局保存在 data；插件在 plugins；编译器在 toolchain/mingw64。
5. 移动时携带整个文件夹。无需另装 g++，无需配置系统 PATH，无需联网或管理员权限。
6. 包含已保存的主题、字号、配色、快捷键及标准编译选项；环境覆盖、历史会话、最近文件与未保存代码不随包分发。
7. Ctrl+点击行号设置断点，F5 开始调试或继续；调试面板提供步入、步过、变量、求值和调用栈。输入文件在设置中指定。样例测试、Subtask 与对拍从“测试”菜单打开。
8. 红杠表示相对上次保存新增或修改的行，绿杠表示原有或已保存行。编译产物保存在源文件旁的 .ccrIDE-build，带唯一时间标识，不覆盖旧 EXE。
9. 系统需支持 OpenGL 3.3，并安装可显示中文的系统字体。
10. 底部“问题 / 输出 / 终端 / 调试”标签可拖动、拆分和停靠；文件图标遵循当前电脑的 Windows 文件关联。旧布局保留，可在“视图 → 恢复默认布局”切换到默认布局。
11. AI 助手和补全注释已拆为独立插件，主程序不含这两项实现。需要时在“插件 → 安装本地插件”中选择单独下载的插件 ZIP。普通代码补全无需插件。
12. Alt+单击添加光标，Alt+Shift+拖动列选择，Esc 回到单光标；支持批量输入、删除和粘贴。
13. 全标签恢复快照每 3 秒保存，启动后可选择恢复副本，不直接覆盖源码。
14. 编辑菜单的符号重命名需要 clangd 1.1.0 插件；请求预览并应用后自行保存。

EXE 是项目自编写的 Windows 离线安装程序：选择安装目录并点击安装。无需联网，不使用通用 SFX 外壳。
ZIP 与 EXE 包含相同的程序文件。更新或重新解压请选新目录，避免覆盖已有 data 设置。
第三方组件许可见 licenses 和 toolchain/mingw64/licenses。
''', encoding='utf-8-sig')

# A test executable is temporarily placed beside the packaged app to exercise
# exactly the same App::Init and compile/run pipeline after relocation, with no dev PATH.
check = bundle / 'package-check.exe'
shutil.copy2(repo / 'build/cmake/run_tests.exe', check)
environment = os.environ.copy()
environment['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32')
subprocess.run([str(check), '--package-check'], cwd=stage, env=environment, check=True)
check.unlink()
for generated in data.glob('package-check*'):
    generated.unlink()
for generated in ('build_out.txt', 'build_err.txt', 'build-artifacts.json'):
    (data / generated).unlink(missing_ok=True)

generated_builds=data/'.ccrIDE-build'
if generated_builds.exists():
    assert generated_builds.resolve().parent == data.resolve()
    shutil.rmtree(generated_builds)

manifest = {}
for file in sorted(bundle.rglob('*')):
    if file.is_file():
        manifest[file.relative_to(bundle).as_posix()] = hashlib.sha256(file.read_bytes()).hexdigest()
(bundle / 'manifest-sha256.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
base = f'ccrIDE-v{args.version}-win64-with-GCC'
exe = output / f'ccrIDE-Setup-v{args.version}.exe'
archive = output / f'{base}.zip'
if exe.exists() or archive.exists():
    raise SystemExit('Output archives already exist; choose a new output directory')
with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=6) as z:
    for file in sorted(bundle.rglob('*')):
        if file.is_file():z.write(file,'ccrIDE/'+file.relative_to(bundle).as_posix())
    failure=z.testzip() if z.mode=='r' else None
with zipfile.ZipFile(archive) as z:
    if z.testzip():raise SystemExit('ZIP verification failed')
stub=repo/'build/cmake/ccride_installer.exe'
with exe.open('wb') as target:
    target.write(stub.read_bytes());offset=target.tell()
    with archive.open('rb') as payload:shutil.copyfileobj(payload,target)
    target.write(struct.pack('<16sQQ',b'CCRIDE-ZIP-v1',offset,archive.stat().st_size))
for file in (exe, archive):
    digest=hashlib.sha256(file.read_bytes()).hexdigest()
    file.with_suffix(file.suffix+'.sha256').write_text(f'{digest}  {file.name}\n',encoding='ascii')
    print(f'PACKAGE {file.name}: {file.stat().st_size:,} bytes SHA256 {digest}',flush=True)
print(f'STAGED_FILES {len(manifest)}',flush=True)
