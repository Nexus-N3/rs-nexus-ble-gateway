
#include "rf_survey.h" 

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#define RF_SURVEY_MAX_TARGETS GATEWAY_MAX_SENSORS
#define RF_SURVEY_DEFAULT_WINDOW_MS 5000U
#define RF_SURVEY_DEFAULT_DURATION_MS 60000U

// what we want to track in the rf survey
typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];

    uint32_t observations_current_window;
    int32_t rssi_sum_current_window;
    int8_t min_rssi_current_window;
    int8_t max_rssi_current_window;
    int64_t last_seen_ms;
} rf_survey_target_t;

// the state of the RF survey
typedef struct {
    rf_survey_state_t state;
    uint32_t window_ms;
    uint32_t duration_ms;
    int64_t started_at_ms;
    int64_t current_window_started_at_ms;
    uint8_t target_count;
    rf_survey_target_t targets[RF_SURVEY_MAX_TARGETS];
} rf_survey_context_t;

static rf_survey_context_t g_survey;

//private helper functions

static const char *rf_survey_state_name(rf_survey_state_t state){
    // returns a pointer to a string literal
    switch(state){
    case RF_SURVEY_STATE_INACTIVE:
        return "inactive";
    case RF_SURVEY_STATE_ACTIVE:
        return "active";
    case RF_SURVEY_STATE_STOPPING:
        return "stopping";
    default:
        return "unknown";
    }
}

static void rf_survey_reset_target_window(rf_survey_target_t *target)
{
    if (target == NULL) {
        return;
    }

    target->observations_current_window = 0;
    target->rssi_sum_current_window = 0;
    target->min_rssi_current_window = 0;
    target->max_rssi_current_window = 0;
}

// returns a point to an rf_survey_target
static rf_survey_target_t *rf_survey_find_target(const char *address)
{
    if (address == NULL || address[0] == '\0') {
        return NULL;
    }

    for (uint8_t i = 0; i < g_survey.target_count; i++) {
        // all targets should be used i would think so this might be redundant?
        if (!g_survey.targets[i].used) {
            continue;
        }

        if (strcmp(g_survey.targets[i].address, address) == 0) {
            return &g_survey.targets[i];
        }
    }

    return NULL;
}

//copies the target address into the survey context
static int rf_survey_add_target(const char *address)
{
    rf_survey_target_t *target; //create a pointer to a target struct

    //basic checks
    if (address == NULL || address[0] == '\0') {
        return -1;
    }

    if (g_survey.target_count >= RF_SURVEY_MAX_TARGETS) {
        return -12;
    }

    // gets the next unused slot in the targets list (its a memory location i think)
    target = &g_survey.targets[g_survey.target_count];

    memset(target, 0, sizeof(*target));

    target->used = true;

    strncpy(
        target->address,
        address,
        sizeof(target->address) - 1
    );

    rf_survey_reset_target_window(target);

    target->last_seen_ms = 0;

    g_survey.target_count++;

    return 0;

}

void rf_survey_init(void){
    //prepares the rf_survey for use when the gateway starts
    memset(&g_survey, 0, sizeof(g_survey)); // clears the entire global survey context to zero
    //set the intial state of the survey to INACTIVE 
    g_survey.state = RF_SURVEY_STATE_INACTIVE;
}

bool rf_survey_is_active(void){
    // checks if the survey is active? returns true or false
    return g_survey.state == RF_SURVEY_STATE_ACTIVE;
}

rf_survey_state_t rf_survey_get_state(void){
    return g_survey.state;
}

// start survey - returns a response code int 
int rf_survey_start(
    const char *request_id,
    const char addresses[][GATEWAY_MAX_ADDRESS_LEN],
    uint8_t address_count,
    uint32_t window_ms,
    uint32_t duration_ms
){

    int rc;

    //basic checks
    if (g_survey.state != RF_SURVEY_STATE_INACTIVE) {
        gateway_interface_send_error(
            request_id,
            "rf_survey_already_active",
            -16
        );
        return -16;
    }

    if (addresses == NULL || address_count == 0) {
        gateway_interface_send_error(
            request_id,
            "rf_survey_no_targets",
            -1
        );
        return -1;
    }

    if (address_count > RF_SURVEY_MAX_TARGETS) {
        gateway_interface_send_error(
            request_id,
            "rf_survey_too_many_targets",
            -12
        );
        return -12;
    }

    //set up the survey context
    memset(&g_survey, 0, sizeof(g_survey));

    g_survey.state = RF_SURVEY_STATE_ACTIVE;
    g_survey.window_ms =
        window_ms != 0 ? window_ms : RF_SURVEY_DEFAULT_WINDOW_MS;
    g_survey.duration_ms =
        duration_ms != 0 ? duration_ms : RF_SURVEY_DEFAULT_DURATION_MS;
    g_survey.started_at_ms = k_uptime_get();
    g_survey.current_window_started_at_ms = g_survey.started_at_ms;

    //add the addresses to the survey context
    for (uint8_t i = 0; i < address_count; i++) {
        rc = rf_survey_add_target(addresses[i]);
        if (rc != 0) {
            memset(&g_survey, 0, sizeof(g_survey)); // clear out the survey if one fails to be added
            g_survey.state = RF_SURVEY_STATE_INACTIVE;

            gateway_interface_send_error(
                request_id,
                "rf_survey_add_target_failed",
                rc
            );

            return rc;
        }
    }

    char line[256];

    //writes the formated line to the char buffer above
    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"rf_survey_started\","
        "\"request_id\":\"%s\","
        "\"ok\":true,"
        "\"state\":\"%s\","
        "\"target_count\":%u,"
        "\"window_ms\":%u,"
        "\"duration_ms\":%u}",
        request_id != NULL ? request_id : "",
        rf_survey_state_name(g_survey.state),
        (unsigned int)g_survey.target_count,
        (unsigned int)g_survey.window_ms,
        (unsigned int)g_survey.duration_ms
    );

    return gateway_interface_send_json_line(line);
}

