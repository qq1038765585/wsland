#include "common/time.h"

int64_t timespec_to_msec(const struct timespec *a) {
    return (int64_t)a->tv_sec * 1000 + a->tv_nsec / 1000000;
}