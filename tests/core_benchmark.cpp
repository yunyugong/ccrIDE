#include "app.h"
#include <chrono>
#include <iostream>
#include <psapi.h>
#include <algorithm>
int main(){
 using Clock=std::chrono::steady_clock;g_app.settings.autoCheck=false;g_app.settings.gppPath.clear();
 for(int bytes:{100000,1000000,4000000}){
  std::string text;while(text.size()<size_t(bytes))text+="int sample_value = 42; // benchmark\n";
  std::vector<double> times;for(int i=0;i<7;++i){CodeEditor ed;auto start=Clock::now();ed.SetText(text);ed.RebuildLines();ed.EnsureCodeModel();auto end=Clock::now();times.push_back(std::chrono::duration<double,std::milli>(end-start).count());}
  std::sort(times.begin(),times.end());PROCESS_MEMORY_COUNTERS_EX counters{};GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters));
  std::cout<<"{\"bytes\":"<<text.size()<<",\"samples\":7,\"medianMs\":"<<times[3]<<",\"p95Ms\":"<<times[6]<<",\"peakWorkingSetBytes\":"<<counters.PeakWorkingSetSize<<",\"privateBytes\":"<<counters.PrivateUsage<<"}\n";
 }
}
