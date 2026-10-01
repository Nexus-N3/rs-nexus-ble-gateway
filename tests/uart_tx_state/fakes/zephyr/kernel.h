#pragma once

#include <stdint.h>

#define ARG_UNUSED(value) ((void)(value))
#define SYS_FOREVER_US (-1)

extern int64_t fake_uptime_ms;

static inline int64_t k_uptime_get(void)
{
    return fake_uptime_ms;
}

static inline unsigned int irq_lock(void)
{
    return 0;
}

static inline void irq_unlock(unsigned int key)
{
    ARG_UNUSED(key);
}
