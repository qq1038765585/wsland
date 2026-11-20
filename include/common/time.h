#ifndef WSLAND_COMMON_TIME_H
#define WSLAND_COMMON_TIME_H

#include <time.h>
#include <stdint.h>

int64_t timespec_to_msec(const struct timespec *a);
#endif // WSLAND_COMMON_TIME_H
