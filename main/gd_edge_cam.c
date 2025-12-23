#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "mqtt_client.h"

/* ================= CONFIG ================= */

#define WIFI_SSID ""
#define WIFI_PASS ""  // Empty for open network, or your password for WPA2
#define PI_IP     ""

// Set to 1 for open network, 0 for WPA2
#define USE_OPEN_NETWORK 1

static const char *TAG = "gd";

/* ================= GLOBALS ================= */

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t mqtt_client = NULL;
static int retry_count = 0;

/* ================= WIFI ================= */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi start -> connecting");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {

        wifi_event_sta_disconnected_t *d =
            (wifi_event_sta_disconnected_t *)event_data;

        ESP_LOGW(TAG, "WiFi disconnected, reason=%d, retry=%d", 
                 d->reason, retry_count);

        // Log specific error reasons
        switch(d->reason) {
            case WIFI_REASON_AUTH_EXPIRE:
                ESP_LOGW(TAG, "Auth expired (timeout)");
                break;
            case WIFI_REASON_NO_AP_FOUND:
                ESP_LOGW(TAG, "AP not found - check SSID");
                break;
            case WIFI_REASON_AUTH_FAIL:
                ESP_LOGW(TAG, "Auth failed - check password/security");
                break;
            case WIFI_REASON_BEACON_TIMEOUT:
                ESP_LOGW(TAG, "Beacon timeout - signal issue");
                break;
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                ESP_LOGW(TAG, "Handshake timeout");
                break;
            default:
                ESP_LOGW(TAG, "Other reason: %d", d->reason);
        }

        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        
        // Add longer delay before retry for auth timeout
        if (d->reason == WIFI_REASON_AUTH_EXPIRE || 
            d->reason == WIFI_REASON_AUTH_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(3000)); // 3 second delay
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        retry_count++;
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT &&
             event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi GOT IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {0};
    
#if USE_OPEN_NETWORK
    // Open network configuration - NO PASSWORD
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = false;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_LOGI(TAG, "Configuring for OPEN network (no password)");
#else
    // WPA2 configuration
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ESP_LOGI(TAG, "Configuring for WPA2-PSK network");
#endif

    strncpy((char *)wifi_config.sta.ssid,
            WIFI_SSID,
            sizeof(wifi_config.sta.ssid));

    // Only set password if not empty (for WPA2)
    if (strlen(WIFI_PASS) > 0) {
        strncpy((char *)wifi_config.sta.password,
                WIFI_PASS,
                sizeof(wifi_config.sta.password));
    }

    // Additional settings to help with stability
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; // Try full scan
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.failure_retry_cnt = 10; // More retries
    wifi_config.sta.listen_interval = 3; // Help with beacon timing

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // No power save
    
    // IMPORTANT: Start WiFi BEFORE setting TX power
    ESP_ERROR_CHECK(esp_wifi_start());
    
    // Set max TX power for better signal (AFTER wifi_start)
    esp_err_t tx_err = esp_wifi_set_max_tx_power(78); // 78 = 19.5dBm
    if (tx_err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set TX power: %d", tx_err);
    }
    
    ESP_LOGI(TAG, "WiFi init complete, SSID: %s", WIFI_SSID);
}

/* ================= MQTT ================= */

static void mqtt_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    
    switch(event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_publish(
                mqtt_client,
                "gd/cam01/event",
                "{\"hello\":\"world\",\"stage\":\"step5\"}",
                0, 1, 0);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
            
        default:
            break;
    }
}

static void mqtt_start(void)
{
    char uri[64];
    snprintf(uri, sizeof(uri), "mqtt://%s:1883", PI_IP);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .network.timeout_ms = 5000,
        .network.reconnect_timeout_ms = 5000,
    };

    mqtt_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(
        mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    
    ESP_LOGI(TAG, "MQTT client started, broker: %s", uri);
}

/* ================= MAIN ================= */

void app_main(void)
{
    printf("\n=== APP_MAIN START ===\n");
    printf("ESP32-S3 WiFi MQTT Test\n");
    printf("Target SSID: %s\n", WIFI_SSID);
    printf("Target Broker: %s:1883\n\n", PI_IP);

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || 
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Start WiFi
    wifi_init();

    // Wait for connection with timeout
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(30000)); // 30 second timeout

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully!");
        mqtt_start();
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout!");
        ESP_LOGE(TAG, "Check: SSID correct? Router 2.4GHz? In range?");
    }

    // Main loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        // Periodic status
        if (xEventGroupGetBits(wifi_event_group) & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "Status: WiFi OK, MQTT %s", 
                     mqtt_client ? "running" : "stopped");
        } else {
            ESP_LOGW(TAG, "Status: WiFi disconnected");
        }
    }
}