int rf_survey_stop(const char *request_id){

    char line[192];  // buffer to write out result

    if (g_survey.state == RF_SURVEY_STATE_INACTIVE) {
        gateway_interface_send_error(
            request_id,
            "rf_survey_not_active",
            -2
        );
        return -2;
    }

    g_survey.state = RF_SURVEY_STATE_STOPPING;

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"rf_survey_stopped\","
        "\"request_id\":\"%s\","
        "\"ok\":true,"
        "\"state\":\"%s\","
        "\"target_count\":%u,"
        "\"elapsed_ms\":%u}",
        request_id != NULL ? request_id : "",
        rf_survey_state_name(g_survey.state),
        (unsigned int)g_survey.target_count,
        (unsigned int)(k_uptime_get() - g_survey.started_at_ms)
    );

    memset(&g_survey, 0, sizeof(g_survey));
    g_survey.state = RF_SURVEY_STATE_INACTIVE;

    return gateway_interface_send_json_line(line);

}

int rf_survey_send_status(const char *request_id)
{
    char line[256];
    int64_t now_ms = k_uptime_get();
    uint32_t elapsed_ms = 0;

    if (g_survey.state == RF_SURVEY_STATE_ACTIVE &&
        g_survey.started_at_ms > 0 &&
        now_ms >= g_survey.started_at_ms) {
        elapsed_ms = (uint32_t)(now_ms - g_survey.started_at_ms);
    }

    snprintf(
        line,
        sizeof(line),
        "{\"type\":\"rf_survey_status\","
        "\"request_id\":\"%s\","
        "\"active\":%s,"
        "\"state\":\"%s\","
        "\"elapsed_ms\":%u,"
        "\"window_ms\":%u,"
        "\"duration_ms\":%u,"
        "\"target_count\":%u}",
        request_id != NULL ? request_id : "",
        rf_survey_is_active() ? "true" : "false",
        rf_survey_state_name(g_survey.state),
        (unsigned int)elapsed_ms,
        (unsigned int)g_survey.window_ms,
        (unsigned int)g_survey.duration_ms,
        (unsigned int)g_survey.target_count
    );

    return gateway_interface_send_json_line(line);
}

void rf_survey_on_sensor_found(const ble_discovered_sensor_t *sensor)
{
    rf_survey_target_t *target;

    //basic checks
    if (sensor == NULL) {
        return;
    }

    if (g_survey.state != RF_SURVEY_STATE_ACTIVE) {
        return;
    }

    target = rf_survey_find_target(sensor->address);
    if (target == NULL) {
        return;
    }

    if (target->observations_current_window == 0) {
        target->min_rssi_current_window = sensor->rssi;
        target->max_rssi_current_window = sensor->rssi;
    } else {
        if (sensor->rssi < target->min_rssi_current_window) {
            target->min_rssi_current_window = sensor->rssi;
        }

        if (sensor->rssi > target->max_rssi_current_window) {
            target->max_rssi_current_window = sensor->rssi;
        }
    }

    target->observations_current_window++;
    target->rssi_sum_current_window += sensor->rssi;
    target->last_seen_ms = k_uptime_get();
}

void rf_survey_tick(void)
{
    int64_t now_ms;

    if (g_survey.state != RF_SURVEY_STATE_ACTIVE) {
        return;
    }

    if (g_survey.duration_ms == 0) {
        return;
    }

    now_ms = k_uptime_get();

    if ((uint32_t)(now_ms - g_survey.started_at_ms) >= g_survey.duration_ms) {
        g_survey.state = RF_SURVEY_STATE_STOPPING;

        /*
         * For this first version, do not emit JSON here.
         * The scan scheduler may also timeout and send scan_complete.
         * We can decide later whether duration expiry should emit
         * rf_survey_stopped automatically.
         */
        memset(&g_survey, 0, sizeof(g_survey));
        g_survey.state = RF_SURVEY_STATE_INACTIVE;
    }
}