# Plugin API reference

This reference describes ccrIDE **0.3.7.2**, plugin API **v3**, with v1/v2 compatibility. The authoritative layout is [plugin_api.h](../src/plugin_api.h). Use the SDK shipped with the host release; compile Windows x64 DLLs with the same ImGui sources and compatible compiler/runtime settings.

## Lifecycle and compatibility

The host reads `plugins/<id>/plugin.json`, validates the DLL and loads it in the IDE process. It selects exactly one load entry point according to the manifest API version: `Plugin_OnLoadV3`, `Plugin_OnLoadV2`, or `Plugin_OnLoad`. A missing selected entry point is an error; it does not retry another ABI. It also calls `Plugin_OnFrame` once per frame and `Plugin_OnUnload` before unloading. These callbacks run on the **UI thread**. Do not block them, throw exceptions across the ABI, or use host callbacks directly from worker threads.

- v1: legacy load entry point; only the first three service fields are guaranteed.
- v2: `Plugin_OnLoadV2(void*, PluginApi*)`; check `apiVersion >= 2` and `structSize >= sizeof(PluginApi)` before reading extended fields.
- v3: `Plugin_OnLoadV3(void*, PluginApi*)`; check version and `structSize >= sizeof(PluginApiV3)`, then cast to `PluginApiV3*`. `base` is the frozen v2 layout; v3 appends language hooks. Do not insert fields or change packing.

Strings use UTF-8. Text lengths and byte offsets are explicit where shown; no ownership is transferred. Copy borrowed strings/views before retaining them. In `OnUnload`, cancel and join your workers and release plugin resources before returning. Plugins link their own matching ImGui core and must use the host allocator/context:

```cpp
#include "plugin_api.h"
#include <imgui.h>
static bool visible = true;
extern "C" __declspec(dllexport)
void Plugin_OnLoadV2(void* context, PluginApi* api) {
    if (!api || api->apiVersion < 2 || api->structSize < sizeof(PluginApi)) return;
    ImGui::SetAllocatorFunctions(api->Allocate, api->Deallocate, api->allocatorUser);
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
}
extern "C" __declspec(dllexport) void Plugin_OnFrame() {
    if (!visible) return;
    if (ImGui::Begin("Community Example", &visible)) ImGui::TextUnformatted("Hello!");
    ImGui::End();
}
extern "C" __declspec(dllexport) void Plugin_OnUnload() {}
```

Build with the SDK, for example (replace `<SDK>` with its directory):

```text
g++ -shared -O2 -std=c++17 -static-libgcc -static-libstdc++ -I<SDK>/include -I<SDK>/imgui plugin.cpp <SDK>/imgui/imgui.cpp <SDK>/imgui/imgui_draw.cpp <SDK>/imgui/imgui_tables.cpp <SDK>/imgui/imgui_widgets.cpp -o example.dll
```

Do not free host objects with a different allocator. Never retain editor pointers: none are part of the public ABI.

## Manifest

```json
{
  "id": "community-example",
  "name": "Community Example",
  "version": "1.0.0",
  "author": "Your name",
  "description": "An example plugin",
  "entry": "example.dll",
  "kind": "soft",
  "apiVersion": 2
}
```

IDs use ASCII letters, digits, `_` or `-` (1–80 characters), must be unique case-insensitively, and cannot be Windows reserved filenames. `entry` is a DLL filename, not a path. `kind=soft` means an external DLL; `kind=hard` requires code compiled into the host and cannot be installed by adding a DLL. For the official AI/clangd service plugins, `taskCoordinatorVersion: 1` declares participation in the shared task coordinator; adding this field alone does not implement coordination.

## Host services (`PluginApi`)

| Callback | Contract |
| --- | --- |
| `GetDataDir()` | Borrowed UTF-8 host data directory. Store private data under `plugins/<your-id>/` inside this directory. |
| `Log(msg)` | Append a message and newline to the host console. Call on the UI thread; never log credentials or prompts. |
| `GetSetting(key)` | Supports `gppPath`, `theme`, `marketUrl`, and, since 0.3.7.2, `language`. Returns an empty string for unknown keys. Copy the result before the next call. `language` is the effective `en` or `zh-CN`, rather than `auto`. |
| `RegisterCommand(id,label,execute)` | Register a Plugins-menu command. IDs must be unique across all plugins. Re-registering the same ID replaces it. The host copies ID/label, but the callback must remain valid until unload. |
| `RegisterCompletion(hooks)` | Register one documentation provider; the latest registration wins. `nullptr` clears it. The host copies the hooks; their functions must remain valid. |
| `GetSelection(sink,user)` | Synchronously pass selected UTF-8 text to the sink, if an editor exists. Initialize your result before calling. |
| `InsertText(text)` | Replace the active selection / insert UTF-8 text into the active editor. No effect without an editor. |
| `DockNextWindow(title,location)` | Call before `ImGui::Begin` for your window. First-use default only: 0 left, 1 bottom, 2 right. User dragging and saved docking take precedence. Use the same stable ImGui window ID in both calls. |
| `Allocate`, `Deallocate`, `allocatorUser` | ImGui allocator callbacks and opaque user value; install these before using the host context. |

