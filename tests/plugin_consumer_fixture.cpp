#include "plugin_api.h"
extern "C" __declspec(dllimport) int CCRDependencyValue();
extern "C" __declspec(dllexport) void Plugin_OnLoadV2(void*,PluginApi* api){api->Log(CCRDependencyValue()==1?"DEPENDENCY=1":"DEPENDENCY=2");}
extern "C" __declspec(dllexport) void Plugin_OnUnload(){}
