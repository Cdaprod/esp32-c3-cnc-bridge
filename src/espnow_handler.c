#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "sdkconfig.h"
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include "config.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include "espnow_handler.h"
#include "espnow_config.h"
#include "espnow_protocol.h"
#include "espnow_decode.h"
#include "serial_handler.h"
#include "tcp_server.h"
#include "my_logs.h"
#include "nvs_storage.h"
#include "mpg_motion.h"
#include "mpg_grbl_status.h"
#include "mpg_grbl_ack.h"

#ifdef CONFIG_MPG_ENABLE_MOTION
#define MPG_ENABLE_MOTION CONFIG_MPG_ENABLE_MOTION
#else
#define MPG_ENABLE_MOTION 0
#endif
#if defined(CONFIG_MPG_DRY_RUN)
#define MPG_DRY_RUN CONFIG_MPG_DRY_RUN
#else
/* Missing configuration must not compile in the MPG command path. */
#define MPG_DRY_RUN 0
#endif
#ifndef CONFIG_MPG_ALLOW_A_AXIS_MOTION
#define CONFIG_MPG_ALLOW_A_AXIS_MOTION 0
#endif
/* Configured using idf.py menuconfig, not firmware-source substitutions. */
#if MPG_ENABLE_MOTION || MPG_DRY_RUN
static uint8_t mpg_trusted_peer[6] = {0};
static bool mpg_trusted_ready = false;
static bool mpg_provision_peer(void)
{
    const char *mac = CONFIG_MPG_TRUSTED_MAC;
    unsigned int octets[6];
    int consumed = 0;
    if (sscanf(mac, "%2x:%2x:%2x:%2x:%2x:%2x%n",
               &octets[0], &octets[1], &octets[2], &octets[3],
               &octets[4], &octets[5], &consumed) != 6 ||
        consumed != 17 || mac[consumed] != '\0')
        return false;
    bool any_nonzero = false, all_broadcast = true;
    for (int i = 0; i < 6; ++i) {
        mpg_trusted_peer[i] = (uint8_t)octets[i];
        if (octets[i] != 0) any_nonzero = true;
        if (octets[i] != 255) all_broadcast = false;
    }
    return any_nonzero && !all_broadcast &&
           !(mpg_trusted_peer[0] & 1); /* reject multicast MAC */
}
#endif
typedef struct {
    const uint8_t *mac;
    uint32_t session_id;
    uint16_t sequence;
    char axis;
    int32_t microns;
    uint16_t feed;
} mpg_dry_run_log_context_t;

typedef struct {
    uint32_t session_id;
    uint16_t sequence;
} mpg_control_log_context_t;

static mpg_motion_lease_t mpg_lease = {0};
static mpg_grbl_status_t mpg_grbl = {0};
static mpg_grbl_ack_t mpg_pending_grbl_ack = {0};
static bool mpg_rearm_wait_idle = false;
static uint32_t mpg_rearm_after_ms = 0;
static uint32_t mpg_grbl_status_log_ms = 0;
static uint32_t mpg_status_query_log_ms = 0;
static SemaphoreHandle_t mpg_mutex = NULL;
static char mpg_line[128];
static size_t mpg_line_len = 0;
static bool mpg_line_overflow = false;
static void mpg_finish_grbl_ack(const mpg_grbl_ack_t *completed,
                                mpg_grbl_ack_result_t result,
                                uint8_t error_code, bool cancel_motion);

static int mpg_uart_cancel_writer(uint8_t cancel, void *context)
{
    (void)context;
    return uart_write_bytes(UART_NUM, (const char *)&cancel, 1);
}

static mpg_output_result_t mpg_stop_uart(void)
{
    mpg_output_result_t result = mpg_motion_output_cancel(
        MPG_ENABLE_MOTION && !MPG_DRY_RUN,
        mpg_uart_cancel_writer, NULL);
    if (result == MPG_OUTPUT_SENT) {
        mpg_rearm_wait_idle = true;
        mpg_rearm_after_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    return result;
}

static int mpg_uart_writer(const char *command, size_t length, void *context)
{
    if (!serial_uart_mpg_owned()) return -1;
    const int written = uart_write_bytes(UART_NUM, command, (uint32_t)length);
    const mpg_dry_run_log_context_t *event = context;
    if (event && written == (int)length) {
        ESP_LOGI("MPG", "event=uart_tx operation=mpg_jog source=uart0 result=accepted bytes=%u session=%lu seq=%u",
                 (unsigned)length, (unsigned long)event->session_id,
                 (unsigned)event->sequence);
    }
    return written;
}

static bool mpg_dry_run_logger(const char *command, void *context)
{
    const mpg_dry_run_log_context_t *event = context;
    if (!command || !event || !event->mac) return false;

    /* Hex keeps the exact G-code recoverable as one parseable log field. */
    static const char digits[] = "0123456789abcdef";
    char command_hex[129];
    size_t length = 0;
    while (command[length] && length < 64) length++;
    if (length == 0 || length >= 64 || length * 2 >= sizeof(command_hex))
        return false;
    for (size_t i = 0; i < length; i++) {
        const uint8_t value = (uint8_t)command[i];
        command_hex[i * 2] = digits[value >> 4];
        command_hex[i * 2 + 1] = digits[value & 0x0F];
    }
    command_hex[length * 2] = '\0';
    ESP_LOGI("MPG", "event=dry_run result=proposed mac=" MACSTR
             " session=%lu seq=%u axis=%c microns=%ld feed=%u command_hex=%s",
             MAC2STR(event->mac), (unsigned long)event->session_id,
             (unsigned)event->sequence, event->axis, (long)event->microns,
             (unsigned)event->feed, command_hex);
    return true;
}

#if MPG_DRY_RUN
static bool mpg_feed_hold_dry_run_logger(const char *command, void *context)
{
    if (!command || strcmp(command, "!") != 0) return false;
    const mpg_control_log_context_t *event = context;
    if (!event) return false;
    ESP_LOGI("MPG", "event=mpg_feed_hold result=dry_run_proposed byte=0x21 physical_uart=not_sent session=%lu seq=%u",
        (unsigned long)event->session_id, (unsigned)event->sequence);
    return true;
}
#endif

static void mpg_telemetry_ingest(const uint8_t *data, int len)
{
    if (!data || len <= 0 || !mpg_mutex) return;
    mpg_grbl_ack_t completed = {0};
    mpg_grbl_ack_result_t completion = MPG_GRBL_ACK_NONE;
    uint8_t error_code = 0;
    char status_log[sizeof(mpg_line)];
    bool status_log_pending = false;
    xSemaphoreTake(mpg_mutex, portMAX_DELAY);
    for (int i = 0; i < len; ++i) {
        const char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (!mpg_line_overflow && mpg_line_len) {
                const uint32_t line_time_ms =
                    (uint32_t)(esp_timer_get_time() / 1000);
                char previous[sizeof(mpg_grbl.line)];
                memcpy(previous, mpg_grbl.line, sizeof(previous));
                previous[sizeof(previous) - 1] = '\0';
                const bool had_status = mpg_grbl.seen;
                const bool valid_status = mpg_grbl_status_parse(
                    &mpg_grbl, mpg_line, mpg_line_len, line_time_ms);
                if (valid_status) {
                    const char *old_sep = strchr(previous, '|');
                    const char *new_sep = strchr(mpg_grbl.line, '|');
                    const size_t old_state_len = old_sep ?
                        (size_t)(old_sep - previous) : 0;
                    const size_t new_state_len = new_sep ?
                        (size_t)(new_sep - mpg_grbl.line) : 0;
                    const bool state_changed = !had_status ||
                        old_state_len != new_state_len ||
                        (new_state_len && memcmp(previous, mpg_grbl.line,
                                                 new_state_len) != 0);
                    if (state_changed || mpg_grbl_status_log_ms == 0 ||
                        (uint32_t)(line_time_ms - mpg_grbl_status_log_ms) >= 1000) {
                        memcpy(status_log, mpg_line, mpg_line_len);
                        status_log[mpg_line_len] = '\0';
                        status_log_pending = true;
                        mpg_grbl_status_log_ms = line_time_ms;
                    }
                }
                if (valid_status && mpg_rearm_wait_idle && mpg_grbl.idle &&
                    (uint32_t)(line_time_ms - mpg_rearm_after_ms) >= 250) {
                    mpg_rearm_wait_idle = false;
                    ESP_LOGI("MPG", "event=motion_rearm result=idle_status_after_stop");
                }
                if (completion == MPG_GRBL_ACK_NONE)
                    completion = mpg_grbl_ack_consume_line(
                        &mpg_pending_grbl_ack, mpg_line, mpg_line_len,
                        (uint32_t)(esp_timer_get_time() / 1000),
                        &completed, &error_code);
            }
            mpg_line_len = 0;
            mpg_line_overflow = false;
        } else if (!mpg_line_overflow) {
            if (mpg_line_len < sizeof(mpg_line))
                mpg_line[mpg_line_len++] = c;
            else
                mpg_line_overflow = true;
        }
    }
    xSemaphoreGive(mpg_mutex);
    if (status_log_pending) {
        ESP_LOGI("GRBL", "event=grbl_status source=uart0 age_ms=0 line=%s",
                 status_log);
    }
    if (completion != MPG_GRBL_ACK_NONE)
        mpg_finish_grbl_ack(&completed, completion, error_code,
                            completion != MPG_GRBL_ACK_ACCEPTED);
}

