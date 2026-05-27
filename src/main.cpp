#include <stdio.h>
#include <string.h>

#ifndef CAMERA_MODEL_ESP32S3_EYE
#define CAMERA_MODEL_ESP32S3_EYE
#endif

#include "app_state.h"
#include "camera_pins.h"
#include "esp_camera.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ssd1306.h"

static const char *TAG = "camera_main";
static const char *NVS_NAMESPACE = "app";
static const char *NVS_SETTINGS_KEY = "settings";
static const int WIFI_CONNECTED_BIT = BIT0;
static const int WIFI_FAIL_BIT = BIT1;
static const int WIFI_MAXIMUM_RETRY = 10;
static const int WIFI_CONNECT_TIMEOUT_MS = 12000;
static const int CAMERA_DEFAULT_XCLK_MHZ = 20;

static EventGroupHandle_t wifi_event_group;
static int retry_count;
static esp_ip4_addr_t local_ip;
static bool sta_connected;
static bool ap_started;
static char ap_ssid[33];
static esp_netif_t *sta_netif;
static esp_netif_t *ap_netif;
static app_settings_t settings;
static app_stream_metrics_t stream_metrics;
static portMUX_TYPE stream_metrics_mux = portMUX_INITIALIZER_UNLOCKED;

void startCameraServer();

static void default_settings(app_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->framesize = FRAMESIZE_SVGA;
    out->quality = 14;
    out->brightness = 1;
    out->contrast = 0;
    out->saturation = 0;
    out->sharpness = 0;
    out->awb = 1;
    out->awb_gain = 1;
    out->wb_mode = 0;
    out->aec = 1;
    out->aec2 = 1;
    out->ae_level = 0;
    out->aec_value = 600;
    out->agc = 1;
    out->agc_gain = 5;
    out->gainceiling = GAINCEILING_16X;
    out->hmirror = 0;
    out->vflip = 1;
    out->special_effect = 0;
    out->xclk_mhz = CAMERA_DEFAULT_XCLK_MHZ;
    out->fb_count = 2;
    out->metrics_interval_ms = 1000;
    out->auto_stream = true;
    strncpy(out->language, "zh", sizeof(out->language));
}

const app_settings_t *app_get_settings(void)
{
    return &settings;
}

esp_err_t app_load_settings(void)
{
    default_settings(&settings);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t size = sizeof(settings);
    err = nvs_get_blob(handle, NVS_SETTINGS_KEY, &settings, &size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        default_settings(&settings);
        return ESP_OK;
    }
    if (err != ESP_OK || size != sizeof(settings)) {
        default_settings(&settings);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }
    settings.ssid[sizeof(settings.ssid) - 1] = 0;
    settings.password[sizeof(settings.password) - 1] = 0;
    settings.language[sizeof(settings.language) - 1] = 0;
    settings.has_wifi = settings.ssid[0] != 0;
    if (settings.xclk_mhz == 10) {
        settings.xclk_mhz = 20; // Upgrade old default 10MHz to 20MHz
    }
    return ESP_OK;
}

esp_err_t app_save_settings(const app_settings_t *new_settings)
{
    app_settings_t copy = *new_settings;
    copy.ssid[sizeof(copy.ssid) - 1] = 0;
    copy.password[sizeof(copy.password) - 1] = 0;
    copy.language[sizeof(copy.language) - 1] = 0;
    copy.has_wifi = copy.ssid[0] != 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, NVS_SETTINGS_KEY, &copy, sizeof(copy));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK) {
        settings = copy;
    }
    return err;
}

esp_err_t app_save_wifi_credentials(const char *ssid, const char *password)
{
    app_settings_t copy = settings;
    strlcpy(copy.ssid, ssid ? ssid : "", sizeof(copy.ssid));
    strlcpy(copy.password, password ? password : "", sizeof(copy.password));
    copy.has_wifi = copy.ssid[0] != 0;
    return app_save_settings(&copy);
}

