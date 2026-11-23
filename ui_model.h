#pragma once
#include <stdbool.h>
#include <stdint.h>
#define AP_LIMIT 24

typedef struct {
    char ssid[129], bssid[18], auth[65];
    int rssi, channel;
    bool fix;
    uint32_t boot, sequence, slot, session;
    bool aggregate;
} ApView;
typedef struct {
    bool connected, ready, running, stopping, fix, wifi, idle, file_open;
    uint64_t observations, rows, no_gps, unique, filtered, stats[21], elapsed_ms;
    uint32_t truncated, discarded, satellites, queued, ap_capacity, queue_capacity, memory[15];
    uint32_t config[13], gps_age, baud, boot, session;
    bool memory_known, storage_ready, paused, scanning, link_wait;
    char firmware[16], gps[11][32], utc_start[20], utc_stop[20], network_detail[1024],
        session_summary[768];
    unsigned network_page;
    bool network_loading;
    unsigned band, interval, ap_count;
    char phase[12], hdop[16], message[48], path[160];
    ApView aps[AP_LIMIT];
} Model;
typedef enum {
    UiOffline,
    UiConnecting,
    UiReady,
    UiRecording,
    UiStopping,
    UiWorking,
    UiError,
    UiNoWifi,
    UiNoMemory
} UiState;
typedef struct {
    Model data;
    bool serial_active, failed, startup, busy;
    unsigned port;
} UiSnapshot;
UiState ui_state(const UiSnapshot* s);
const char* ui_state_title(UiState state);
const char* ui_primary_action(UiState state);
bool ui_settings_allowed(const UiSnapshot* s);
void ui_format_count(uint64_t value, char* output, unsigned capacity);

/* The GUI drawing model contains only dashboard fields, not the AP table/journal details. */
typedef struct {
    UiState state;
    unsigned band, port;
    uint64_t unique, rows;
    uint32_t satellites;
    bool fix;
    char hdop[16], message[48];
} DashboardSnapshot;
void ui_dashboard_snapshot(const UiSnapshot* source, DashboardSnapshot* output);