static void mpg_watchdog_task(void *unused)
{
    (void)unused;
    uint32_t last_query_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        if (!mpg_mutex) continue;
        mpg_output_result_t cancel_result = MPG_OUTPUT_SUPPRESSED;
        bool lease_ended = false;
        mpg_grbl_ack_t completed_ack = {0};
        mpg_grbl_ack_result_t ack_result = MPG_GRBL_ACK_NONE;
        xSemaphoreTake(mpg_mutex, portMAX_DELAY);
        const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        ack_result = mpg_grbl_ack_expire(&mpg_pending_grbl_ack, now,
                                         &completed_ack);
        if (ack_result != MPG_GRBL_ACK_NONE) {
            mpg_motion_revoke(&mpg_lease);
            cancel_result = mpg_stop_uart();
            serial_uart_mpg_release();
            lease_ended = true;
        }
        if (mpg_motion_expired(&mpg_lease, now) ||
            (mpg_lease.active && tcp_server_get_client_count() > 0)) {
            mpg_motion_revoke(&mpg_lease);
            /* Keep the MPG mutex through cancellation and ownership release;
             * otherwise a new lease could race between revoke and 0x85. */
            cancel_result = mpg_stop_uart();
            serial_uart_mpg_release();
            lease_ended = true;
        }
        xSemaphoreGive(mpg_mutex);
        if (ack_result != MPG_GRBL_ACK_NONE)
            mpg_finish_grbl_ack(&completed_ack, ack_result, 0, false);
        if ((uint32_t)(now - last_query_ms) >= 250) {
            const char status_query = '?';
            const int written = uart_write_bytes(UART_NUM, &status_query, 1);
            if (written == 1) {
                last_query_ms = now;
                if (mpg_status_query_log_ms == 0 ||
                    (uint32_t)(now - mpg_status_query_log_ms) >= 1000) {
                    ESP_LOGI("GRBL", "event=uart_tx operation=status_query source=uart0 bytes=1 result=accepted");
                    mpg_status_query_log_ms = now;
                }
            } else {
                ESP_LOGW("GRBL", "event=uart_tx operation=status_query source=uart0 bytes=1 result=failed");
                last_query_ms = now;
            }
        }
        if (lease_ended) {
            if (cancel_result == MPG_OUTPUT_SENT) {
                ESP_LOGW("MPG", "MPG lease expired: GRBL jog cancel sent");
            } else if (cancel_result == MPG_OUTPUT_SUPPRESSED) {
                ESP_LOGI("MPG", "MPG lease expired: controller cancel suppressed by disarmed/dry-run mode");
            } else if (cancel_result == MPG_OUTPUT_FAILED) {
                ESP_LOGE("MPG", "MPG lease expired: GRBL jog cancel output failed");
            }
        }
    }
}


static const char *TAG = "ESPNOW";

static int find_peer_index_locked(const uint8_t *mac);

#define ESPNOW_RX_QUEUE_SIZE       20
#define ESPNOW_LEGACY_RX_QUEUE_SIZE 20
#define ESPNOW_LEGACY_PACKET_MAX   ESPNOW_MAX_FRAME_SIZE

typedef enum {
    ESPNOW_PEER_PROTOCOL_UNKNOWN = 0,
    ESPNOW_PEER_PROTOCOL_LEGACY,
    ESPNOW_PEER_PROTOCOL_NEW
} espnow_peer_protocol_t;

typedef struct {
    uint8_t mac[6];
    bool active;
    int64_t last_activity_ms;
    int64_t last_ping_ms;
    uint8_t ping_retries;

    espnow_peer_protocol_t protocol;

    // Sequence state for the new protocol.
    uint32_t last_rx_session;
    uint16_t last_rx_seq;
    bool rx_seq_valid;
    uint8_t last_rx_result;
    mpg_motion_sequence_t mpg_sequence;

    // Sequence state for the legacy pendant protocol.
    uint16_t legacy_last_seq;
    bool legacy_seq_valid;
} espnow_peer_t;

typedef espnow_decoded_packet_t espnow_rx_packet_t;

typedef struct {
    uint8_t mac[6];
    uint16_t seq;
    uint16_t payload_len;
    uint8_t payload[ESPNOW_LEGACY_PACKET_MAX - 4];
} espnow_legacy_rx_packet_t;

static espnow_peer_t peers[MAX_ESPNOW_PEERS] = {0};
static SemaphoreHandle_t peers_mutex = NULL;
static QueueHandle_t espnow_rx_queue = NULL;
static QueueHandle_t espnow_legacy_rx_queue = NULL;
static bool espnow_initialized = false;
static int espnow_channel = 11;
static bool output_state[ESPNOW_GPIO_COUNT] = {0};

// MODIFICA 2: handle delle due task RX
static TaskHandle_t espnow_rx_task_handle = NULL;
static TaskHandle_t espnow_legacy_rx_task_handle = NULL;

static const char *const espnow_output_names[ESPNOW_GPIO_COUNT] = {
    "PROBE", "XLIMIT", "YLIMIT", "ZLIMIT", "CUSTOM1", "CUSTOM2", "CUSTOM3"
};

static void espnow_peer_monitor_task(void *pvParameters);
static void espnow_rx_task(void *pvParameters);
static void espnow_legacy_rx_task(void *pvParameters);

static bool is_zero_mac(const uint8_t *mac)
{
    if (!mac) return true;
    for (int i = 0; i < 6; i++)
        if (mac[i] != 0) return false;
    return true;
}

static int sensor_function_for_mac_locked(const uint8_t *mac)
{
    const bridge_config_t *cfg = nvs_storage_get_config();
    for (int f = 0; f < ESPNOW_GPIO_COUNT; f++) {
        if (!(cfg->espnow_sensor_binding_mask & (1u << f))) continue;
        if (memcmp(cfg->espnow_sensor_macs[f], mac, 6) == 0)
            return f;
    }
    return -1;
}

static bool sensor_is_bound_to_function(const uint8_t *mac, uint8_t function)
{
    if (!mac || function >= ESPNOW_GPIO_COUNT || !peers_mutex) return false;
    bool ok = false;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    if (idx >= 0 && peers[idx].protocol == ESPNOW_PEER_PROTOCOL_NEW) {
        const bridge_config_t *cfg = nvs_storage_get_config();
        ok = (cfg->espnow_sensor_binding_mask & (1u << function)) &&
             memcmp(cfg->espnow_sensor_macs[function], mac, 6) == 0;
    }
    xSemaphoreGive(peers_mutex);
    return ok;
}

static int get_wifi_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0)
        return primary;
    return 11;
}

static bool is_allowed_output_pin(int pin)
{
    return pin == 4 || pin == 5 || pin == 6 || pin == 7 || pin == 10;
}

static int find_peer_index_locked(const uint8_t *mac)
{
    for (int i = 0; i < MAX_ESPNOW_PEERS; i++) {
        if (peers[i].active && memcmp(peers[i].mac, mac, 6) == 0)
            return i;
    }
    return -1;
}

static int find_peer_index(const uint8_t *mac)
{
    if (!peers_mutex) return -1;
    int idx;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    idx = find_peer_index_locked(mac);
    xSemaphoreGive(peers_mutex);
    return idx;
}

static int add_peer_to_list(const uint8_t *mac, espnow_peer_protocol_t protocol)
{
    int result = -1;
    int64_t now = esp_timer_get_time() / 1000;

    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int existing = find_peer_index_locked(mac);
    if (existing >= 0) {
        peers[existing].last_activity_ms = now;
        peers[existing].ping_retries = 0;
        peers[existing].last_ping_ms = 0;
        if (protocol != ESPNOW_PEER_PROTOCOL_UNKNOWN)
            peers[existing].protocol = protocol;
        xSemaphoreGive(peers_mutex);
        return existing;
    }

    for (int i = 0; i < MAX_ESPNOW_PEERS; i++) {
        if (!peers[i].active) {
            memset(&peers[i], 0, sizeof(peers[i]));
            memcpy(peers[i].mac, mac, 6);
            peers[i].active = true;
            peers[i].last_activity_ms = now;
            peers[i].protocol = protocol;
            result = i;
            break;
        }
    }
    xSemaphoreGive(peers_mutex);

    if (result >= 0)
        ESP_LOGI(TAG, "Peer aggiunto: " MACSTR " (slot %d, protocollo=%s)",
                 MAC2STR(mac), result,
                 protocol == ESPNOW_PEER_PROTOCOL_LEGACY ? "legacy" :
                 protocol == ESPNOW_PEER_PROTOCOL_NEW ? "new" : "unknown");
    else
        ESP_LOGW(TAG, "Limite peer raggiunto (%d)", MAX_ESPNOW_PEERS);
    return result;
}

static bool ensure_radio_peer(const uint8_t *mac)
{
    if (esp_now_is_peer_exist(mac))
        return true;

    esp_now_peer_info_t info = {0};
    memcpy(info.peer_addr, mac, 6);
    info.channel = espnow_channel;
    info.ifidx = WIFI_IF_STA;
    info.encrypt = false;

    esp_err_t err = esp_now_add_peer(&info);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST)
        return true;

    ESP_LOGW(TAG, "Impossibile aggiungere peer " MACSTR ": %s",
             MAC2STR(mac), esp_err_to_name(err));
    return false;
}

