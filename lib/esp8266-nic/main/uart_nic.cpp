/* UART NIC

  This code makes ESP WiFi device accessible to external system using UART.
  This is implemented using a simple protocol that enables sending and
  receiveing network packets and some confuration using messages.

  In general the application works as follows:
  - Read incomming messages on UART
  - Read incomming packets on WiFi
  - Resend incomming packets from Wifi as UART messages
  - Resend incomming packet messages from UART using WiFi
  - Configure WiFi interface acoring to client message
  - Report link status on Wifi event or explicit request using UART message

  This aims to be used from MCUs. A Python script that exposes UART nic as
  Linux tap device is attached for testing.


  Copyright (C) 2022 Prusa Research a.s - www.prusa3d.com
  SPDX-License-Identifier: GPL-3.0-or-later
*/

#include <cstdint>
#include <cstring>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_aio.h"
#include "esp_crc.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_private/wifi.h"
extern "C" {
// including esp_supplicant/esp_wpa.h breaks c++ compilation,
// lets forward declare the needed function from that header
// to prevent compiler errors
esp_err_t esp_supplicant_init();
}

#include "uart0_driver.h"
#include "esp_protocol/messages.hpp"

// Externals with no header
extern "C" {
int ieee80211_output_pbuf(esp_aio_t *aio);
esp_err_t mac_init(void);
}

static constexpr size_t SCAN_MAX_STORED_SSIDS = 64;
static constexpr size_t BSSID_LEN = 6;

// Hack: because we don't see the beacon on some networks (and it's quite
// common), but don't want to be "flapping", we set the timeout for beacon
// inactivity to a ridiculously long time and handle the disconnect ourselves.
//
// It's not longer for the only reason the uint16_t doesn't hold as big numbers.
static constexpr uint16_t INACTIVE_BEACON_SECONDS = 400;
// This is the effective timeout. If we don't receive any packet for this long,
// we consider the signal lost.
//
// TODO: Shall we generate something to provoke getting some packets? Like ARP
// pings to the AP?
static constexpr uint32_t INACTIVE_PACKET_SECONDS = 5;

// Note: `uart0_tx_queue` is a FreeRTOS queue of `uart0_tx_queue_item` elements.
//       Send items to this queue in order to transmit them via UART0
//       from ESP to printer. The queue is drained by realtime priority
//       task `uart0_tx_task`.
struct uart0_tx_queue_item {
    esp::Header header;
    uint8_t *data;
    void *rx_buffer; // Note: This is some internal ESP buffer which we are not
                     //       sure if we can free before data is transmitted.
};
static QueueHandle_t uart0_tx_queue = NULL;

// Note: `uart0_rx_queue` is a FreeRTOS queue of `uart0_rx_queue_item` elements.
//       Items are send to this queue from realtime priority task `uart0_rx_task`
//       whenever they are received via UART0 by ESP from the printer. The queue
//       is drained by lower priority task `main_task`.
struct uart0_rx_queue_item {
    void (*callback)(uint8_t *, const esp::Header &);
    esp::Header header;
    uint8_t *data;
};
static QueueHandle_t uart0_rx_queue = 0;

static void uart0_rx_skip_bytes(size_t size) {
    uint8_t c;
    for (uint32_t i = 0; i < size; ++i) {
        uart0_rx_bytes(&c, 1);
    }
}

static const uint8_t uart_nic_protocol = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;

static const char *TAG = "uart_nic";

static int s_retry_num = 0;

// Note: We are using single global buffer here. It has three main parts:
//        * intron is read-only most of the time. The only write access is synchronized by means of critical section.
//        * header is used only by `uart0_tx_task`
//        * checksum is also used only by uart0_tx_task
static esp::MessagePrelude tx_message = {
    .intron = esp::DEFAULT_INTRON,
    .header = {
        .type = esp::MessageType::DEVICE_INFO_V2,
        .variable_byte = 0,
        .size = 0,
    },
    .data_checksum = 0,
};

