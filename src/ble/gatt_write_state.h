#pragma once

#include <stdbool.h>

typedef enum {
    GATT_WRITE_IDLE = 0,
    GATT_WRITE_DISCOVERING,
    GATT_WRITE_HANDLE_READY,
    GATT_WRITE_ACTIVE,
    GATT_WRITE_CANCELLING,
    GATT_WRITE_COMPLETE,
} gatt_write_state_t;

static inline bool gatt_write_state_is_busy(gatt_write_state_t state)
{
    return state != GATT_WRITE_IDLE;
}

static inline bool gatt_write_state_can_start(gatt_write_state_t state)
{
    return state == GATT_WRITE_IDLE;
}

static inline bool gatt_write_state_has_zephyr_owner(gatt_write_state_t state)
{
    return state == GATT_WRITE_DISCOVERING ||
        state == GATT_WRITE_ACTIVE ||
        state == GATT_WRITE_CANCELLING;
}

static inline gatt_write_state_t gatt_write_state_after_cancel(
    gatt_write_state_t state
)
{
    if (state == GATT_WRITE_HANDLE_READY) {
        return GATT_WRITE_COMPLETE;
    }

    if (state == GATT_WRITE_DISCOVERING || state == GATT_WRITE_ACTIVE) {
        return GATT_WRITE_CANCELLING;
    }

    return state;
}