static esp_err_t start_config_ap(void);
static esp_err_t configure_sta(bool keep_ap);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (settings.has_wifi) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        sta_connected = false;
        if (settings.has_wifi && retry_count < WIFI_MAXIMUM_RETRY) {
            retry_count++;
            ESP_LOGW(TAG, "WiFi disconnected, retrying (%d/%d)", retry_count, WIFI_MAXIMUM_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            if (!ap_started) {
                ESP_LOGW(TAG, "STA connection failed after maximum retries, starting fallback AP");
                start_config_ap();
                configure_sta(true);
                esp_wifi_connect();
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ap_started = true;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        ap_started = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        local_ip = event->ip_info.ip;
        retry_count = 0;
        sta_connected = true;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void make_ap_ssid(void)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32-CAM-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static esp_err_t start_config_ap(void)
{
    make_ap_ssid();
    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(settings.has_wifi ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_LOGW(TAG, "Configuration AP enabled: %s", ap_ssid);
    return ESP_OK;
}

static esp_err_t configure_sta(bool keep_ap)
{
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, settings.ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, settings.password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(keep_ap ? WIFI_MODE_APSTA : WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    return ESP_OK;
}

static esp_err_t wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();
    (void)sta_netif;
    (void)ap_netif;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (settings.has_wifi) {
        ESP_ERROR_CHECK(configure_sta(false));
    } else {
        ESP_ERROR_CHECK(start_config_ap());
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t app_wifi_reconfigure(const char *ssid, const char *password)
{
    ESP_RETURN_ON_ERROR(app_save_wifi_credentials(ssid, password), TAG, "save WiFi credentials failed");
    retry_count = 0;
    sta_connected = false;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_RETURN_ON_ERROR(configure_sta(ap_started), TAG, "configure STA failed");
    return esp_wifi_connect();
}

bool app_wifi_sta_connected(void)
{
    return sta_connected;
}

bool app_wifi_ap_started(void)
{
    return ap_started;
}

const char *app_wifi_ap_ssid(void)
{
    return ap_ssid;
}

esp_ip4_addr_t app_wifi_sta_ip(void)
{
    return local_ip;
}

int app_wifi_rssi(void)
{
    wifi_ap_record_t ap = {};
    if (!sta_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

static esp_err_t init_camera(void)
{
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = settings.xclk_mhz * 1000000;
    config.frame_size = (framesize_t)settings.framesize;
    config.pixel_format = PIXFORMAT_JPEG;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = settings.quality;
    config.fb_count = settings.fb_count > 0 ? settings.fb_count : 1;

    bool psram_available = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    if (!psram_available) {
        ESP_LOGW(TAG, "PSRAM not available, limiting frame size to SVGA");
        config.frame_size = FRAMESIZE_SVGA;
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return err;
    }

    return app_apply_camera_settings();
}

esp_err_t app_apply_camera_settings(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        return ESP_ERR_NOT_FOUND;
    }
    s->set_framesize(s, (framesize_t)settings.framesize);
    s->set_quality(s, settings.quality);
    s->set_brightness(s, settings.brightness);
    s->set_contrast(s, settings.contrast);
    s->set_saturation(s, settings.saturation);
    s->set_sharpness(s, settings.sharpness);
    s->set_whitebal(s, settings.awb);
    s->set_awb_gain(s, settings.awb_gain);
    s->set_wb_mode(s, settings.wb_mode);
    s->set_exposure_ctrl(s, settings.aec);
    s->set_aec2(s, settings.aec2);
    s->set_ae_level(s, settings.ae_level);
    s->set_aec_value(s, settings.aec_value);
    s->set_gain_ctrl(s, settings.agc);
    s->set_agc_gain(s, settings.agc_gain);
    s->set_gainceiling(s, (gainceiling_t)settings.gainceiling);
    s->set_hmirror(s, settings.hmirror);
    s->set_vflip(s, settings.vflip);
    s->set_special_effect(s, settings.special_effect);
    s->set_xclk(s, LEDC_TIMER_0, settings.xclk_mhz);
    return ESP_OK;
}

void app_stream_client_delta(int delta)
{
    portENTER_CRITICAL(&stream_metrics_mux);
    if (delta < 0 && stream_metrics.active_clients == 0) {
        stream_metrics.active_clients = 0;
    } else {
        stream_metrics.active_clients += delta;
    }
    portEXIT_CRITICAL(&stream_metrics_mux);
}

void app_stream_note_frame(size_t frame_size, int64_t frame_time_ms)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&stream_metrics_mux);
    stream_metrics.frame_count++;
    stream_metrics.avg_frame_size = stream_metrics.avg_frame_size == 0
                                        ? frame_size
                                        : ((stream_metrics.avg_frame_size * 7) + frame_size) / 8;
    stream_metrics.avg_frame_ms = stream_metrics.avg_frame_ms == 0
                                      ? frame_time_ms
                                      : ((stream_metrics.avg_frame_ms * 7) + frame_time_ms) / 8;
    stream_metrics.fps_x10 = stream_metrics.avg_frame_ms > 0 ? (10000 / stream_metrics.avg_frame_ms) : 0;
    stream_metrics.bandwidth_kbps = stream_metrics.avg_frame_ms > 0 ? (uint32_t)((stream_metrics.avg_frame_size * 8) / stream_metrics.avg_frame_ms) : 0;
    stream_metrics.last_frame_us = now;
    portEXIT_CRITICAL(&stream_metrics_mux);
}

app_stream_metrics_t app_get_stream_metrics(void)
{
    app_stream_metrics_t snapshot;
    portENTER_CRITICAL(&stream_metrics_mux);
    snapshot = stream_metrics;
    portEXIT_CRITICAL(&stream_metrics_mux);
    return snapshot;
}

static void oled_task(void *pvParameters)
{
    while (1) {
        char buf[64];
        oled_clear();
        oled_draw_string(0, 0, "= ESP32-S3 CAM =");

        // Wifi status / IP
        if (sta_connected) {
            snprintf(buf, sizeof(buf), "IP:%d.%d.%d.%d", IP2STR(&local_ip));
            oled_draw_string(2, 0, buf);
        } else if (ap_started) {
            snprintf(buf, sizeof(buf), "AP: %s", ap_ssid);
            oled_draw_string(2, 0, buf);
        } else {
            oled_draw_string(2, 0, "WiFi: Connecting");
        }

        // Streaming Metrics
        app_stream_metrics_t metrics = app_get_stream_metrics();
        snprintf(buf, sizeof(buf), "FPS:%d.%d  Clients:%u", (int)(metrics.fps_x10 / 10), (int)(metrics.fps_x10 % 10), (unsigned int)metrics.active_clients);
        oled_draw_string(4, 0, buf);

        // Memory Status
        uint32_t free_heap = esp_get_free_heap_size();
        uint32_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snprintf(buf, sizeof(buf), "RAM:%uKB PS:%uMB", (unsigned int)(free_heap / 1024), (unsigned int)(free_psram / (1024 * 1024)));
        oled_draw_string(6, 0, buf);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(app_load_settings());

    if (oled_init() == ESP_OK) {
        oled_clear();
        oled_draw_string(0, 0, "= ESP32-S3 CAM =");
        oled_draw_string(3, 0, "Booting...");
        xTaskCreate(oled_task, "oled_task", 4096, NULL, 5, NULL);
    }

    ESP_ERROR_CHECK(init_camera());
    ESP_ERROR_CHECK(wifi_init());

    startCameraServer();
    if (sta_connected) {
        ESP_LOGI(TAG, "Camera ready: http://" IPSTR, IP2STR(&local_ip));
    } else {
        ESP_LOGI(TAG, "Camera ready on AP: %s", ap_ssid);
    }
}