static esp::data::MacAddress mac;

static uint32_t IRAM_ATTR now_seconds() {
    return xTaskGetTickCount() / configTICK_RATE_HZ;
}

static std::atomic<uint_least32_t> last_inbound_seen { 0 };
static std::atomic<bool> associated { false };
static std::atomic<bool> wifi_running { false };
static std::atomic<bool> connecting { false };

static bool beacon_quirk;
static uint8_t probe_max_reties = 3;
static std::atomic<bool> probe_in_progress { false };
static uint8_t probe_retry_count;
static uint8_t latest_ssid[esp::SSID_LEN];
static uint8_t latest_bssid[BSSID_LEN];

typedef enum {
    SCAN_TYPE_UNKNOWN,
    SCAN_TYPE_PROBE,
    SCAN_TYPE_INCREMENTAL,
} ScanType;

static constexpr esp::data::APInfo EMPTY_RESULT = {};

typedef void (*wifi_scan_callback)(wifi_ap_record_t *, int);

static struct {
    ScanType scan_type = SCAN_TYPE_UNKNOWN;
    wifi_scan_callback callback = nullptr;
    uint32_t incremental_scan_time = 0;
    esp::data::APInfo stored_ssids[SCAN_MAX_STORED_SSIDS] = {};
    uint8_t stored_ssids_count = 0;
    std::atomic<bool> in_progress { false };
    std::atomic<bool> should_reconnect { false };
} scan {};

static void IRAM_ATTR send_link_status(uint8_t up) {
    struct uart0_tx_queue_item queue_item;
    queue_item.header.type = esp::MessageType::PACKET_V2;
    queue_item.header.up = up;
    queue_item.header.size = htons(0);
    queue_item.data = NULL;
    queue_item.rx_buffer = NULL;
    if (xQueueSendToBack(uart0_tx_queue, &queue_item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "xQueueSendToBack failed (uart0tx)");
    }
}

static void IRAM_ATTR do_wifi_scan(void *arg) {
    wifi_scan_config_t config {};

    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    switch (scan.scan_type) {
    case SCAN_TYPE_PROBE:
        config.show_hidden = true;
        config.scan_time.active.min = 120;
        config.scan_time.active.max = 300;
        break;
    case SCAN_TYPE_INCREMENTAL:
        // The given timeouts are in ms, but per channel
        // On 2.4GHz wifi it would be 12-14 channels
        if (scan.incremental_scan_time == 0) {
            scan.incremental_scan_time = 42; // 42*12 = 504ms of total scan time
        } else {
            scan.incremental_scan_time *= 2;
            // Cap scan segment at ~30s (12*2500 = 30 000ms).
            if (scan.incremental_scan_time > 2500) {
                scan.incremental_scan_time = 2500;
            }
        }
        config.scan_time.active.max = config.scan_time.active.min = scan.incremental_scan_time;
        break;
    case SCAN_TYPE_UNKNOWN:
    default:
        break;
    }

    ESP_ERROR_CHECK(esp_wifi_scan_start(&config, false));

    vTaskDelete(NULL);
}

static esp_err_t IRAM_ATTR start_wifi_scan(wifi_scan_callback callback, ScanType scan_type) {
    if (scan_type == SCAN_TYPE_UNKNOWN) {
        return ESP_FAIL;
    }

    if (scan.in_progress) {
        return ESP_FAIL;
    }

    scan.callback = callback;
    scan.scan_type = scan_type;

    if (!wifi_running) {
        esp_err_t err = esp_wifi_start();
        if (err == ESP_OK) {
            wifi_running = true;
        } else {
            ESP_LOGE(TAG, "Unable start the wifi for scan");
            return err;
        }
    }

    scan.in_progress = true;

    if (associated && scan_type != SCAN_TYPE_PROBE) {
        // The wifi scan behaves differently when the esp is connected to the AP.
        // Intentionally disconnect when starting a scan.
        // The connection will be automatically reestablished after the scan.
        //
        // But we don't want to disconnect the wifi while probing.
        // That can cause "wifi speed decrease" since the connection procedure can take a long time
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to disconnect from current wifi AP: %s", esp_err_to_name(err));
        }
        associated = false;
    }

    if (connecting) {
        esp_err_t err = esp_wifi_disconnect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to disconnect from current wifi AP: %s", esp_err_to_name(err));
        }
        err = esp_wifi_deauth_sta(0); // 0 -> all
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to deauthorize from wifi: %s", esp_err_to_name(err));
        }
    }

    xTaskCreate(&do_wifi_scan, "wifi_scan", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    return ESP_OK;
}