`GetDataDir`/`GetSetting` return borrowed buffers, not thread-safe persistent storage. Marshal asynchronous results into a plugin queue and consume them in `OnFrame`.

## Completion documentation hooks (v2)

- `Begin()` starts a candidate collection cycle; clear per-cycle caches as appropriate.
- `Describe(text,length,offset,sink,user)` receives borrowed declaration source text and a UTF-8 byte offset. Send the completed documentation **once** to the sink: the current host assigns the result rather than appending chunks.
- `Draw(view)` renders details for the selected candidate. `PluginCompletionView` contains borrowed label/hint and an array of `{file,line,documentation}` sources. Lines are 1-based. `selectedSource` points to host state; keep it within `[0,sourceCount)` and do not retain it.

User code, extracted source comments, filenames and compiler messages are not automatically translated. Translate only your own UI. The official documentation plugin allows multiple header sources and opens comment links with the default browser.

## Language services (v3)

`RegisterLanguage(hooks)` installs one provider, replacing/resetting any previous provider; `nullptr` clears it. `Document(file,text,length,revision)` supplies a full UTF-8 snapshot with a monotonic revision. `Close(file)` removes a document. `Request(kind,file,revision,byteOffset)` requests `completion`, `definition`, or `rename:<new-name>`. Copy inputs before submitting asynchronous work.

`PublishLanguage(json,length)` is called from `OnFrame`, never directly from a service thread. Payload limit: 2 MiB. Positions/ranges in the JSON use **zero-based lines and UTF-16 character units**; request offsets and completion replacement offsets use **UTF-8 bytes**. Windows paths are normalized by the host. Current source text, revision and cursor position are checked before applying responses; stale results are discarded.

Common response envelopes:

```json
{"kind":"status","ready":true}
```

```json
{"kind":"completion","file":"C:/work/main.cpp","revision":7,"offset":42,"items":[{"label":"value","detail":"int","text":"value","start":40,"end":42,"symbolKind":6,"callable":false}]}
```

```json
{"kind":"diagnostics","file":"C:/work/main.cpp","revision":7,"items":[{"position":{"line":2,"character":4},"error":true,"message":"Example diagnostic"}]}
```

```json
{"kind":"definition","file":"C:/work/main.cpp","revision":7,"offset":42,"target":"C:/work/box.h","position":{"line":5,"character":0}}
```

```json
{"kind":"rename","file":"C:/work/main.cpp","revision":7,"offset":42,"edits":[{"file":"C:/work/main.cpp","range":{"start":{"line":2,"character":4},"end":{"line":2,"character":9}},"text":"renamed"}]}
```

Completion kinds use LSP CompletionItemKind numbers. Failed completion/definition/rename responses may include `"failed":true` and still need the matching file, revision and offset. Send `ready:false` if the service fails so the core can fall back. The host displays at most 120 completions and 300 diagnostics per response, considers saved documents up to 2 MiB, and limits periodic document sync to 32 documents / 16 MiB. Completion and definition requests fall back after 10 seconds; rename requests time out with a message.

## Localization

Read `GetSetting("language")` on load and each frame. Keep persisted field names, command IDs and protocol names in stable ASCII; do not translate them. Use ImGui `English title###stable-id` to keep layouts stable when visible labels change. The optional `i18n.h`/`i18n_catalog.inc` SDK helpers support the host's catalog, but plugins should own their additional translations. Existing plugin binaries continue to load; they need an updated build to translate their own windows.

## Packaging and trust

ZIP layout: exactly one top-level plugin folder containing `plugin.json`, its DLL and any dependencies/services. Install with Plugins > Install Local Plugin. For a GitHub-hosted catalog and checksums, see [Community hosting](community.md).

Native DLLs run in the IDE process. The API does not provide a security sandbox or complete permission enforcement; crashes, blocking callbacks and memory errors can affect the IDE. Run heavy language/AI work in supervised services and keep UI callbacks short. A catalog checksum detects changed downloads; it does not establish author trust.

中文说明：接口保持 v1/v2/v3 兼容，新增 `GetSetting("language")`；所有宿主回调在主线程调用，异步结果须回到 `OnFrame` 后提交。借用字符串需要及时复制，不能跨 ABI 抛出异常；卸载前停止并回收工作线程。翻译贡献与 GitHub 配置见下方文档。