static void clear_peer_locked(int idx)
{
    memset(&peers[idx], 0, sizeof(peers[idx]));
}

static void mark_peer_activity(const uint8_t *mac)
{
    if (!peers_mutex) return;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    if (idx >= 0) {
        peers[idx].last_activity_ms = esp_timer_get_time() / 1000;
        peers[idx].ping_retries = 0;
        peers[idx].last_ping_ms = 0;
    }
    xSemaphoreGive(peers_mutex);
}

static void espnow_outputs_init(void)
{
    const bridge_config_t *cfg = nvs_storage_get_config();
    memset(output_state, 0, sizeof(output_state));

    for (int i = 0; i < ESPNOW_GPIO_COUNT; i++) {
        int pin = cfg->espnow_output_pins[i];
        if (is_allowed_output_pin(pin) && pin != cfg->state_pin) {
            gpio_reset_pin((gpio_num_t)pin);
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
            // Logical OFF always means inactive electrical level.
            // Active LOW -> OFF = HIGH; Active HIGH -> OFF = LOW.
            const bool active_low =
                (cfg->espnow_gpio_active_low_mask & (1UL << pin)) != 0;
            gpio_set_level((gpio_num_t)pin, active_low ? 1 : 0);
            ESP_LOGI(TAG, "ESP-NOW output %s -> GPIO%d (OFF)",
                     espnow_output_names[i], pin);
        }
    }
}

static bool build_packet(uint8_t type, uint32_t session_id, uint16_t seq,
                         const uint8_t *payload, uint16_t payload_len,
                         uint8_t *packet, size_t packet_capacity, size_t *packet_len)
{
    if (!packet || !packet_len || payload_len > ESPNOW_MAX_PAYLOAD)
        return false;

    size_t total = ESPNOW_HEADER_SIZE + payload_len;
    if (total > packet_capacity || total > 250)
        return false;

    packet[0] = ESPNOW_PROTOCOL_VERSION;
    packet[1] = type;
    packet[2] = (uint8_t)(session_id & 0xFF);
    packet[3] = (uint8_t)((session_id >> 8) & 0xFF);
    packet[4] = (uint8_t)((session_id >> 16) & 0xFF);
    packet[5] = (uint8_t)((session_id >> 24) & 0xFF);
    packet[6] = (uint8_t)(seq & 0xFF);
    packet[7] = (uint8_t)(seq >> 8);
    packet[8] = (uint8_t)(payload_len & 0xFF);
    packet[9] = (uint8_t)(payload_len >> 8);

    if (payload_len && payload)
        memcpy(packet + ESPNOW_HEADER_SIZE, payload, payload_len);

    *packet_len = total;
    return true;
}

static bool parse_packet(const uint8_t *data, int data_len,
                         espnow_rx_packet_t *out, const uint8_t *mac)
{
    return espnow_decode_frame(data, data_len, mac, out);
}

static void send_simple_message(const uint8_t *mac, uint8_t type,
                                uint32_t session_id, uint16_t seq,
                                const uint8_t *payload, uint16_t payload_len)
{
    uint8_t packet[250];
    size_t packet_len = 0;
    if (!build_packet(type, session_id, seq, payload, payload_len,
                      packet, sizeof(packet), &packet_len))
        return;

    esp_err_t err = esp_now_send(mac, packet, packet_len);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "ESP-NOW send type=0x%02X fallito: %s",
                 type, esp_err_to_name(err));
}

static bool mpg_ack_peer_session_active(const mpg_grbl_ack_t *completed,
                                        uint8_t result)
{
    bool active = false;
    if (!completed || !peers_mutex) return false;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(completed->mac);
    active = idx >= 0 && peers[idx].active &&
        peers[idx].last_rx_session == completed->session;
    if (active) {
        mpg_motion_sequence_result(&peers[idx].mpg_sequence,
            completed->session, completed->sequence,
            result);
        if (peers[idx].last_rx_session == completed->session &&
            peers[idx].last_rx_seq == completed->sequence)
            peers[idx].last_rx_result = result;
    }
    xSemaphoreGive(peers_mutex);
    return active;
}

static void mpg_finish_grbl_ack(const mpg_grbl_ack_t *completed,
                                mpg_grbl_ack_result_t result,
                                uint8_t error_code, bool cancel_motion)
{
    if (!completed || result == MPG_GRBL_ACK_NONE) return;
    if (cancel_motion && mpg_mutex) {
        xSemaphoreTake(mpg_mutex, portMAX_DELAY);
        mpg_motion_revoke(&mpg_lease);
        const mpg_output_result_t stopped = mpg_stop_uart();
        serial_uart_mpg_release();
        xSemaphoreGive(mpg_mutex);
        if (stopped == MPG_OUTPUT_FAILED)
            ESP_LOGE("MPG", "event=grbl_jog_cancel result=failed session=%lu seq=%u",
                (unsigned long)completed->session, (unsigned)completed->sequence);
    }

    const bool accepted = result == MPG_GRBL_ACK_ACCEPTED;
    const uint8_t wire_result = accepted ? ESPNOW_RESULT_OK : ESPNOW_RESULT_ERROR;
    const bool current = mpg_ack_peer_session_active(completed, wire_result);
    ESP_LOGI("MPG", "event=grbl_jog_ack result=%s reason=%s session=%lu seq=%u error=%u peer_current=%u",
        accepted ? "accepted" : "rejected",
        result == MPG_GRBL_ACK_ACCEPTED ? "grbl_ok" :
        result == MPG_GRBL_ACK_REJECTED ? "grbl_error" :
        result == MPG_GRBL_ACK_TIMED_OUT ? "grbl_timeout" : "cancelled",
        (unsigned long)completed->session, (unsigned)completed->sequence,
        (unsigned)error_code, current ? 1u : 0u);
    if (current)
        send_simple_message(completed->mac, ESPNOW_MSG_MPG_JOG_ACK,
            completed->session, completed->sequence, &wire_result, 1);
}

static void send_gpio_ack(const uint8_t *mac, uint32_t session_id, uint16_t seq,
                          uint8_t function, uint8_t state, uint8_t result)
{
    uint8_t payload[3] = {function, state, result};
    send_simple_message(mac, ESPNOW_MSG_GPIO_ACK, session_id, seq,
                        payload, sizeof(payload));
}

static void send_sensor_assignment(const uint8_t *mac, uint32_t session_id,
                                   uint16_t seq, uint8_t function)
{
    uint8_t payload[1] = { function };
    send_simple_message(mac, ESPNOW_MSG_SENSOR_ASSIGNMENT, session_id, seq,
                        payload, sizeof(payload));
}

static void send_gpio_status(const uint8_t *mac, uint32_t session_id, uint16_t seq)
{
    const bridge_config_t *cfg = nvs_storage_get_config();
    uint16_t configured_mask = 0;
    uint16_t state_mask = 0;

    for (int i = 0; i < ESPNOW_GPIO_COUNT; i++) {
        int pin = cfg->espnow_output_pins[i];
        if (is_allowed_output_pin(pin) && pin != cfg->state_pin) {
            configured_mask |= (uint16_t)(1u << i);
            if (output_state[i])
                state_mask |= (uint16_t)(1u << i);
        }
    }

    uint8_t payload[4] = {
        (uint8_t)(configured_mask & 0xFF),
        (uint8_t)(configured_mask >> 8),
        (uint8_t)(state_mask & 0xFF),
        (uint8_t)(state_mask >> 8)
    };
    send_simple_message(mac, ESPNOW_MSG_GPIO_STATUS, session_id, seq,
                        payload, sizeof(payload));
}

static bool seq_is_newer(uint16_t seq, uint16_t last_seq)
{
    return (int16_t)(seq - last_seq) > 0;
}

static bool peer_accepts_sequence(const espnow_rx_packet_t *packet,
                                  uint8_t *duplicate_result)
{
    bool reject_or_duplicate = false;
    *duplicate_result = ESPNOW_RESULT_ERROR;

    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(packet->mac);
    if (idx < 0 || peers[idx].last_rx_session != packet->session_id) {
        /* The active pairing session is authoritative. */
        reject_or_duplicate = true;
    } else if (peers[idx].rx_seq_valid &&
               !seq_is_newer(packet->seq, peers[idx].last_rx_seq)) {
        reject_or_duplicate = true;
        if (packet->seq == peers[idx].last_rx_seq)
            *duplicate_result = peers[idx].last_rx_result;
    }
    xSemaphoreGive(peers_mutex);
    return reject_or_duplicate;
}

static void remember_sequence(const espnow_rx_packet_t *packet, uint8_t result)
{
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(packet->mac);
    if (idx >= 0) {
        peers[idx].last_rx_session = packet->session_id;
        peers[idx].last_rx_seq = packet->seq;
        peers[idx].rx_seq_valid = true;
        peers[idx].last_rx_result = result;
        mpg_motion_sequence_result(&peers[idx].mpg_sequence,
            packet->session_id, packet->seq, result);
    }
    xSemaphoreGive(peers_mutex);
}

