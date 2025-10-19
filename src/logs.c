#include <stdio.h>
#include <time.h>
#include "logs.h"

void log_print(const char *level, const char *fmt, ...)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);

    printf("[%s] %s: ", buf, level);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
}