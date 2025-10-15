// logs.h
#ifndef LOGS_H
#define LOGS_H

#include <stdarg.h>

void log_print(const char *level, const char *fmt, ...);

#define LOG_INFO(fmt, ...)  log_print("INFO", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_print("WARN", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_print("ERROR", fmt, ##__VA_ARGS__)

#endif