static bool peer_accepts_mpg_sequence(const espnow_rx_packet_t *packet,
                                      uint8_t *cached_result)
{
    bool reject_or_duplicate = true;
    *cached_result = ESPNOW_RESULT_ERROR;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(packet->mac);
    if (idx >= 0) {
        mpg_sequence_result_t accepted = mpg_motion_sequence_accept(
            &peers[idx].mpg_sequence, packet->mac,
            peers[idx].last_rx_session, packet->session_id, packet->seq,
            cached_result);
        reject_or_duplicate = accepted != MPG_SEQUENCE_NEW;
        if (accepted == MPG_SEQUENCE_REJECT_SESSION ||
            accepted == MPG_SEQUENCE_REJECT_STALE)
            *cached_result = ESPNOW_RESULT_ERROR;
    }
    xSemaphoreGive(peers_mutex);
    return reject_or_duplicate;
}

static void reset_new_sequence(const uint8_t *mac, uint32_t session_id,
                               uint16_t initial_sequence)
{
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    if (idx >= 0) {
        /* A retransmitted PAIR in the current session must not reopen its
         * sequence window; only a genuinely new session resets baselines. */
        if (peers[idx].last_rx_session != session_id) {
            peers[idx].last_rx_session = session_id;
            peers[idx].last_rx_seq = 0;
            peers[idx].rx_seq_valid = false;
            peers[idx].last_rx_result = ESPNOW_RESULT_ERROR;
            mpg_motion_sequence_reset(&peers[idx].mpg_sequence, mac,
                                      session_id, initial_sequence);
        }
    }
    xSemaphoreGive(peers_mutex);
}

static bool legacy_sequence_is_duplicate(const uint8_t *mac, uint16_t seq)
{
    bool duplicate = false;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    if (idx >= 0 && peers[idx].legacy_seq_valid && peers[idx].legacy_last_seq == seq)
        duplicate = true;
    xSemaphoreGive(peers_mutex);
    return duplicate;
}

static void remember_legacy_sequence(const uint8_t *mac, uint16_t seq)
{
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    if (idx >= 0) {
        peers[idx].legacy_last_seq = seq;
        peers[idx].legacy_seq_valid = true;
    }
    xSemaphoreGive(peers_mutex);
}

static void reset_legacy_sequence(const uint8_t *mac)
{
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    if (idx >= 0) {
        peers[idx].legacy_last_seq = 0;
        peers[idx].legacy_seq_valid = false;
    }
    xSemaphoreGive(peers_mutex);
}

static bool process_gpio_command(const espnow_rx_packet_t *packet,
                                 uint8_t *function, uint8_t *state,
                                 uint8_t *result)
{
    if (packet->payload_len != 2)
        return false;

    *function = packet->payload[0];
    *state = packet->payload[1];

    if (*function >= ESPNOW_GPIO_COUNT || *state > 1) {
        *result = ESPNOW_RESULT_ERROR;
        return true;
    }

    const bridge_config_t *cfg = nvs_storage_get_config();

    // I comandi GPIO del nuovo protocollo sono accettati solo dal SENSOR
    // esplicitamente associato alla funzione. I pendant legacy restano
    // completamente fuori da questa logica.
    if (!sensor_is_bound_to_function(packet->mac, *function)) {
        ESP_LOGW(TAG, "GPIO %s rifiutato: sensore " MACSTR " non associato",
                 espnow_output_names[*function], MAC2STR(packet->mac));
        *result = ESPNOW_RESULT_ERROR;
        return true;
    }

    int pin = cfg->espnow_output_pins[*function];
    if (!is_allowed_output_pin(pin) || pin == cfg->state_pin) {
        ESP_LOGW(TAG, "GPIO function %u non configurata", *function);
        *result = ESPNOW_RESULT_ERROR;
        return true;
    }

    // 1=ON and 0=OFF are logical states. The GPIO polarity is
    // applied only at the final electrical output level.
    const bool active_low =
        (cfg->espnow_gpio_active_low_mask & (1UL << pin)) != 0;
    const int physical_level =
        active_low ? (*state ? 0 : 1) : (*state ? 1 : 0);

    esp_err_t err = gpio_set_level((gpio_num_t)pin, physical_level);
    if (err != ESP_OK) {
        *result = ESPNOW_RESULT_ERROR;
        return true;
    }

    output_state[*function] = (*state != 0);
    ESP_LOGI(TAG, "ESP-NOW GPIO %s -> GPIO%d %s",
             espnow_output_names[*function], pin, *state ? "ON" : "OFF");
    *result = ESPNOW_RESULT_OK;
    return true;
}

