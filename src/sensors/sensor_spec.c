#include "sensor_spec.h"
#include "movella_dot_spec.h"
#include <string.h>

const sensor_spec_t *sensor_spec_get(sensor_type_t sensor_type)
{
    switch (sensor_type) {
    case SENSOR_TYPE_MOVELLA_DOT:
        return movella_dot_get_spec();
    default:
        return NULL;
    }
}

bool sensor_spec_matches_advertisement(
    const sensor_spec_t *spec,
    const char *name,
    const char **service_uuids,
    size_t service_uuid_count
)
{
    if (spec == NULL) {
        return false;
    }

    if (spec->advertising.name_contains != NULL && name != NULL) {
        if (strstr(name, spec->advertising.name_contains) != NULL) {
            return true;
        }
    }

    if (spec->advertising.service_uuid_hint != NULL) {
        for (size_t i = 0; i < service_uuid_count; i++) {
            if (service_uuids[i] != NULL &&
                strcmp(service_uuids[i], spec->advertising.service_uuid_hint) == 0) {
                return true;
            }
        }
    }

    return false;
}