static esp_err_t IRAM_ATTR force_stop_wifi_scan() {
    scan.in_progress = false;
    scan.scan_type = SCAN_TYPE_UNKNOWN;
    scan.incremental_scan_time = 0;
    return esp_wifi_scan_stop();
}

static void IRAM_ATTR cache_current_ap_info() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        memcpy(latest_bssid, ap_info.bssid, BSSID_LEN);
        memcpy(latest_ssid, ap_info.ssid, esp::SSID_LEN);
    }
}

static void IRAM_ATTR probe_run();

static void IRAM_ATTR probe_handler(wifi_ap_record_t *aps, int ap_count) {
    bool found = false;

    if (associated) {
        cache_current_ap_info();
    }

    // Try to match BSSID first and if that fails go on and try SSID match . The BSSD check
    // should be sufficient, but there are APs that advertise mismatching BSSID in their
    // beacons and/or probe rensonse. That's the real culprit of the beacon timeout
    // disconnects and the primary motivation of this whole excercise.
    for (int i = 0; i < ap_count; ++i) {
        if (latest_bssid != NULL && 0 == memcmp(latest_bssid, aps[i].bssid, BSSID_LEN)) {
            found = true;
            beacon_quirk = false;
            break;
        }
    }
    if (beacon_quirk && !found) {
        for (int i = 0; i < ap_count; ++i) {
            if (latest_ssid != NULL && latest_ssid[0] && aps[i].ssid[0]) {
                if (0 == strncmp((char *)(latest_ssid), (char *)(aps[i].ssid), esp::SSID_LEN)) {
                    found = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        if (probe_retry_count++ < probe_max_reties) {
            probe_run();
        } else {
            send_link_status(0);
            probe_in_progress = false;
        }
    } else {
        probe_in_progress = false;
        last_inbound_seen = now_seconds();
    }
}

static void IRAM_ATTR probe_run() {
    start_wifi_scan(&probe_handler, SCAN_TYPE_PROBE);
}

static void IRAM_ATTR wifi_re_connect_task(void *args) {
    if (!scan.in_progress) {
        ESP_LOGI(TAG, "Connecting to AP");
        esp_wifi_connect();
    } else {
        ESP_LOGW(TAG, "Unable to connect, scan is in progress. Will try to reconnect after the scan");
        scan.should_reconnect = true;
    }

    vTaskDelete(NULL);
}

static void IRAM_ATTR start_wifi_connect_task() {
    connecting = true;
    xTaskCreate(wifi_re_connect_task, "wifi_connect_task", 2048, NULL, tskIDLE_PRIORITY, NULL);
}

static void IRAM_ATTR handle_disconnect_and_try_reconnect() {
    associated = false;
    send_link_status(0);
    if (s_retry_num < CONFIG_ESP_MAXIMUM_RETRY) {
        start_wifi_connect_task();
        s_retry_num++;
        ESP_LOGI(TAG, "retry to connect to the AP");
        ESP_LOGI(TAG, "connect to the AP fail, now lowering RF power to reduce interference");
        esp_wifi_set_max_tx_power(48); // 12dB (down from 20dB) to reduce antenna reflections, needed for some modules (see ESP8266_RTOS_SDK#1200)
    } else {
        connecting = false;
    }
}

static void IRAM_ATTR event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        uint8_t current_protocol;
        ESP_ERROR_CHECK(esp_wifi_get_protocol(WIFI_IF_STA, &current_protocol));
        if (current_protocol != uart_nic_protocol) {
            ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA, uart_nic_protocol));
        }
        start_wifi_connect_task();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (scan.in_progress) {
            // We have intentionally disconnected from the wifi to make sure that the scan is not interupted by anything.
            // Lets handle the disconnection at the end of the scan.
            scan.should_reconnect = true;
            connecting = false;
        } else {
            handle_disconnect_and_try_reconnect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        connecting = false;
        last_inbound_seen = now_seconds();
        associated = true;
        beacon_quirk = true;
        send_link_status(1);
        s_retry_num = 0;
        ESP_ERROR_CHECK(esp_wifi_set_inactive_time(WIFI_IF_STA, INACTIVE_BEACON_SECONDS));
        cache_current_ap_info();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        scan.in_progress = false;
        const wifi_event_sta_scan_done_t *scan_data = (const wifi_event_sta_scan_done_t *)event_data;
        uint16_t ap_count = scan_data->number;

        if (!scan_data->status && ap_count) {
            wifi_ap_record_t *aps = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, aps));
            if (scan.callback != NULL) {
                scan.callback(aps, ap_count);
            }
            free(aps);
        } else if (scan.callback != NULL) {
            // if the scan failed, still call callback with no data to allow logic to redo the scan
            scan.callback(NULL, 0);
        }

        if (!scan.in_progress) {
            // If callback didn't start a new scan check the scan type, change it and rerun scan if possible
            switch (scan.scan_type) {
            case SCAN_TYPE_INCREMENTAL:
                ESP_LOGI(TAG, "Restarting incremental scan");
                start_wifi_scan(scan.callback, SCAN_TYPE_INCREMENTAL);
                break;
            default:
                scan.scan_type = SCAN_TYPE_UNKNOWN;
                if (scan.should_reconnect) {
                    scan.should_reconnect = false;
                    handle_disconnect_and_try_reconnect();
                }
                break;
            }
        }
    }
}