static void espnow_rx_task(void *pvParameters)
{
    espnow_rx_packet_t packet;

    while (1) {
        if (xQueueReceive(espnow_rx_queue, &packet, portMAX_DELAY) != pdTRUE)
            continue;

        if (find_peer_index(packet.mac) < 0)
            continue;

        mark_peer_activity(packet.mac);

        switch (packet.type) {
        case ESPNOW_MSG_PAIR:
            // PAIR is handled in the callback because it must be accepted from
            // an unknown peer. The task only receives post-pair messages.
            break;

        case ESPNOW_MSG_GPIO_COMMAND: {
            uint8_t duplicate_result = ESPNOW_RESULT_ERROR;
            if (peer_accepts_sequence(&packet, &duplicate_result)) {
                uint8_t function = packet.payload_len > 0 ? packet.payload[0] : 0;
                uint8_t state = packet.payload_len > 1 ? packet.payload[1] : 0;
                send_gpio_ack(packet.mac, packet.session_id, packet.seq,
                              function, state, duplicate_result);
                break;
            }

            uint8_t function = 0;
            uint8_t state = 0;
            uint8_t result = ESPNOW_RESULT_ERROR;
            if (!process_gpio_command(&packet, &function, &state, &result))
                result = ESPNOW_RESULT_ERROR;

            remember_sequence(&packet, result);
            send_gpio_ack(packet.mac, packet.session_id, packet.seq,
                          function, state, result);
            break;
        }

        case ESPNOW_MSG_GPIO_STATUS_REQ:
            send_gpio_status(packet.mac, packet.session_id, packet.seq);
            break;

        case ESPNOW_MSG_SENSOR_ASSIGNMENT:
            // Direzione prevista: bridge -> sensor. Non accettare questo
            // messaggio come comando proveniente dal peer.
            break;

        case ESPNOW_MSG_MPG_JOG: {
            uint8_t result = ESPNOW_RESULT_ERROR;
            uint8_t duplicate_result = ESPNOW_RESULT_ERROR;
            bool defer_ack = false;
            bool sequence_remembered = false;
            bool ack_pending = false;
            bool is_pending_duplicate = false;
            if (mpg_mutex) {
                xSemaphoreTake(mpg_mutex, portMAX_DELAY);
                ack_pending = mpg_pending_grbl_ack.active;
                is_pending_duplicate = mpg_grbl_ack_matches(
                    &mpg_pending_grbl_ack, packet.mac,
                    packet.session_id, packet.seq);
                xSemaphoreGive(mpg_mutex);
            }
            if (ack_pending) {
                /* Never queue or replay a second jog while GRBL has not
                 * acknowledged the first. A retransmission of the exact
                 * in-flight packet waits for its original completion. */
                if (is_pending_duplicate) break;
                if (peer_accepts_mpg_sequence(&packet, &duplicate_result)) {
                    send_simple_message(packet.mac, ESPNOW_MSG_MPG_JOG_ACK,
                        packet.session_id, packet.seq, &duplicate_result, 1);
                } else {
                    remember_sequence(&packet, ESPNOW_RESULT_ERROR);
                    ESP_LOGW("MPG", "intent rejected reason=grbl_ack_pending MAC=" MACSTR
                        " session=0x%08lX seq=%u", MAC2STR(packet.mac),
                        (unsigned long)packet.session_id, (unsigned)packet.seq);
                    send_simple_message(packet.mac, ESPNOW_MSG_MPG_JOG_ACK,
                        packet.session_id, packet.seq, &result, 1);
                }
                break;
            }
            if (peer_accepts_mpg_sequence(&packet, &duplicate_result)) {
                send_simple_message(packet.mac, ESPNOW_MSG_MPG_JOG_ACK,
                                    packet.session_id, packet.seq,
                                    &duplicate_result, 1);
                break;
            }
#if MPG_ENABLE_MOTION || MPG_DRY_RUN
            /*
             * This path only works with a locally provisioned six-byte MAC,
             * a current GRBL Idle report and a live receiver motion lease.
             * Actual machine admission is opt-in at compile time.
             */
            const bool trusted = mpg_trusted_ready &&
                memcmp(packet.mac, mpg_trusted_peer, 6) == 0;
            const char *reject_reason = NULL;
            if (trusted && mpg_mutex && packet.payload_len == 8 &&
                tcp_server_get_client_count() == 0 &&
                serial_uart_tx_idle()) {
                const char axis = (char)packet.payload[0];
                const bool deadman = packet.payload[1] == 1;
                const uint32_t bits = (uint32_t)packet.payload[2] |
                    ((uint32_t)packet.payload[3] << 8) |
                    ((uint32_t)packet.payload[4] << 16) |
                    ((uint32_t)packet.payload[5] << 24);
                const int32_t microns = (int32_t)bits;
                const uint16_t feed = (uint16_t)packet.payload[6] |
                                      ((uint16_t)packet.payload[7] << 8);
                char cmd[64];
                const bool physical_motion =
                    MPG_ENABLE_MOTION && !MPG_DRY_RUN;
                if (!mpg_motion_axis_allowed(axis, physical_motion,
                        CONFIG_MPG_ALLOW_A_AXIS_MOTION))
                    reject_reason = axis == 'A' ? "a_axis_not_verified" :
                                                  "unsupported_axis";
                const uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                xSemaphoreTake(mpg_mutex, portMAX_DELAY);
                const bool fresh_idle =
                    mpg_grbl_status_fresh_idle(&mpg_grbl, now, 250);
                /* While an owned lease is active GRBL may report Jog;
                 * only that same MAC/session may extend it. */
                const bool owned_jog =
                    mpg_lease.active &&
                    memcmp(mpg_lease.owner, packet.mac, 6) == 0 &&
                    mpg_lease.session == packet.session_id &&
                    mpg_grbl.seen && !mpg_grbl.alarm &&
                    (uint32_t)(now - mpg_grbl.updated_ms) <= 250 &&
                    strncmp(mpg_grbl.line, "<Jog|", 5) == 0;
                const bool fresh = (fresh_idle || owned_jog) &&
                    !mpg_rearm_wait_idle;
#if MPG_ENABLE_MOTION && !MPG_DRY_RUN
                bool newly_claimed_uart = false;
                bool uart_owner_ready = serial_uart_mpg_owned();
                if (!reject_reason && !uart_owner_ready && !mpg_lease.active) {
                    uart_owner_ready = serial_uart_mpg_claim();
                    newly_claimed_uart = uart_owner_ready;
                }
                if (!reject_reason && !uart_owner_ready)
                    reject_reason = "controller_uart_owned";
#endif
                if (!reject_reason && fresh && mpg_motion_format_jog(cmd, sizeof(cmd), axis,
                                                    microns, feed)) {
                    mpg_motion_result_t admitted = mpg_motion_admit(
                        &mpg_lease, packet.mac, packet.session_id, packet.seq,
                        trusted, deadman, fresh, now, 250);
                    if (admitted == MPG_MOTION_OK) {
                        mpg_dry_run_log_context_t dry_run_event = {
                            .mac = packet.mac,
                            .session_id = packet.session_id,
                            .sequence = packet.seq,
                            .axis = axis,
                            .microns = microns,
                            .feed = feed,
                        };
#if MPG_ENABLE_MOTION && !MPG_DRY_RUN
                        const bool pending_started = mpg_grbl_ack_begin(
                            &mpg_pending_grbl_ack, packet.mac,
                            packet.session_id, packet.seq, now,
                            MPG_GRBL_ACK_TIMEOUT_MS);
                        if (!pending_started) {
                            mpg_motion_revoke(&mpg_lease);
                            serial_uart_mpg_release();
                            reject_reason = "grbl_ack_slot_busy";
                        }
#endif
                        const mpg_output_result_t output =
                            reject_reason ? MPG_OUTPUT_FAILED :
                            mpg_motion_output_command(cmd, MPG_DRY_RUN,
                                mpg_uart_writer, mpg_dry_run_logger,
                                &dry_run_event);
                        if (output == MPG_OUTPUT_SENT ||
                            output == MPG_OUTPUT_DRY_RUN) {
                            result = ESPNOW_RESULT_OK;
#if MPG_ENABLE_MOTION && !MPG_DRY_RUN
                            defer_ack = true;
                            /* Reserve the sequence result before releasing
                             * mpg_mutex, so an exceptionally fast GRBL RX
                             * cannot race the deferred completion. */
                            remember_sequence(&packet, ESPNOW_RESULT_ERROR);
                            sequence_remembered = true;
#endif
                            ESP_LOGI("MPG", "event=motion_admission result=accepted mode=%s mac=" MACSTR
                                " session=%lu seq=%u axis=%c microns=%ld feed=%u",
                                MPG_DRY_RUN ? "dry_run" : "uart",
                                MAC2STR(packet.mac),
                                (unsigned long)packet.session_id,
                                (unsigned)packet.seq, axis, (long)microns, (unsigned)feed);
                        } else {
                            mpg_grbl_ack_cancel(&mpg_pending_grbl_ack, NULL);
                            mpg_motion_revoke(&mpg_lease);
                            mpg_stop_uart();
                            serial_uart_mpg_release();
                            if (!reject_reason)
                                reject_reason = "command_output_failed";
                            ESP_LOGE("MPG", "intent rejected: %s", reject_reason);
                        }
                    } else if (admitted == MPG_MOTION_EXPIRED) {
                        mpg_stop_uart();
                        serial_uart_mpg_release();
                        reject_reason = "motion_lease_expired";
                    } else {
                        reject_reason = mpg_rearm_wait_idle ?
                            "post_stop_idle_required" : deadman ?
                            "motion_admission_denied" :
                            "deadman_released";
                    }
                } else {
                    if (!reject_reason)
                        reject_reason = mpg_rearm_wait_idle ?
                            "post_stop_idle_required" : fresh ?
                            "invalid_jog_parameters" :
                                                "grbl_state_stale_or_not_idle";
                }
#if MPG_ENABLE_MOTION && !MPG_DRY_RUN
                if (newly_claimed_uart && !mpg_lease.active)
                    serial_uart_mpg_release();
#endif
                xSemaphoreGive(mpg_mutex);
            } else {
                reject_reason = !trusted ? "untrusted_peer" :
                    packet.payload_len != 8 ? "invalid_payload_length" :
                    tcp_server_get_client_count() != 0 ? "other_controller_connected" :
                    !serial_uart_tx_idle() ? "uart_busy" : "motion_state_unavailable";
            }
            if (reject_reason) {
                ESP_LOGW("MPG", "intent rejected reason=%s MAC=" MACSTR
                    " session=0x%08lX seq=%u", reject_reason,
                    MAC2STR(packet.mac), (unsigned long)packet.session_id,
                    (unsigned)packet.seq);
            }
#endif
            if (!sequence_remembered)
                remember_sequence(&packet,
                    defer_ack ? ESPNOW_RESULT_ERROR : result);
            if (!defer_ack)
                send_simple_message(packet.mac, ESPNOW_MSG_MPG_JOG_ACK,
                                    packet.session_id, packet.seq, &result, 1);
            break;
        }

        case ESPNOW_MSG_MPG_STOP: {
            /*
             * GRBL 1.1 realtime jog cancel is byte 0x85, without a newline.
             * It must bypass the normal TX queue so an enabled physical
             * cancel cannot wait behind queued G-code. The shared mode gate
             * suppresses this output in disarmed and dry-run builds.
             */
            mpg_output_result_t cancel_result;
            mpg_grbl_ack_t cancelled_ack = {0};
            mpg_grbl_ack_result_t pending_cancelled = MPG_GRBL_ACK_NONE;
            if (mpg_mutex) {
                xSemaphoreTake(mpg_mutex, portMAX_DELAY);
                pending_cancelled = mpg_grbl_ack_cancel(
                    &mpg_pending_grbl_ack, &cancelled_ack);
                mpg_motion_revoke(&mpg_lease);
                cancel_result = mpg_stop_uart();
                serial_uart_mpg_release();
                xSemaphoreGive(mpg_mutex);
            } else {
                cancel_result = mpg_stop_uart();
            }
            if (pending_cancelled != MPG_GRBL_ACK_NONE)
                mpg_finish_grbl_ack(&cancelled_ack, pending_cancelled, 0, false);
            const uint8_t result = mpg_motion_stop_ack_result(cancel_result);
            if (cancel_result == MPG_OUTPUT_SUPPRESSED) {
                ESP_LOGW("MPG", "event=mpg_stop result=suppressed physical_uart=not_sent mode=%s lease=revoked session=%lu seq=%u",
                         MPG_DRY_RUN ? "dry_run" : "disarmed",
                         (unsigned long)packet.session_id,
                         (unsigned)packet.seq);
            } else if (cancel_result == MPG_OUTPUT_SENT) {
                ESP_LOGI("MPG", "event=mpg_stop result=sent physical_uart=0x85 session=%lu seq=%u",
                         (unsigned long)packet.session_id,
                         (unsigned)packet.seq);
            } else if (cancel_result == MPG_OUTPUT_FAILED) {
                ESP_LOGE("MPG", "event=mpg_stop result=failed reason=stop_output_failed session=%lu seq=%u",
                         (unsigned long)packet.session_id,
                         (unsigned)packet.seq);
            }
            send_simple_message(packet.mac, ESPNOW_MSG_MPG_STOP_ACK,
                                packet.session_id, packet.seq, &result, 1);
            break;
        }

        case ESPNOW_MSG_MPG_FEED_HOLD: {
            uint8_t result = ESPNOW_RESULT_ERROR;
            uint8_t duplicate_result = ESPNOW_RESULT_ERROR;
            if (peer_accepts_mpg_sequence(&packet, &duplicate_result)) {
                send_simple_message(packet.mac,
                    ESPNOW_MSG_MPG_FEED_HOLD_ACK, packet.session_id,
                    packet.seq, &duplicate_result, 1);
                break;
            }
#if MPG_ENABLE_MOTION || MPG_DRY_RUN
            const bool trusted = mpg_trusted_ready &&
                memcmp(packet.mac, mpg_trusted_peer, 6) == 0;
            const char *reject_reason = NULL;
            if (!trusted)
                reject_reason = "untrusted_peer";
            else if (tcp_server_get_client_count() != 0)
                reject_reason = "other_controller_connected";
            else if (!serial_uart_tx_idle())
                reject_reason = "uart_busy";
            else if (!mpg_mutex)
                reject_reason = "motion_state_unavailable";

            if (!reject_reason) {
                mpg_grbl_ack_t cancelled_ack = {0};
                mpg_grbl_ack_result_t pending_cancelled = MPG_GRBL_ACK_NONE;
                xSemaphoreTake(mpg_mutex, portMAX_DELAY);
                pending_cancelled = mpg_grbl_ack_cancel(
                    &mpg_pending_grbl_ack, &cancelled_ack);
                mpg_motion_revoke(&mpg_lease);
#if MPG_ENABLE_MOTION && !MPG_DRY_RUN
                /* Claiming the shared arbiter prevents a concurrent TCP
                 * writer from racing this one-byte GRBL realtime command. */
                if (!serial_uart_mpg_claim()) {
                    reject_reason = "uart_busy";
                } else {
                    const mpg_output_result_t output =
                        mpg_motion_output_command("!", false,
                            mpg_uart_writer, NULL, NULL);
                    serial_uart_mpg_release();
                    if (output == MPG_OUTPUT_SENT) {
                        result = ESPNOW_RESULT_OK;
                        mpg_rearm_wait_idle = true;
                        mpg_rearm_after_ms = (uint32_t)(esp_timer_get_time() / 1000);
                        ESP_LOGI("MPG", "event=mpg_feed_hold result=uart_write_accepted byte=0x21 session=%lu seq=%u",
                            (unsigned long)packet.session_id,
                            (unsigned)packet.seq);
                    } else {
                        reject_reason = "command_output_failed";
                    }
                }
#elif MPG_DRY_RUN
                const mpg_control_log_context_t dry_run_event = {
                    .session_id = packet.session_id,
                    .sequence = packet.seq,
                };
                if (mpg_motion_output_command("!", true, mpg_uart_writer,
                        mpg_feed_hold_dry_run_logger,
                        (void *)&dry_run_event) ==
                        MPG_OUTPUT_DRY_RUN)
                    result = ESPNOW_RESULT_SUPPRESSED;
                else
                    reject_reason = "dry_run_log_failed";
#else
                reject_reason = "motion_disabled";
#endif
                xSemaphoreGive(mpg_mutex);
                if (pending_cancelled != MPG_GRBL_ACK_NONE)
                    mpg_finish_grbl_ack(&cancelled_ack,
                        pending_cancelled, 0, false);
            }
            if (reject_reason) {
                ESP_LOGW("MPG", "event=mpg_feed_hold result=rejected reason=%s session=%lu seq=%u",
                    reject_reason, (unsigned long)packet.session_id,
                    (unsigned)packet.seq);
            }
#else
            ESP_LOGW("MPG", "event=mpg_feed_hold result=rejected reason=motion_disabled session=%lu seq=%u",
                (unsigned long)packet.session_id, (unsigned)packet.seq);
#endif
            remember_sequence(&packet, result);
            send_simple_message(packet.mac, ESPNOW_MSG_MPG_FEED_HOLD_ACK,
                                packet.session_id, packet.seq, &result, 1);
            break;
        }

        case ESPNOW_MSG_UART_COMMAND: {
            /*
             * The upstream bridge forwarded arbitrary, unauthenticated
             * UART_COMMAND bytes directly to GRBL. Radio pairing and the
             * sensor GPIO function assignment do not authorize CNC motion.
             * Reject this legacy escape hatch until a dedicated authorized
             * MPG command handler, UART arbitration, and receiver-side
             * timeout cancellation are integrated and validated.
             */
            uint8_t result = ESPNOW_RESULT_ERROR;
            send_simple_message(packet.mac, ESPNOW_MSG_UART_ACK,
                                packet.session_id, packet.seq, &result, 1);
            ESP_LOGW(TAG, "Unsafe v1 UART_COMMAND rejected from " MACSTR,
                     MAC2STR(packet.mac));
            break;
        }

        case ESPNOW_MSG_HEARTBEAT:
            send_simple_message(packet.mac, ESPNOW_MSG_HEARTBEAT_ACK,
                                packet.session_id, packet.seq, NULL, 0);
            break;

        default:
            ESP_LOGW(TAG, "Messaggio ESP-NOW non gestito: type=0x%02X",
                     packet.type);
            break;
        }
    }
}

