#pragma once
#include "plugin_api.h"
class CodeEditor;
namespace language {
void Register(const PluginLanguageHooks* hooks);
void Publish(const char* json,size_t length);
void Reset();
void Tick();
bool Ready();
bool Handles(const CodeEditor* editor);
bool Complete(CodeEditor& editor);
bool Definition(CodeEditor& editor);
bool Current(const CodeEditor& editor,unsigned long long revision);
}