static int get_link_status();

static esp_err_t IRAM_ATTR wifi_receive_cb(void *buffer, uint16_t len, void *eb) {

    // Seeing some traffic - we have signal :-)
    last_inbound_seen = now_seconds();

    // MAC filter
    if ((((const char *)buffer)[5] & 0x01) == 0) {
        for (uint i = 0; i < 6; ++i) {
            if (((const char *)buffer)[i] != mac[i]) {
                ESP_LOGI(TAG, "Incoming packet filtered out.");
                free(buffer);
                esp_wifi_internal_free_rx_buffer(eb);
                return ESP_FAIL;
            }
        }
    }

    uart0_tx_queue_item queue_item;
    queue_item.header.type = esp::MessageType::PACKET_V2;
    queue_item.header.up = get_link_status();
    queue_item.header.size = htons(len);
    queue_item.data = (uint8_t *)buffer;
    queue_item.rx_buffer = eb;
    if (xQueueSendToBack(uart0_tx_queue, &queue_item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "xQueueSendToBack failed (uart0tx)");
        free(buffer);
        esp_wifi_internal_free_rx_buffer(eb);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void IRAM_ATTR wifi_init_sta(void) {
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(mac_init());
    esp_wifi_set_rx_pbuf_mem_type(WIFI_RX_PBUF_DRAM);
    ESP_ERROR_CHECK(esp_wifi_init_internal(&cfg));
    ESP_ERROR_CHECK(esp_supplicant_init());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, wifi_receive_cb));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
}