static void espnow_legacy_rx_task(void *pvParameters)
{
    espnow_legacy_rx_packet_t packet;

    while (1) {
        if (xQueueReceive(espnow_legacy_rx_queue, &packet, portMAX_DELAY) != pdTRUE)
            continue;

        if (find_peer_index(packet.mac) < 0)
            continue;

        mark_peer_activity(packet.mac);

        /*
         * Legacy packets carry unbounded raw G-code without receiver-side
         * lease/authorization. Keep pairing compatibility, but do not allow
         * this route to bypass the dedicated MPG admission handler.
         */
        ESP_LOGW(TAG, "Unsafe legacy ESP-NOW UART packet rejected from " MACSTR,
                 MAC2STR(packet.mac));
    }
}

static bool parse_legacy_command(const uint8_t *data, int data_len,
                                 uint16_t *seq, uint16_t *payload_len)
{
    if (!data || data_len < 5 || !seq || !payload_len)
        return false;

    *seq = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    *payload_len = (uint16_t)data[2] | ((uint16_t)data[3] << 8);

    if (*payload_len == 0 || *payload_len > ESPNOW_MAX_PAYLOAD)
        return false;
    if ((int)*payload_len > data_len - 4)
        return false;

    return true;
}

static bool enqueue_legacy_command(const uint8_t *mac, uint16_t seq,
                                   const uint8_t *payload, uint16_t payload_len)
{
    if (!espnow_legacy_rx_queue || !mac || !payload || payload_len == 0 ||
        payload_len > sizeof(((espnow_legacy_rx_packet_t *)0)->payload))
        return false;

    espnow_legacy_rx_packet_t packet = {0};
    memcpy(packet.mac, mac, 6);
    packet.seq = seq;
    packet.payload_len = payload_len;
    memcpy(packet.payload, payload, payload_len);

    if (xQueueSend(espnow_legacy_rx_queue, &packet, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Legacy RX queue piena, scarto seq=%u", seq);
        return false;
    }
    return true;
}

static void on_espnow_recv_cb(const esp_now_recv_info_t *recv_info,
                              const uint8_t *data, int data_len)
{
    if (!recv_info || !data || data_len <= 0)
        return;

    const uint8_t *mac = recv_info->src_addr;

    // -------------------------------------------------------------------------
    // LEGACY PENDANT PROTOCOL - DO NOT CHANGE
    // -------------------------------------------------------------------------
    if (data_len == 4 && memcmp(data, CMD_PAIR, 4) == 0) {
        ESP_LOGI(TAG, "Legacy PAIR da " MACSTR, MAC2STR(mac));

        if (ensure_radio_peer(mac) && add_peer_to_list(mac,
                ESPNOW_PEER_PROTOCOL_LEGACY) >= 0) {
            reset_legacy_sequence(mac);
            reset_new_sequence(mac, 0, 0);
            esp_now_send(mac, (const uint8_t *)CMD_PAIR_OK,
                         strlen(CMD_PAIR_OK));
        }
        return;
    }

    if (data_len == 4 && memcmp(data, CMD_PONG, 4) == 0) {
        int idx = find_peer_index(mac);
        if (idx >= 0) {
            xSemaphoreTake(peers_mutex, portMAX_DELAY);
            idx = find_peer_index_locked(mac);
            if (idx >= 0) {
                peers[idx].protocol = ESPNOW_PEER_PROTOCOL_LEGACY;
                peers[idx].last_activity_ms = esp_timer_get_time() / 1000;
                peers[idx].ping_retries = 0;
                peers[idx].last_ping_ms = 0;
            }
            xSemaphoreGive(peers_mutex);
        }
        return;
    }

    uint16_t legacy_seq = 0;
    uint16_t legacy_len = 0;
    bool legacy_valid = parse_legacy_command(data, data_len,
                                             &legacy_seq, &legacy_len);

    // Strict new-protocol parsing. A valid new packet is recognized before
    // legacy handling, except for the explicit legacy PAIR/PONG strings above.
    espnow_rx_packet_t new_packet;
    bool new_valid = parse_packet(data, data_len, &new_packet, mac);

    if (new_valid) {
        if (new_packet.type == ESPNOW_MSG_PAIR) {
            // Il PAIR del nuovo sensore contiene il proprio MAC nei primi 6 byte.
            // Questo permette al bridge di distinguere un SENSOR dal protocollo
            // legacy senza modificare in alcun modo i pendant esistenti.
            if (new_packet.payload_len < 6 ||
                memcmp(new_packet.payload, mac, 6) != 0) {
                ESP_LOGW(TAG, "Nuovo PAIR rifiutato: MAC payload non corrisponde al mittente");
                return;
            }
            if (!ensure_radio_peer(mac))
                return;
            if (add_peer_to_list(mac, ESPNOW_PEER_PROTOCOL_NEW) < 0)
                return;

            /* Seed the motion window from PAIR itself. After a C3 reboot, an
             * S3 that has been running for hours may already have a sequence
             * above half-range relative to zero; resetting to zero would
             * reject fresh packets as stale until the uint16 counter wrapped.
             */
            reset_new_sequence(mac, new_packet.session_id, new_packet.seq);
            // PAIR_ACK per il nuovo protocollo: oltre al tipo/versione,
            // conferma esplicitamente quale sensore il bridge ha riconosciuto
            // e quale funzione gli risulta associata.
            // Payload: [bridge_type][protocol_version][sensor_mac x6][function]
            // function 0..6 = PROBE..CUSTOM3, 0xFF = non assegnato.
            const bridge_config_t *cfg = nvs_storage_get_config();
            uint8_t assigned_function = 0xFF;
            for (int f = 0; f < ESPNOW_GPIO_COUNT; f++) {
                if ((cfg->espnow_sensor_binding_mask & (1u << f)) &&
                    memcmp(cfg->espnow_sensor_macs[f], mac, 6) == 0) {
                    assigned_function = (uint8_t)f;
                    break;
                }
            }

            uint8_t ack_payload[9] = {
                ESPNOW_DEVICE_TYPE_BRIDGE,
                ESPNOW_PROTOCOL_VERSION,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                assigned_function
            };
            send_simple_message(mac, ESPNOW_MSG_PAIR_ACK,
                                new_packet.session_id, 0,
                                ack_payload, sizeof(ack_payload));

            // Invia al nuovo SENSOR l'associazione attuale.
            // 0xFF significa non assegnato.
            send_sensor_assignment(mac, new_packet.session_id, 0,
                                   assigned_function);

            ESP_LOGI(TAG, "PAIR v%d accettato da " MACSTR
                     " session=0x%08lX",
                     ESPNOW_PROTOCOL_VERSION, MAC2STR(mac),
                     (unsigned long)new_packet.session_id);
            return;
        }

        if (find_peer_index(mac) < 0)
            return;

        // A legacy pendant is never fed the new protocol by the bridge, but a
        // peer can explicitly identify itself through a new PAIR to migrate.
        xSemaphoreTake(peers_mutex, portMAX_DELAY);
        int idx = find_peer_index_locked(mac);
        if (idx >= 0)
            peers[idx].protocol = ESPNOW_PEER_PROTOCOL_NEW;
        xSemaphoreGive(peers_mutex);

        if (!espnow_rx_queue)
            return;

        if (xQueueSend(espnow_rx_queue, &new_packet, 0) != pdTRUE)
            ESP_LOGW(TAG, "RX queue piena, scarto type=0x%02X seq=%u",
                     new_packet.type, new_packet.seq);
        return;
    }

    // Legacy ACKs are generated by the pendant and ignored by the bridge.
    if (data_len == 3)
        return;

    // Legacy command packet: [SEQ little endian][LEN little endian][payload].
    if (legacy_valid && find_peer_index(mac) >= 0) {
        mark_peer_activity(mac);

        xSemaphoreTake(peers_mutex, portMAX_DELAY);
        int idx = find_peer_index_locked(mac);
        if (idx >= 0)
            peers[idx].protocol = ESPNOW_PEER_PROTOCOL_LEGACY;
        xSemaphoreGive(peers_mutex);

        ESP_LOGI(TAG, "G-code legacy ricevuto da " MACSTR
                 ": seq=%u, len=%u", MAC2STR(mac), legacy_seq, legacy_len);

        // Preserve the original ACK format and timing: ACK is sent immediately,
        // before the command is forwarded to UART.
        uint8_t ack[3] = {
            data[0], data[1], 0x01
        };
        esp_now_send(mac, ack, sizeof(ack));

        // Avoid executing the same legacy packet twice if the pendant retries
        // after an ACK was already accepted.
        if (legacy_sequence_is_duplicate(mac, legacy_seq))
            return;

        remember_legacy_sequence(mac, legacy_seq);
        enqueue_legacy_command(mac, legacy_seq, &data[4], legacy_len);
        return;
    }

    ESP_LOGD(TAG, "Pacchetto ESP-NOW non riconosciuto da " MACSTR ", len=%d",
             MAC2STR(mac), data_len);
}

static void espnow_broadcast_to_peers(const uint8_t *data, int len)
{
    if (!data || len <= 0 || !peers_mutex)
        return;

    uint8_t macs[MAX_ESPNOW_PEERS][6];
    int count = 0;

    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_ESPNOW_PEERS; i++) {
        if (peers[i].active) {
            memcpy(macs[count], peers[i].mac, 6);
            count++;
        }
    }
    xSemaphoreGive(peers_mutex);

    for (int i = 0; i < count; i++) {
        esp_err_t err = esp_now_send(macs[i], data, len);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "Broadcast verso peer %d fallito: %s",
                     i, esp_err_to_name(err));
    }
}

