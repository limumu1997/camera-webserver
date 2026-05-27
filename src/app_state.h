#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

typedef struct {
    char ssid[33];
    char password[65];
    bool has_wifi;
    int framesize;
    int quality;
    int brightness;
    int contrast;
    int saturation;
    int sharpness;
    int awb;
    int awb_gain;
    int wb_mode;
    int aec;
    int aec2;
    int ae_level;
    int aec_value;
    int agc;
    int agc_gain;
    int gainceiling;
    int hmirror;
    int vflip;
    int special_effect;
    int xclk_mhz;
    int fb_count;
    int metrics_interval_ms;
    bool auto_stream;
    char language[8];
} app_settings_t;

typedef struct {
    uint32_t active_clients;
    uint32_t frame_count;
    uint32_t fps_x10;
    uint32_t avg_frame_ms;
    uint32_t avg_frame_size;
    uint32_t bandwidth_kbps;
    int64_t last_frame_us;
} app_stream_metrics_t;

const app_settings_t *app_get_settings(void);
esp_err_t app_load_settings(void);
esp_err_t app_save_settings(const app_settings_t *settings);
esp_err_t app_save_wifi_credentials(const char *ssid, const char *password);
esp_err_t app_apply_camera_settings(void);

bool app_wifi_sta_connected(void);
bool app_wifi_ap_started(void);
const char *app_wifi_ap_ssid(void);
esp_ip4_addr_t app_wifi_sta_ip(void);
int app_wifi_rssi(void);
int app_wifi_signal_percent(void);
esp_err_t app_wifi_reconfigure(const char *ssid, const char *password);

void app_stream_client_delta(int delta);
void app_stream_note_frame(size_t frame_size, int64_t frame_time_ms);
app_stream_metrics_t app_get_stream_metrics(void);