static void IRAM_ATTR send_device_info() {
    esp_wifi_get_mac(WIFI_IF_STA, mac.data()); // ignore error

    struct uart0_tx_queue_item queue_item;
    queue_item.header.type = esp::MessageType::DEVICE_INFO_V2;
    queue_item.header.version = esp::REQUIRED_PROTOCOL_VERSION;
    queue_item.header.size = htons(6);
    queue_item.data = mac.data();
    queue_item.rx_buffer = NULL;
    if (xQueueSendToBack(uart0_tx_queue, &queue_item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "xQueueSendToBack failed (uart0tx)");
    }
}

static void IRAM_ATTR wait_for_intron() {
    // Hope for the best...
    uint8_t intron[8];
    uart0_rx_bytes(intron, 8);
    while (memcmp(intron, tx_message.intron.data(), 8) != 0) {
        // ...but be prepared for the worst.
        for (int i = 0; i < 7; ++i) {
            intron[i] = intron[i + 1];
        }
        uart0_rx_bytes(&intron[7], 1);
    }
}

static int IRAM_ATTR get_link_status() {
    static wifi_ap_record_t ap_info;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap_info);
    // ap_info is not important, just not receiven ESP_ERR_WIFI_NOT_CONNECT means we are associated
    const bool online = ret == ESP_OK;
    associated = online;
    return online;
}

static void IRAM_ATTR handle_rx_msg_packet_v2(uint8_t *data, const esp::Header &header) {
    if (header.size == 0) {
        send_link_status(get_link_status());
    } else {
        esp_wifi_internal_tx(WIFI_IF_STA, data, header.size);
        free(data);
    }
}

static void IRAM_ATTR handle_rx_msg_clientconfig_v2(uint8_t *data, const esp::Header &header) {
    wifi_config_t wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config_t));

    {
        taskENTER_CRITICAL();
        memcpy(tx_message.intron.data(), data, tx_message.intron.size());
        taskEXIT_CRITICAL();
        data += tx_message.intron.size();
    }
    uint8_t ssid_length = 0;
    {
        memcpy(&ssid_length, data, sizeof(ssid_length));
        data += sizeof(ssid_length);
        size_t memcpy_size = ssid_length < sizeof(wifi_config.sta.ssid)
            ? ssid_length
            : sizeof(wifi_config.sta.ssid);
        memcpy(wifi_config.sta.ssid, data, memcpy_size);
        data += ssid_length;
    }
    uint8_t password_length = 0;
    {
        memcpy(&password_length, data, sizeof(password_length));
        data += sizeof(password_length);
        size_t memcpy_size = password_length < sizeof(wifi_config.sta.password)
            ? password_length
            : sizeof(wifi_config.sta.password);
        memcpy(wifi_config.sta.password, data, memcpy_size);
        data += password_length;
    }

    /* Setting a password implies station will connect to all security modes including WEP/WPA.
     * However these modes are deprecated and not advisable to be used. Incase your Access point
     * doesn't support WPA2, these mode can be enabled by commenting below line */
    if (password_length) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wifi_config.sta.pmf_cfg.capable = 1;

    // If scan is in progress we need to stop it manually here to prevent reconnect to previous AP.
    if (scan.in_progress) {
        scan.should_reconnect = false;
        force_stop_wifi_scan();
    }

    wifi_running = false;
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    wifi_running = true;
}

static void IRAM_ATTR handle_rx_msg_unknown(uint8_t *data, const esp::Header &header) {
    if (data) {
        free(data);
    }
}

static void IRAM_ATTR check_online_status() {
    if (!associated || probe_in_progress) {
        // Nothing to check, we are not online and we know it.
        return;
    }
    const uint32_t last = last_inbound_seen; // Atomic load
    const uint32_t now = now_seconds();
    // The time may overflow from time to time and due to the conversion to
    // seconds, we don't know when exactly. But if it overflows, the now would
    // get smaller than the last time (assuming we check often enough). In that
    // case we ignore the part up to the overflow and take just the part in the
    // „new round“.
    const uint32_t elapsed = now >= last ? now - last : now;

    if (elapsed > INACTIVE_PACKET_SECONDS) {
        probe_in_progress = true;
        probe_retry_count = 0;
        probe_run();
    }
}

