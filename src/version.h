#pragma once
#if __has_include(<ccride_build_info.h>)
#include <ccride_build_info.h>
#endif
#ifndef CCRIDE_VERSION
#define CCRIDE_VERSION "0.3.3"
#endif
#ifndef CCRIDE_BUILD_TIME
#define CCRIDE_BUILD_TIME __DATE__ " " __TIME__
#endif