int espnow_get_peer_count(void)
{
    int count = 0;
    if (!peers_mutex) return 0;

    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_ESPNOW_PEERS; i++)
        if (peers[i].active) count++;
    xSemaphoreGive(peers_mutex);
    return count;
}

bool espnow_is_paired(void)
{
    return espnow_get_peer_count() > 0;
}

void espnow_clear_all_peers(void)
{
    uint8_t macs[MAX_ESPNOW_PEERS][6];
    int count = 0;

    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_ESPNOW_PEERS; i++) {
        if (peers[i].active) {
            memcpy(macs[count], peers[i].mac, 6);
            count++;
            clear_peer_locked(i);
        }
    }
    xSemaphoreGive(peers_mutex);

    for (int i = 0; i < count; i++) {
        if (esp_now_is_peer_exist(macs[i]))
            esp_now_del_peer(macs[i]);
    }

    ESP_LOGI(TAG, "Tutti i peer rimossi");
}

static void espnow_peer_monitor_task(void *pvParameters)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time() / 1000;

        uint8_t ping_macs[MAX_ESPNOW_PEERS][6];
        espnow_peer_protocol_t ping_protocol[MAX_ESPNOW_PEERS];
        int ping_count = 0;

        uint8_t remove_macs[MAX_ESPNOW_PEERS][6];
        int remove_count = 0;

        xSemaphoreTake(peers_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_ESPNOW_PEERS; i++) {
            if (!peers[i].active) continue;

            int64_t idle = now - peers[i].last_activity_ms;
            if (peers[i].ping_retries == 0 && idle > ESPNOW_PING_IDLE_MS) {
                memcpy(ping_macs[ping_count], peers[i].mac, 6);
                ping_protocol[ping_count] = peers[i].protocol;
                ping_count++;
                peers[i].ping_retries = 1;
                peers[i].last_ping_ms = now;
                continue;
            }

            if (peers[i].ping_retries > 0 &&
                now - peers[i].last_ping_ms >= ESPNOW_PING_INTERVAL_MS) {
                if (peers[i].ping_retries < ESPNOW_PING_MAX_RETRIES) {
                    memcpy(ping_macs[ping_count], peers[i].mac, 6);
                    ping_protocol[ping_count] = peers[i].protocol;
                    ping_count++;
                    peers[i].ping_retries++;
                    peers[i].last_ping_ms = now;
                } else {
                    memcpy(remove_macs[remove_count++], peers[i].mac, 6);
                    clear_peer_locked(i);
                }
            }
        }
        xSemaphoreGive(peers_mutex);

        for (int i = 0; i < ping_count; i++) {
            if (ping_protocol[i] == ESPNOW_PEER_PROTOCOL_LEGACY) {
                // Existing pendants require the original ASCII PING/PONG protocol.
                esp_now_send(ping_macs[i], (const uint8_t *)CMD_PING, 4);
            } else if (ping_protocol[i] == ESPNOW_PEER_PROTOCOL_NEW) {
                send_simple_message(ping_macs[i], ESPNOW_MSG_HEARTBEAT,
                                    0, 0, NULL, 0);
            } else {
                // Unknown peers are probed using the legacy heartbeat, which is
                // harmless for existing pendants and avoids sending new frames
                // before a protocol has been established.
                esp_now_send(ping_macs[i], (const uint8_t *)CMD_PING, 4);
            }
        }

        for (int i = 0; i < remove_count; i++) {
            if (esp_now_is_peer_exist(remove_macs[i]))
                esp_now_del_peer(remove_macs[i]);
            ESP_LOGW(TAG, "Peer " MACSTR " rimosso per timeout",
                     MAC2STR(remove_macs[i]));
        }
    }
}

int espnow_get_sensor_list(espnow_sensor_info_t *out, int max_items)
{
    if (!out || max_items <= 0 || !peers_mutex) return 0;
    int count = 0;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_ESPNOW_PEERS && count < max_items; i++) {
        if (!peers[i].active || peers[i].protocol != ESPNOW_PEER_PROTOCOL_NEW)
            continue;
        memcpy(out[count].mac, peers[i].mac, 6);
        out[count].function = (int8_t)sensor_function_for_mac_locked(peers[i].mac);
        count++;
    }
    xSemaphoreGive(peers_mutex);
    return count;
}

bool espnow_get_sensor_binding(uint8_t function, uint8_t mac[6])
{
    if (!mac || function >= ESPNOW_GPIO_COUNT) return false;
    const bridge_config_t *cfg = nvs_storage_get_config();
    if (!(cfg->espnow_sensor_binding_mask & (1u << function)) ||
        is_zero_mac(cfg->espnow_sensor_macs[function])) {
        memset(mac, 0, 6);
        return false;
    }
    memcpy(mac, cfg->espnow_sensor_macs[function], 6);
    return true;
}

