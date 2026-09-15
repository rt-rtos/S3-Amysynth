#include "sdkconfig.h"
#if CONFIG_SYNTH_WIRELESS

#include "radio_manager.h"
#include "ble_midi.h"
#include "midi_core.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_gatt.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radio_mgr";

/* Pre-flight thresholds: a trimmed NimBLE session costs ~20-25 KB of internal
 * heap; refuse below these so a session can never OOM the synth. Retune once
 * the real on-target numbers are measured. */
#define RADIO_MIN_FREE_INTERNAL   (40 * 1024)
#define RADIO_MIN_LARGEST_BLOCK   (16 * 1024)

/* Controller bring-up + host sync normally lands within tens of ms; a host
 * that has not synced by now never will (bad config, controller fault). */
#define RADIO_SYNC_TIMEOUT_MS     5000

enum { REQ_NONE = 0, REQ_START, REQ_STOP };

static volatile uint8_t     s_req   = REQ_NONE;
static radio_state_t        s_state = RADIO_OFF;
static const radio_hooks_t *s_hooks = 0;
static bool                 s_nvs_ready = false;
static TickType_t           s_start_tick = 0;

void radio_manager_init(const radio_hooks_t *hooks)
{
    s_hooks = hooks;
    if (hooks && hooks->peer_disconnect) {
        ble_midi_set_disconnect_cb(hooks->peer_disconnect);
    }
}

void radio_manager_request_start(void)
{
    s_req = REQ_START;
}

void radio_manager_request_stop(void)
{
    s_req = REQ_STOP;
}

radio_state_t radio_manager_state(void)
{
    return s_state;
}

bool radio_manager_connected(void)
{
    return s_state == RADIO_ACTIVE && ble_midi_connected();
}

/* NVS backs both the PHY calibration cache and BLE bond persistence. Mounted
 * lazily on the first session and left mounted. Failure is non-fatal: the
 * session still runs, bonds just do not persist - never auto-erase the
 * partition to "fix" it. */
static void radio_nvs_ensure(void)
{
    if (s_nvs_ready) return;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) {
        s_nvs_ready = true;
    } else {
        ESP_LOGW(TAG, "nvs_flash_init: %s - bonds will not persist",
                 esp_err_to_name(err));
    }
}

static bool radio_preflight_ok(void)
{
    size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "pre-flight: internal free=%u largest=%u",
             (unsigned)free_int, (unsigned)largest);
    return free_int >= RADIO_MIN_FREE_INTERNAL &&
           largest  >= RADIO_MIN_LARGEST_BLOCK;
}

static void radio_session_start(void)
{
    if (!radio_preflight_ok()) {
        ESP_LOGW(TAG, "refused: not enough internal RAM for a BLE session");
        s_state = RADIO_FAILED_RAM;
        return;
    }
    s_state = RADIO_STARTING;
    radio_nvs_ensure();
    midi_core_reset();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        s_state = RADIO_FAILED_ERR;
        return;
    }
    int rc = ble_midi_setup();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_midi_setup rc=%d", rc);
        nimble_port_deinit();
        s_state = RADIO_FAILED_ERR;
        return;
    }
    nimble_port_freertos_init(ble_midi_host_task);

    /* The stack is allocated but the host has not synced with the controller
     * yet; the session is confirmed ACTIVE from service() once ble_midi
     * reports sync (or torn down if it never arrives). */
    s_start_tick = xTaskGetTickCount();
    s_state = RADIO_STARTING;
}

static void radio_session_stop(void)
{
    s_state = RADIO_STOPPING;
    ble_midi_prepare_stop();          /* no re-advertising under teardown */
    /* Release held live notes while the ingest path is still coherent. */
    if (s_hooks && s_hooks->session_stop) s_hooks->session_stop();

    int rc = nimble_port_stop();      /* blocks until the host quiesces; the
                                       * host task then self-deletes */
    if (rc != 0) {
        /* Deinit over a host that refused to stop dereferences freed or
         * never-built stack state (ble_att_svr ctx). Leaking the session's
         * heap is the safe failure; a power cycle reclaims it. */
        ESP_LOGE(TAG, "nimble_port_stop rc=%d - stack left allocated", rc);
        s_state = RADIO_FAILED_ERR;
        return;
    }
    /* ble_hs_deinit() unregisters the ATT server's entry pool before
     * ble_gatts_stop() returns the registered attributes to it, and with
     * BT_NIMBLE_MEMPOOL_RUNTIME_ALLOC the unregister asserts that no block
     * is still out. Return them first; esp-nimble 1a714b03d moves this reset
     * into ble_att_svr_deinit(), after which the call here is a no-op. */
    rc = ble_gatts_reset();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_reset rc=%d - stack left allocated", rc);
        s_state = RADIO_FAILED_ERR;
        return;
    }
    nimble_port_deinit();             /* disables + deinits the controller,
                                       * returning the stack's heap */
    s_state = RADIO_OFF;
    ESP_LOGI(TAG, "BLE MIDI session down (internal free=%u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void radio_manager_service(void)
{
    if (s_state == RADIO_STARTING) {
        if (ble_midi_synced()) {
            if (s_hooks && s_hooks->session_start) s_hooks->session_start();
            s_state = RADIO_ACTIVE;
            ESP_LOGI(TAG, "BLE MIDI session up (internal free=%u)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        } else if ((xTaskGetTickCount() - s_start_tick) >
                   pdMS_TO_TICKS(RADIO_SYNC_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "host did not sync within %d ms - tearing down",
                     RADIO_SYNC_TIMEOUT_MS);
            radio_session_stop();
            if (s_state == RADIO_OFF) s_state = RADIO_FAILED_ERR;
        }
    }

    uint8_t req = s_req;
    if (req == REQ_NONE) return;
    s_req = REQ_NONE;

    if (req == REQ_START &&
        (s_state == RADIO_OFF || s_state == RADIO_FAILED_RAM ||
         s_state == RADIO_FAILED_ERR)) {
        radio_session_start();
    } else if (req == REQ_STOP &&
               (s_state == RADIO_ACTIVE || s_state == RADIO_STARTING)) {
        radio_session_stop();
    }
}

#endif /* CONFIG_SYNTH_WIRELESS */
