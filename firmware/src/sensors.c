#include "sensors.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static sensor_entry_t s_tab[SENSORS_MAX];
static int s_count;
static uint32_t s_gen;
static SemaphoreHandle_t s_lock;

void sensors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
}

bool sensors_add(const tpms_ford_t *pkt, const radio_rx_info_t *rx, uint32_t ms)
{
    int rssi_dbm = rx->rssi_dbm;
    bool new_burst = true;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sensor_entry_t *e = NULL;
    for (int i = 0; i < s_count; i++) {
        if (s_tab[i].pkt.id == pkt->id) {
            e = &s_tab[i];
            break;
        }
    }
    if (!e) {
        if (s_count < SENSORS_MAX) {
            e = &s_tab[s_count++];
        } else {
            /* Evict the least recently heard ID. */
            e = &s_tab[0];
            for (int i = 1; i < s_count; i++)
                if (s_tab[i].last_ms < e->last_ms) e = &s_tab[i];
        }
        memset(e, 0, sizeof(*e));
        e->first_ms = ms;
    } else if (e->packets && ms - e->last_ms < SENSORS_BURST_GAP_MS) {
        new_burst = false;
    }

    e->pkt = *pkt;
    e->rssi_dbm = (int16_t)rssi_dbm;
    if (new_burst || rssi_dbm > e->rssi_best_dbm) e->rssi_best_dbm = (int16_t)rssi_dbm;
    e->last_ms = ms;
    e->packets++;
    if (rx->quality) {
        if (!e->q_n || rx->freq_off_hz < e->foff_min_hz) e->foff_min_hz = rx->freq_off_hz;
        if (!e->q_n || rx->freq_off_hz > e->foff_max_hz) e->foff_max_hz = rx->freq_off_hz;
        e->q_n++;
        e->foff_hz = rx->freq_off_hz;
        e->foff_sum_hz += rx->freq_off_hz;
        e->lqi = rx->lqi;
        e->lqi_sum += rx->lqi;
    }
    if (new_burst) e->burst_pkts = 0;
    if (e->burst_pkts < UINT8_MAX) e->burst_pkts++;
    if (new_burst) {
        e->bursts++;
        e->published = false;
        s_gen++;
    }
    xSemaphoreGive(s_lock);
    return new_burst;
}

bool sensors_take_finished(uint32_t now_ms, sensor_entry_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !found; i++) {
        sensor_entry_t *e = &s_tab[i];
        if (!e->published && now_ms - e->last_ms > SENSORS_BURST_GAP_MS) {
            e->published = true;
            *out = *e;
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

int sensors_snapshot(sensor_entry_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    bool used[SENSORS_MAX] = {0};
    /* Selection sort by last_ms, newest first (n <= 20). */
    while (n < max && n < s_count) {
        int best = -1;
        for (int i = 0; i < s_count; i++)
            if (!used[i] && (best < 0 || s_tab[i].last_ms > s_tab[best].last_ms)) best = i;
        used[best] = true;
        out[n++] = s_tab[best];
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t sensors_generation(void)
{
    return s_gen;
}

bool sensors_get(uint32_t id, sensor_entry_t *out)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !found; i++) {
        if (s_tab[i].pkt.id == id) {
            *out = s_tab[i];
            found = true;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

int sensors_count(void)
{
    return s_count;
}

void sensors_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    s_gen++;
    xSemaphoreGive(s_lock);
}
