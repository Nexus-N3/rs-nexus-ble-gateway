
#include "rf_survey.h" 

#include <errno.h>
#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>

#define RF_SURVEY_MAX_TARGETS GATEWAY_MAX_SENSORS
#define RF_SURVEY_DEFAULT_WINDOW_MS 5000U
#define RF_SURVEY_DEFAULT_DURATION_MS 60000U
#define RF_SURVEY_JSON_LINE_MAX 768

// what we want to track in the rf survey
typedef struct {
    bool used;
    char address[GATEWAY_MAX_ADDRESS_LEN];

    uint32_t observations_current_window;
    int32_t rssi_sum_current_window;
    int8_t min_rssi_current_window;
    int8_t max_rssi_current_window;

    uint32_t observations_total;
    int32_t rssi_sum_total;
    int8_t min_rssi_total;
    int8_t max_rssi_total;

    int64_t first_seen_ms;
    int64_t last_seen_ms;

    //trend parameters
    bool has_previous_window_score;
    uint8_t previous_window_score;
    uint8_t last_reported_score;

    //survey variation
    bool has_score_history;

    uint8_t best_score;
    uint8_t worst_score;
    uint32_t score_sum;
    uint32_t score_sample_count;

    uint32_t best_score_elapsed_ms;
    uint32_t worst_score_elapsed_ms;
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
static char g_rf_survey_status_line[RF_SURVEY_JSON_LINE_MAX];
static char g_rf_survey_target_status_line[RF_SURVEY_JSON_LINE_MAX];
static char g_rf_survey_target_final_line[RF_SURVEY_JSON_LINE_MAX];
static char g_rf_survey_stop_line[RF_SURVEY_JSON_LINE_MAX];

typedef struct {
    bool seen;
    bool seen_total;
    int32_t avg_rssi;
    int32_t avg_rssi_total;
    uint32_t first_seen_age_ms;
    uint32_t last_seen_age_ms;
    uint8_t score;
    const char *quality;
} rf_survey_target_snapshot_t;

//private helper functions

static uint8_t rf_survey_score_rssi(int32_t rssi_avg)
{
    if (rssi_avg >= -45) {
        return 70;
    }

    if (rssi_avg <= -85) {
        return 0;
    }

    return (uint8_t)(((rssi_avg + 85) * 70) / 40);
}

static uint8_t rf_survey_score_observations(uint32_t observations)
{
    if (observations >= 8) {
        return 20;
    }

    return (uint8_t)((observations * 20) / 8);
}

static uint8_t rf_survey_score_freshness(uint32_t last_seen_age_ms)
{
    if (last_seen_age_ms <= RF_SURVEY_FRESH_MS_EXCELLENT) {
        return RF_SURVEY_FRESH_SCORE_MAX;
    }

    if (last_seen_age_ms <= RF_SURVEY_FRESH_MS_OK) {
        return RF_SURVEY_FRESH_SCORE_OK;
    }

    return 0;
}

static uint8_t rf_survey_compute_score(
    bool seen,
    bool seen_total,
    int32_t rssi_avg,
    uint32_t observations,
    uint32_t last_seen_age_ms
)
{
    uint8_t score = 0;

    if (observations == 0U) {
        if (!seen_total) {
            return 0;
        }

        return rf_survey_score_freshness(last_seen_age_ms);
    }

    if (!seen) {
        return 0;
    }

    score += rf_survey_score_rssi(rssi_avg);
    score += rf_survey_score_observations(observations);
    score += rf_survey_score_freshness(last_seen_age_ms);

    if (score > 100) {
        score = 100;
    }

    return score;
}

static const char *rf_survey_quality_label(uint8_t score)
{
    if (score == 0U) {
        return "missing";
    }

    if (score >= RF_SURVEY_SCORE_EXCELLENT_MIN) {
        return "excellent";
    }

    if (score >= RF_SURVEY_SCORE_GOOD_MIN) {
        return "good";
    }

    if (score >= RF_SURVEY_SCORE_FAIR_MIN) {
        return "fair";
    }

    if (score >= RF_SURVEY_SCORE_POOR_MIN) {
        return "poor";
    }

    return "missing";
}

static void rf_survey_snapshot_target(
    const rf_survey_target_t *target,
    int64_t now_ms,
    rf_survey_target_snapshot_t *snapshot
)
{
    if (target == NULL || snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->quality = "missing";
    snapshot->seen = target->observations_current_window > 0U;
    snapshot->seen_total = target->observations_total > 0U;

    if (snapshot->seen) {
        snapshot->avg_rssi =
            target->rssi_sum_current_window /
            (int32_t)target->observations_current_window;
    }

    if (snapshot->seen_total) {
        snapshot->avg_rssi_total =
            target->rssi_sum_total /
            (int32_t)target->observations_total;
    }

    if (target->last_seen_ms > 0 && now_ms >= target->last_seen_ms) {
        snapshot->last_seen_age_ms = (uint32_t)(now_ms - target->last_seen_ms);
    }

    if (target->first_seen_ms > 0 && now_ms >= target->first_seen_ms) {
        snapshot->first_seen_age_ms = (uint32_t)(now_ms - target->first_seen_ms);
    }

    snapshot->score = rf_survey_compute_score(
        snapshot->seen,
        snapshot->seen_total,
        snapshot->avg_rssi,
        target->observations_current_window,
        snapshot->last_seen_age_ms
    );
    snapshot->quality = rf_survey_quality_label(snapshot->score);
}

static int rf_survey_finish_json_line(
    const char *line,
    size_t line_capacity,
    int line_len
)
{
    if (line_len < 0) {
        return -EIO;
    }

    if (line_len >= (int)line_capacity) {
        return -EMSGSIZE;
    }

    return gateway_interface_send_json_line(line);
}

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

static void rf_survey_reset_all_target_windows(void)
{
    for (uint8_t i = 0; i < g_survey.target_count; i++) {
        rf_survey_reset_target_window(&g_survey.targets[i]);
    }
}

static void rf_survey_complete_window(void)
{
    for (uint8_t i = 0; i < RF_SURVEY_MAX_TARGETS; i++) {
        rf_survey_target_t *target = &g_survey.targets[i];

        if (!target->used) {
            continue;
        }

        target->previous_window_score = target->last_reported_score;
        target->has_previous_window_score = true;
    }

    rf_survey_reset_all_target_windows();
    g_survey.current_window_started_at_ms += g_survey.window_ms;
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

static void rf_survey_update_target_stats(
    rf_survey_target_t *target,
    int8_t rssi,
    int64_t now_ms
)
{
    if (target == NULL) {
        return;
    }

    if (target->observations_current_window == 0) {
        target->min_rssi_current_window = rssi;
        target->max_rssi_current_window = rssi;
    } else {
        if (rssi < target->min_rssi_current_window) {
            target->min_rssi_current_window = rssi;
        }

        if (rssi > target->max_rssi_current_window) {
            target->max_rssi_current_window = rssi;
        }
    }

    target->observations_current_window++;
    target->rssi_sum_current_window += rssi;

    if (target->observations_total == 0) {
        target->min_rssi_total = rssi;
        target->max_rssi_total = rssi;
        target->first_seen_ms = now_ms;
    } else {
        if (rssi < target->min_rssi_total) {
            target->min_rssi_total = rssi;
        }

        if (rssi > target->max_rssi_total) {
            target->max_rssi_total = rssi;
        }
    }

    target->observations_total++;
    target->rssi_sum_total += rssi;

    target->last_seen_ms = now_ms;
}

static const char *rf_survey_trend_label(
    uint8_t current_score,
    uint8_t previous_score,
    bool has_previous_score
)
{
    if (!has_previous_score) {
        return "not_yet_computed";
    }

    if (current_score >= previous_score + RF_SURVEY_TREND_DELTA_MIN) {
        return "improving";
    }

    if (previous_score >= current_score + RF_SURVEY_TREND_DELTA_MIN) {
        return "degrading";
    }

    return "stable";
}

static void rf_survey_update_score_history(
    rf_survey_target_t *target,
    uint8_t score,
    uint32_t elapsed_ms
)
{
    if (target == NULL) {
        return;
    }

    if (!target->has_score_history) {
        target->has_score_history = true;
        target->best_score = score;
        target->worst_score = score;
        target->best_score_elapsed_ms = elapsed_ms;
        target->worst_score_elapsed_ms = elapsed_ms;
    }

    if (score > target->best_score) {
        target->best_score = score;
        target->best_score_elapsed_ms = elapsed_ms;
    }

    if (score < target->worst_score) {
        target->worst_score = score;
        target->worst_score_elapsed_ms = elapsed_ms;
    }

    target->score_sum += score;
    target->score_sample_count++;
}

static uint8_t rf_survey_mean_score(const rf_survey_target_t *target)
{
    if (target == NULL || target->score_sample_count == 0U) {
        return 0U;
    }

    return (uint8_t)(target->score_sum / target->score_sample_count);
}

static int rf_survey_send_target_status(
    const char *request_id,
    rf_survey_target_t *target,
    int64_t now_ms
)
{
    int line_len;
    rf_survey_target_snapshot_t snapshot;
    const char *trend;
    uint32_t elapsed_ms = 0;

    if (target == NULL || !target->used) {
        return 0;
    }

    rf_survey_snapshot_target(target, now_ms, &snapshot);

    if (g_survey.started_at_ms > 0 && now_ms >= g_survey.started_at_ms) {
        elapsed_ms = (uint32_t)(now_ms - g_survey.started_at_ms);
    }

    trend = rf_survey_trend_label(
        snapshot.score,
        target->previous_window_score,
        target->has_previous_window_score
    );

    target->last_reported_score = snapshot.score;
    rf_survey_update_score_history(target, snapshot.score, elapsed_ms);

    line_len = snprintf(
        g_rf_survey_target_status_line,
        sizeof(g_rf_survey_target_status_line),
        "{\"type\":\"rf_survey_target_status\","
        "\"request_id\":\"%s\","
        "\"address\":\"%s\","
        "\"seen\":%s,"
        "\"observations\":%u,"
        "\"rssi_avg\":%d,"
        "\"rssi_min\":%d,"
        "\"rssi_max\":%d,"
        "\"last_seen_age_ms\":%u,"
        "\"score\":%u,"
        "\"quality\":\"%s\","
        "\"trend\":\"%s\"}",
        request_id != NULL ? request_id : "",
        target->address,
        snapshot.seen ? "true" : "false",
        (unsigned int)target->observations_current_window,
        (int)snapshot.avg_rssi,
        (int)target->min_rssi_current_window,
        (int)target->max_rssi_current_window,
        (unsigned int)snapshot.last_seen_age_ms,
        (unsigned int)snapshot.score,
        snapshot.quality,
        trend
    );

    return rf_survey_finish_json_line(
        g_rf_survey_target_status_line,
        sizeof(g_rf_survey_target_status_line),
        line_len
    );
}

static int rf_survey_send_target_final(
    const char *request_id,
    const rf_survey_target_t *target,
    int64_t now_ms
)
{
    int line_len;
    rf_survey_target_snapshot_t snapshot;
    const char *trend;

    if (target == NULL || !target->used) {
        return 0;
    }

    rf_survey_snapshot_target(target, now_ms, &snapshot);

    trend = rf_survey_trend_label(
        snapshot.score,
        target->previous_window_score,
        target->has_previous_window_score
    );

    line_len = snprintf(
        g_rf_survey_target_final_line,
        sizeof(g_rf_survey_target_final_line),
        "{\"type\":\"rf_survey_target_final\","
        "\"request_id\":\"%s\","
        "\"address\":\"%s\","
        "\"seen\":%s,"
        "\"seen_total\":%s,"
        "\"observations_total\":%u,"
        "\"rssi_avg_total\":%d,"
        "\"rssi_min_total\":%d,"
        "\"rssi_max_total\":%d,"
        "\"first_seen_age_ms\":%u,"
        "\"last_seen_age_ms\":%u,"
        "\"score\":%u,"
        "\"quality\":\"%s\","
        "\"trend\":\"%s\","
        "\"best_score\":%u,"
        "\"worst_score\":%u,"
        "\"mean_score\":%u,"
        "\"score_sample_count\":%u,"
        "\"best_score_elapsed_ms\":%u,"
        "\"worst_score_elapsed_ms\":%u}",
        request_id != NULL ? request_id : "",
        target->address,
        snapshot.seen ? "true" : "false",
        snapshot.seen_total ? "true" : "false",
        (unsigned int)target->observations_total,
        (int)snapshot.avg_rssi_total,
        (int)target->min_rssi_total,
        (int)target->max_rssi_total,
        (unsigned int)snapshot.first_seen_age_ms,
        (unsigned int)snapshot.last_seen_age_ms,
        (unsigned int)snapshot.score,
        snapshot.quality,
        trend,
        (unsigned int)target->best_score,
        (unsigned int)target->worst_score,
        (unsigned int)rf_survey_mean_score(target),
        (unsigned int)target->score_sample_count,
        (unsigned int)target->best_score_elapsed_ms,
        (unsigned int)target->worst_score_elapsed_ms
    );

    return rf_survey_finish_json_line(
        g_rf_survey_target_final_line,
        sizeof(g_rf_survey_target_final_line),
        line_len
    );
}

// end of private helpers section

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

static bool rf_survey_has_status_data(void)
{
    return g_survey.state == RF_SURVEY_STATE_ACTIVE ||
           g_survey.state == RF_SURVEY_STATE_STOPPING;
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
    int64_t now_ms;
    uint32_t elapsed_ms = 0;
    int line_len;
    int rc;

    if (g_survey.state == RF_SURVEY_STATE_INACTIVE) {
        gateway_interface_send_error(
            request_id,
            "rf_survey_not_active",
            -2
        );
        return -2;
    }

    g_survey.state = RF_SURVEY_STATE_STOPPING;
    now_ms = k_uptime_get();

    if (g_survey.started_at_ms > 0 && now_ms >= g_survey.started_at_ms) {
        elapsed_ms = (uint32_t)(now_ms - g_survey.started_at_ms);
    }

    line_len = snprintf(
        g_rf_survey_stop_line,
        sizeof(g_rf_survey_stop_line),
        "{\"type\":\"rf_survey_stopped\","
        "\"request_id\":\"%s\","
        "\"ok\":true,"
        "\"state\":\"%s\","
        "\"target_count\":%u,"
        "\"elapsed_ms\":%u}",
        request_id != NULL ? request_id : "",
        rf_survey_state_name(g_survey.state),
        (unsigned int)g_survey.target_count,
        (unsigned int)elapsed_ms
    );

    rc = rf_survey_finish_json_line(
        g_rf_survey_stop_line,
        sizeof(g_rf_survey_stop_line),
        line_len
    );
    if (rc != 0) {
        goto clear_and_return;
    }

    for (uint8_t i = 0; i < g_survey.target_count; i++) {
        rc = rf_survey_send_target_final(
            request_id,
            &g_survey.targets[i],
            now_ms
        );
        if (rc != 0) {
            goto clear_and_return;
        }
    }

    line_len = snprintf(
        g_rf_survey_stop_line,
        sizeof(g_rf_survey_stop_line),
        "{\"type\":\"rf_survey_stop_complete\","
        "\"request_id\":\"%s\","
        "\"target_count\":%u}",
        request_id != NULL ? request_id : "",
        (unsigned int)g_survey.target_count
    );

    rc = rf_survey_finish_json_line(
        g_rf_survey_stop_line,
        sizeof(g_rf_survey_stop_line),
        line_len
    );

clear_and_return:
    memset(&g_survey, 0, sizeof(g_survey));
    g_survey.state = RF_SURVEY_STATE_INACTIVE;

    return rc;
}

int rf_survey_send_status(const char *request_id)
{
    int64_t now_ms = k_uptime_get();
    uint32_t elapsed_ms = 0;
    uint32_t window_elapsed_ms = 0;

    if (rf_survey_has_status_data() &&
        g_survey.started_at_ms > 0 &&
        now_ms >= g_survey.started_at_ms) {
        elapsed_ms = (uint32_t)(now_ms - g_survey.started_at_ms);
    }

    if (rf_survey_has_status_data() &&
        g_survey.current_window_started_at_ms > 0 &&
        now_ms >= g_survey.current_window_started_at_ms) {
        window_elapsed_ms =
            (uint32_t)(now_ms - g_survey.current_window_started_at_ms);
    }

    int rc;
    int line_len;

    line_len = snprintf(
        g_rf_survey_status_line,
        sizeof(g_rf_survey_status_line),
        "{\"type\":\"rf_survey_status\","
        "\"request_id\":\"%s\","
        "\"active\":%s,"
        "\"state\":\"%s\","
        "\"elapsed_ms\":%u,"
        "\"window_ms\":%u,"
        "\"window_elapsed_ms\":%u,"
        "\"duration_ms\":%u,"
        "\"target_count\":%u}",
        request_id != NULL ? request_id : "",
        rf_survey_is_active() ? "true" : "false",
        rf_survey_state_name(g_survey.state),
        (unsigned int)elapsed_ms,
        (unsigned int)g_survey.window_ms,
        (unsigned int)window_elapsed_ms,
        (unsigned int)g_survey.duration_ms,
        (unsigned int)g_survey.target_count
    );

    rc = rf_survey_finish_json_line(
        g_rf_survey_status_line,
        sizeof(g_rf_survey_status_line),
        line_len
    );
    if (rc != 0) {
        return rc;
    }

    for (uint8_t i = 0; i < g_survey.target_count; i++) {
        rc = rf_survey_send_target_status(
            request_id,
            &g_survey.targets[i],
            now_ms
        );

        if (rc != 0) {
            return rc;
        }
    }

    line_len = snprintf(
        g_rf_survey_status_line,
        sizeof(g_rf_survey_status_line),
        "{\"type\":\"rf_survey_status_complete\","
        "\"request_id\":\"%s\","
        "\"target_count\":%u}",
        request_id != NULL ? request_id : "",
        (unsigned int)g_survey.target_count
    );

    return rf_survey_finish_json_line(
        g_rf_survey_status_line,
        sizeof(g_rf_survey_status_line),
        line_len
    );
}

void rf_survey_on_sensor_found(const ble_discovered_sensor_t *sensor)
{
    rf_survey_target_t *target;

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

    rf_survey_update_target_stats(
        target,
        sensor->rssi,
        k_uptime_get()
    );
}

void rf_survey_tick(void)
{
    int64_t now_ms;
    uint32_t elapsed_ms = 0;
    uint32_t window_elapsed_ms = 0;
    bool should_complete_window = false;
    bool reached_duration = false;
    bool should_emit_final_status = false;

    if (g_survey.state != RF_SURVEY_STATE_ACTIVE) {
        return;
    }

    now_ms = k_uptime_get();

    if (g_survey.started_at_ms > 0 && now_ms >= g_survey.started_at_ms) {
        elapsed_ms = (uint32_t)(now_ms - g_survey.started_at_ms);
    }

    if (g_survey.current_window_started_at_ms > 0 &&
        now_ms >= g_survey.current_window_started_at_ms) {
        window_elapsed_ms =
            (uint32_t)(now_ms - g_survey.current_window_started_at_ms);
    }

    if (g_survey.window_ms > 0 && window_elapsed_ms >= g_survey.window_ms) {
        should_complete_window = true;
    }

    if (g_survey.duration_ms > 0 && elapsed_ms >= g_survey.duration_ms) {
        reached_duration = true;
    }

    if (reached_duration && !should_complete_window) {
        should_emit_final_status = true;
    }

    if (should_complete_window && reached_duration) {
        g_survey.state = RF_SURVEY_STATE_STOPPING;
    }

    if (should_complete_window) {
        (void)rf_survey_send_status(NULL);
        if (!reached_duration) {
            rf_survey_complete_window();
        }
    }

    if (should_emit_final_status) {
        g_survey.state = RF_SURVEY_STATE_STOPPING;
        (void)rf_survey_send_status(NULL);
    }

    if (reached_duration) {
        g_survey.state = RF_SURVEY_STATE_STOPPING;
    }
}
