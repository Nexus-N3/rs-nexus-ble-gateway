

//rf survey module application level
#pragma once

// include the std c libraries required
#include <stdbool.h>
#include <stdint.h>

//include modules required
#include "../interface/gateway_interface.h"
#include "../ble/ble_interface.h"

#define RF_SURVEY_SCORE_EXCELLENT_MIN 80U
#define RF_SURVEY_SCORE_GOOD_MIN      65U
#define RF_SURVEY_SCORE_FAIR_MIN      45U
#define RF_SURVEY_SCORE_POOR_MIN      20U

#define RF_SURVEY_FRESH_MS_EXCELLENT 500U
#define RF_SURVEY_FRESH_MS_OK        1500U
#define RF_SURVEY_FRESH_SCORE_MAX    10U
#define RF_SURVEY_FRESH_SCORE_OK     5U

// Define RF Survey states - what is the rf_survery current state?
typedef enum {
    RF_SURVEY_STATE_INACTIVE = 0,
    RF_SURVEY_STATE_ACTIVE,
    RF_SURVEY_STATE_STOPPING, 

} rf_survey_state_t;

// initialise function
void rf_survey_init(void);

// rf survey start function
int rf_survey_start(
    const char *request_id,
    const char addresses[][GATEWAY_MAX_ADDRESS_LEN],
    uint8_t address_count,
    uint32_t window_ms,
    uint32_t duration_ms
);

int rf_survey_stop(const char *request_id);
int rf_survey_send_status(const char *request_id);

bool rf_survey_is_active(void);
rf_survey_state_t rf_survey_get_state(void);

void rf_survey_on_sensor_found(const ble_discovered_sensor_t *sensor);

void rf_survey_tick(void);