static void IRAM_ATTR clear_stored_ssids() {
    scan.stored_ssids_count = 0;
}

static void IRAM_ATTR store_scanned_ssids(wifi_ap_record_t *aps, int ap_count) {
    for (uint8_t i = 0; i < ap_count; ++i) {
        bool found = false;
        if (scan.stored_ssids_count == SCAN_MAX_STORED_SSIDS) {
            ESP_LOGW(TAG, "Scan result storage is full");
            break;
        }
        for (uint8_t j = 0; j < scan.stored_ssids_count; ++j) {
            ESP_LOGD(TAG, "Comparing >%.32s< and %s", (char *)scan.stored_ssids[j].ssid.data(), (char *)aps[i].ssid);
            if (memcmp(scan.stored_ssids[j].ssid.data(), aps[i].ssid, esp::SSID_LEN) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            memcpy(scan.stored_ssids[scan.stored_ssids_count].ssid.data(), aps[i].ssid, esp::SSID_LEN);
            scan.stored_ssids[scan.stored_ssids_count].requires_password = aps[i].authmode != WIFI_AUTH_OPEN || aps[i].pairwise_cipher != WIFI_CIPHER_TYPE_NONE;
            scan.stored_ssids_count++;
            ESP_LOGI(TAG, "Found SSID: %s", aps[i].ssid);
        }
    }

    struct uart0_tx_queue_item queue_item {};
    queue_item.header.type = esp::MessageType::SCAN_AP_CNT;
    queue_item.header.ap_count = scan.stored_ssids_count;
    if (xQueueSendToBack(uart0_tx_queue, &queue_item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "xQueueSendToBack failed (uart0tx)");
    }
    ESP_LOGI(TAG, "Scan done. Found: %d", scan.stored_ssids_count);
}

static void IRAM_ATTR send_scanned_ssid(uint8_t index, const esp::data::APInfo &ap_info) {
    struct uart0_tx_queue_item queue_item {};
    queue_item.header.type = esp::MessageType::SCAN_AP_GET;
    queue_item.header.ap_index = index;
    queue_item.header.size = htons(sizeof(ap_info));
    queue_item.data = (uint8_t *)(&ap_info);

    if (xQueueSendToBack(uart0_tx_queue, &queue_item, 0) != pdTRUE) {
        ESP_LOGE(TAG, "xQueueSendToBack failed (uart0tx)");
    }
}

static void IRAM_ATTR handle_rx_msg_scan_start(uint8_t *data, const esp::Header &header) {
    clear_stored_ssids();
    ESP_LOGI(TAG, "Starting scan...");

    start_wifi_scan(&store_scanned_ssids, SCAN_TYPE_INCREMENTAL);
}

static void IRAM_ATTR handle_rx_msg_scan_stop(uint8_t *data, const esp::Header &header) {
    // Response is send automatically after scan is stopped
    // Intentionally don't fail if the scan is no longer running
    // (aka we connected to AP during scan)
    force_stop_wifi_scan();
}

static void IRAM_ATTR handle_rx_msg_scan_get(uint8_t *data, const esp::Header &header) {
    if (header.ap_index < scan.stored_ssids_count) {
        send_scanned_ssid(header.ap_index, scan.stored_ssids[header.ap_index]);
    } else {
        send_scanned_ssid(UINT8_MAX, EMPTY_RESULT);
    }
}

static void IRAM_ATTR read_message() {
    wait_for_intron();
    uint32_t crc = 0;
    crc = crc32_le(crc, tx_message.intron.data(), tx_message.intron.size());
    struct uart0_rx_queue_item queue_item {};
    uart0_rx_bytes(reinterpret_cast<uint8_t *>(&queue_item.header), sizeof(queue_item.header));
    crc = crc32_le(crc, reinterpret_cast<uint8_t *>(&queue_item.header), sizeof(queue_item.header));
    uint32_t checksum = 0;
    uart0_rx_bytes(reinterpret_cast<uint8_t *>(&checksum), sizeof(checksum));
    checksum = ntohl(checksum);

    switch (queue_item.header.type) {
    case esp::MessageType::PACKET_V2:
        queue_item.callback = handle_rx_msg_packet_v2;
        break;
    case esp::MessageType::CLIENTCONFIG_V2:
        queue_item.callback = handle_rx_msg_clientconfig_v2;
        break;
    case esp::MessageType::SCAN_START:
        queue_item.callback = handle_rx_msg_scan_start;
        break;
    case esp::MessageType::SCAN_STOP:
        queue_item.callback = handle_rx_msg_scan_stop;
        break;
    case esp::MessageType::SCAN_AP_CNT:
        ESP_LOGE(TAG, "esp::MessageType::SCAN_AP_CNT is only transmitted, never recieved");
        queue_item.callback = handle_rx_msg_unknown;
        break;
    case esp::MessageType::SCAN_AP_GET:
        queue_item.callback = handle_rx_msg_scan_get;
        break;
    case esp::MessageType::DEVICE_INFO_V2:
        ESP_LOGE(TAG, "esp::MessageType::DEVICE_INFO_V2 is only transmitted, never recieved");
        queue_item.callback = handle_rx_msg_unknown;
        break;
    default:
        queue_item.callback = handle_rx_msg_unknown;
        break;
    }

    queue_item.header.size = ntohs(queue_item.header.size);
    if (queue_item.header.size != 0 && queue_item.header.size <= 2000) {
        queue_item.data = (uint8_t *)malloc(queue_item.header.size);
        if (queue_item.data) {
            uart0_rx_bytes(queue_item.data, queue_item.header.size);
        } else {
            uart0_rx_skip_bytes(queue_item.header.size);
        }
    } else {
        queue_item.data = NULL;
        uart0_rx_skip_bytes(queue_item.header.size);
    }

    if (queue_item.header.size > 0 && queue_item.data != nullptr) {
        crc = crc32_le(crc, queue_item.data, queue_item.header.size);
    }

    if (crc == checksum) {
        if (xQueueSendToBack(uart0_rx_queue, &queue_item, 0) != pdTRUE) {
            ESP_LOGE(TAG, "xQueueSendToBack failed (uart0rx)");
            if (queue_item.data) {
                free(queue_item.data);
            }
        }
    } else {
        ESP_LOGE(TAG, "Checksum mismatch: MT %d, calc: %x ref: %x", static_cast<uint8_t>(queue_item.header.type), crc, checksum);
    }
}

static void IRAM_ATTR uart0_rx_task(void *arg) {
    // wait for uart0_driver_install() to finish
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    for (;;) {
        read_message();
    }
}

static void IRAM_ATTR main_task(void *arg) {
    // Wait because printer sends reset for whatever reason.
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    // Send initial device info to let master know ESP is ready
    send_device_info();

    for (;;) {
        struct uart0_rx_queue_item queue_item;
        if (xQueueReceive(uart0_rx_queue, &queue_item, portMAX_DELAY) == pdTRUE) {
            (*queue_item.callback)(queue_item.data, queue_item.header);
            check_online_status();
        }
    }
}

static void IRAM_ATTR uart0_tx_task(void *arg) {
    // wait for uart0_driver_install() to finish
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // consume messages from uart0_tx_queue, forever
    for (;;) {
        struct uart0_tx_queue_item queue_item;
        if (xQueueReceive(uart0_tx_queue, &queue_item, portMAX_DELAY) == pdTRUE) {

            // send fix-sized part of the message (intron + header + checksum)
            tx_message.header = queue_item.header;
            uint32_t crc = 0;
            crc = crc32_le(crc, tx_message.intron.data(), tx_message.intron.size());
            crc = crc32_le(crc, reinterpret_cast<uint8_t *>(&tx_message.header), sizeof(tx_message.header));
            if (queue_item.header.size != 0 && queue_item.data != nullptr) {
                crc = crc32_le(crc, queue_item.data, ntohs(queue_item.header.size));
            }
            tx_message.data_checksum = htonl(crc);
            uart0_tx_bytes(reinterpret_cast<uint8_t *>(&tx_message), sizeof(tx_message));

            // send variable-sized part of the message (payload depending on message type)
            switch (queue_item.header.type) {
            case esp::MessageType::PACKET_V2: {
                // size may be empty when we are using esp::MessageType::PACKET_V2 to only send link status
                uint16_t size = ntohs(queue_item.header.size);
                if (size) {
                    uart0_tx_bytes(queue_item.data, size);
                    free(queue_item.data);
                }
                if (queue_item.rx_buffer) {
                    free(queue_item.rx_buffer);
                }
            } break;
            case esp::MessageType::DEVICE_INFO_V2:
                uart0_tx_bytes(mac.data(), mac.size());
                break;
            case esp::MessageType::CLIENTCONFIG_V2:
                ESP_LOGE(TAG, "esp::MessageType::CLIENTCONFIG_V2 is only received, never transmitted");
                break;
            case esp::MessageType::SCAN_START:
                ESP_LOGE(TAG, "esp::MessageType::SCAN_START is only received, never transmitted");
                break;
            case esp::MessageType::SCAN_AP_CNT:
                // no data to send
                break;
            case esp::MessageType::SCAN_STOP:
                ESP_LOGE(TAG, "esp::MessageType::SCAN_STOP is only received, never transmitted");
                break;
            case esp::MessageType::SCAN_AP_GET:
                uart0_tx_bytes(queue_item.data, ntohs(queue_item.header.size));
                break;
            }
        }
    }
}

extern "C" void IRAM_ATTR app_main() {
    ESP_LOGI(TAG, "UART NIC");

    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set(TAG, ESP_LOG_WARNING);

    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_LOGI(TAG, "Wifi init");
    esp_wifi_restore();
    wifi_init_sta();
    esp_wifi_set_ps(WIFI_PS_NONE);

    uart0_rx_queue = xQueueCreate(20, sizeof(struct uart0_rx_queue_item));
    if (uart0_rx_queue == 0) {
        ESP_LOGE(TAG, "xQueueCreate failed (uart0rx)");
        abort();
    }
    uart0_tx_queue = xQueueCreate(20, sizeof(struct uart0_tx_queue_item));
    if (uart0_tx_queue == 0) {
        ESP_LOGE(TAG, "xQueueCreate failed (uart0tx)");
        abort();
    }

    // Initialize scan ap storage
    scan.stored_ssids_count = SCAN_MAX_STORED_SSIDS;
    clear_stored_ssids();
    memset(latest_bssid, 0, BSSID_LEN);
    memset(latest_ssid, 0, esp::SSID_LEN);

    TaskHandle_t uart0_rx_task_handle;
    TaskHandle_t uart0_tx_task_handle;
    if (xTaskCreate(uart0_rx_task, "uart0rx", 1024, NULL, 14, &uart0_rx_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (uart0rx)");
        abort();
    }
    if (xTaskCreate(uart0_tx_task, "uart0tx", 1024, NULL, 14, &uart0_tx_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (uart0tx)");
        abort();
    }
    if (uart0_driver_install(uart0_rx_task_handle, uart0_tx_task_handle) != ESP_OK) {
        ESP_LOGE(TAG, "uart0_driver_install failed");
        abort();
    }
    if (xTaskCreate(main_task, "main", 2048, NULL, 12, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed (main)");
        abort();
    }

    // Note: These are not the only tasks running. There are also ESP system tasks present, see
    //       https://docs.espressif.com/projects/esp8266-rtos-sdk/en/latest/api-guides/system-tasks.html
}