bool espnow_assign_sensor(uint8_t function, const uint8_t mac[6])
{
    if (function >= ESPNOW_GPIO_COUNT || !mac || is_zero_mac(mac)) return false;
    if (!peers_mutex) return false;

    uint32_t session_id = 0;
    xSemaphoreTake(peers_mutex, portMAX_DELAY);
    int idx = find_peer_index_locked(mac);
    bool is_sensor = (idx >= 0 && peers[idx].protocol == ESPNOW_PEER_PROTOCOL_NEW);
    if (is_sensor)
        session_id = peers[idx].last_rx_session;
    xSemaphoreGive(peers_mutex);

    if (!is_sensor) {
        ESP_LOGW(TAG, "Impossibile associare SENSOR " MACSTR ": non presente come peer NEW", MAC2STR(mac));
        return false;
    }

    bridge_config_t *cfg = nvs_storage_get_config_mutable();
    // Un SENSOR puo' avere una sola funzione.
    for (int f = 0; f < ESPNOW_GPIO_COUNT; f++) {
        if (f != function && (cfg->espnow_sensor_binding_mask & (1u << f)) &&
            memcmp(cfg->espnow_sensor_macs[f], mac, 6) == 0) {
            ESP_LOGW(TAG, "SENSOR " MACSTR " gia' associato a %s",
                     MAC2STR(mac), espnow_output_names[f]);
            return false;
        }
    }

    memcpy(cfg->espnow_sensor_macs[function], mac, 6);
    cfg->espnow_sensor_binding_mask |= (uint8_t)(1u << function);
    if (!nvs_storage_save_config()) {
        ESP_LOGE(TAG, "Errore salvataggio associazione SENSOR -> %s", espnow_output_names[function]);
        return false;
    }

    // Notifica immediata al SENSOR online. La radio API viene chiamata senza
    // mantenere il mutex dei peer.
    send_sensor_assignment(mac, session_id, 0, function);

    ESP_LOGI(TAG, "SENSOR " MACSTR " associato a %s", MAC2STR(mac), espnow_output_names[function]);
    return true;
}

bool espnow_unassign_sensor(uint8_t function)
{
    if (function >= ESPNOW_GPIO_COUNT) return false;

    uint8_t mac[6] = {0};
    bool had_binding = false;
    uint32_t session_id = 0;

    const bridge_config_t *before = nvs_storage_get_config();
    if (before->espnow_sensor_binding_mask & (1u << function)) {
        memcpy(mac, before->espnow_sensor_macs[function], 6);
        had_binding = !is_zero_mac(mac);
    }

    if (had_binding && peers_mutex) {
        xSemaphoreTake(peers_mutex, portMAX_DELAY);
        int idx = find_peer_index_locked(mac);
        if (idx >= 0 && peers[idx].protocol == ESPNOW_PEER_PROTOCOL_NEW)
            session_id = peers[idx].last_rx_session;
        xSemaphoreGive(peers_mutex);
    }

    bridge_config_t *cfg = nvs_storage_get_config_mutable();
    memset(cfg->espnow_sensor_macs[function], 0, 6);
    cfg->espnow_sensor_binding_mask &= (uint8_t)~(1u << function);
    if (!nvs_storage_save_config())
        return false;

    if (had_binding)
        send_sensor_assignment(mac, session_id, 0, 0xFF);

    return true;
}

void espnow_init_handler(void)
{
    if (espnow_initialized) return;

    peers_mutex = xSemaphoreCreateMutex();
    if (!peers_mutex) {
        ESP_LOGE(TAG, "Impossibile creare mutex peer");
        return;
    }

    espnow_channel = get_wifi_channel();
    ESP_LOGI(TAG, "Canale ESP-NOW: %d", espnow_channel);
    esp_wifi_set_channel(espnow_channel, WIFI_SECOND_CHAN_NONE);

    // MODIFICA 1: gestione errore esp_now_init() con cleanup del mutex
    esp_err_t espnow_init_err = esp_now_init();
    if (espnow_init_err != ESP_OK) {
        ESP_LOGE(TAG, "Errore ESP-NOW: %s",
                 esp_err_to_name(espnow_init_err));
        vSemaphoreDelete(peers_mutex);
        peers_mutex = NULL;
        return;
    }

    mpg_mutex = xSemaphoreCreateMutex();
    if (!mpg_mutex) {
        ESP_LOGE(TAG, "MPG watchdog mutex allocation failed");
        esp_now_deinit();
        return;
    }
#if MPG_ENABLE_MOTION || MPG_DRY_RUN
    mpg_trusted_ready = mpg_provision_peer();
    if (!mpg_trusted_ready) {
        ESP_LOGW(TAG, "MPG diagnostics fail-closed: trusted peer MAC not provisioned");
    }
    ESP_LOGI(TAG, "MPG mode=%s motion_enabled=%d",
             MPG_DRY_RUN ? "DRY_RUN" : "UART",
             MPG_ENABLE_MOTION ? 1 : 0);
#endif
    espnow_rx_queue = xQueueCreate(ESPNOW_RX_QUEUE_SIZE,
                                    sizeof(espnow_rx_packet_t));
    espnow_legacy_rx_queue = xQueueCreate(ESPNOW_LEGACY_RX_QUEUE_SIZE,
                                          sizeof(espnow_legacy_rx_packet_t));
    if (!espnow_rx_queue || !espnow_legacy_rx_queue) {
        ESP_LOGE(TAG, "Impossibile creare le RX queue ESP-NOW");
        if (espnow_rx_queue) {
            vQueueDelete(espnow_rx_queue);
            espnow_rx_queue = NULL;
        }
        if (espnow_legacy_rx_queue) {
            vQueueDelete(espnow_legacy_rx_queue);
            espnow_legacy_rx_queue = NULL;
        }
        esp_now_deinit();
        return;
    }

    esp_now_register_recv_cb(on_espnow_recv_cb);
    espnow_outputs_init();

    // MODIFICA 3: creazione delle due task RX con handle dedicati
    BaseType_t rx_task_result =
        xTaskCreate(espnow_rx_task,
                    "espnow_rx",
                    4096,
                    NULL,
                    5,
                    &espnow_rx_task_handle);

    BaseType_t legacy_rx_task_result =
        xTaskCreate(espnow_legacy_rx_task,
                    "espnow_legacy_rx",
                    3072,
                    NULL,
                    5,
                    &espnow_legacy_rx_task_handle);

    if (rx_task_result != pdPASS ||
        legacy_rx_task_result != pdPASS) {
        ESP_LOGE(TAG, "Impossibile creare le RX task ESP-NOW");
    }

    uint8_t broadcast_mac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_peer_info_t broadcast_peer = {0};
    memcpy(broadcast_peer.peer_addr, broadcast_mac, 6);
    broadcast_peer.channel = espnow_channel;
    broadcast_peer.ifidx = WIFI_IF_STA;
    broadcast_peer.encrypt = false;
    esp_err_t add_broadcast = esp_now_add_peer(&broadcast_peer);
    if (add_broadcast != ESP_OK && add_broadcast != ESP_ERR_ESPNOW_EXIST)
        ESP_LOGW(TAG, "Broadcast peer non aggiunto: %s",
                 esp_err_to_name(add_broadcast));

    const bridge_config_t *cfg = nvs_storage_get_config();
    if (cfg->client_mode != 1)
        xTaskCreate(espnow_peer_monitor_task, "espnow_monitor", 3072,
                    NULL, 3, NULL);

    if (xTaskCreate(mpg_watchdog_task, "mpg_watchdog", 3072,
                    NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "CRITICAL: MPG watchdog failed; motion disabled");
#if MPG_ENABLE_MOTION
        abort();
#endif
    }
    espnow_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW attivo sul canale %d, nuovo protocollo v%d, max peer=%d",
             espnow_channel, ESPNOW_PROTOCOL_VERSION, MAX_ESPNOW_PEERS);
    ESP_LOGI(TAG, "Compatibilita' protocollo legacy pendant attiva");
}

void espnow_send_uart_response(const uint8_t *data, int len)
{
    if (!data || len <= 0) return;
    mpg_telemetry_ingest(data, len);
    if (espnow_get_peer_count() == 0) return;

    int send_len = len;
    if (send_len > ESPNOW_MAX_PAYLOAD)
        send_len = ESPNOW_MAX_PAYLOAD;

    /* Preserve raw responses for older peers while providing a framed
     * response for the new CircuitPython C3 client. */
    uint8_t newer[MAX_ESPNOW_PEERS][6];
    uint32_t sessions[MAX_ESPNOW_PEERS];
    int newer_count = 0;
    if (peers_mutex) {
        xSemaphoreTake(peers_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_ESPNOW_PEERS; ++i) {
            if (peers[i].active && peers[i].protocol == ESPNOW_PEER_PROTOCOL_NEW) {
                memcpy(newer[newer_count], peers[i].mac, 6);
                sessions[newer_count] = peers[i].last_rx_session;
                newer_count++;
            }
        }
        xSemaphoreGive(peers_mutex);
    }
    for (int i = 0; i < newer_count; ++i)
        send_simple_message(newer[i], ESPNOW_MSG_UART_RESPONSE,
                            sessions[i], 0, data, (uint16_t)send_len);
    espnow_broadcast_to_peers(data, send_len);
}
