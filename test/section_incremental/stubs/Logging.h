#pragma once

#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_ERR(...) ((void)0)

inline void logMemAt(const char*) {}
inline void logPrintf(const char*, const char*, const char*, ...) {}
