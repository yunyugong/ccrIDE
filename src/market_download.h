#pragma once
#include <atomic>
#include <string>
namespace marketdownload {
// Dispatch before application/UI initialization. -1 means normal application mode.
int WorkerMain();
bool Fetch(const std::string& url,std::string& body,std::string& error,size_t maxBytes,std::atomic<bool>& cancel);
}
