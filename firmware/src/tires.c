#include "tires.h"

#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "settings.h"

#define KEY "tires"

static uint32_t s_ids[TIRE_COUNT];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *const k_short[TIRE_COUNT] = {"FL", "FR", "RL", "RR"};
static const char *const k_long[TIRE_COUNT] = {"Front Left", "Front Right", "Rear Left", "Rear Right"};

void tires_init(void)
{
    if (!settings_load(KEY, s_ids, sizeof(s_ids))) {
        for (int i = 0; i < TIRE_COUNT; i++) s_ids[i] = TIRE_NONE;
    }
}

uint32_t tires_get(tire_pos_t pos)
{
    return pos < TIRE_COUNT ? s_ids[pos] : TIRE_NONE;
}

int tires_find(uint32_t id)
{
    if (id == TIRE_NONE) return -1;
    for (int i = 0; i < TIRE_COUNT; i++)
        if (s_ids[i] == id) return i;
    return -1;
}

void tires_set(tire_pos_t pos, uint32_t id, uint32_t *old_id, int *old_pos)
{
    if (pos >= TIRE_COUNT) return;
    uint32_t copy[TIRE_COUNT];
    taskENTER_CRITICAL(&s_mux);
    int prev = tires_find(id);
    if (old_id) *old_id = s_ids[pos];
    if (old_pos) *old_pos = prev;
    if (prev >= 0) s_ids[prev] = TIRE_NONE;
    s_ids[pos] = id;
    for (int i = 0; i < TIRE_COUNT; i++) copy[i] = s_ids[i];
    taskEXIT_CRITICAL(&s_mux);
    settings_save(KEY, copy, sizeof(copy));
}

const char *tires_short_name(tire_pos_t pos)
{
    return pos < TIRE_COUNT ? k_short[pos] : "?";
}

const char *tires_long_name(tire_pos_t pos)
{
    return pos < TIRE_COUNT ? k_long[pos] : "?";
}

int tires_parse(const char *s)
{
    for (int i = 0; i < TIRE_COUNT; i++)
        if (!strcasecmp(s, k_short[i])) return i;
    return -1;
}
