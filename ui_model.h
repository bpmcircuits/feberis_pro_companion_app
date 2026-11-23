#pragma once
#include "scan_metrics.h"
#include <stdbool.h>
#include <stdint.h>
#define AP_LIMIT 24

typedef struct {
    char ssid[129], bssid[18], auth[65];
    int rssi, channel;
    bool fix, rx_unverified, reception_timing;
    uint32_t boot, sequence, slot, session;
    bool aggregate;
} ApView;
typedef struct {
    bool connected, ready, running, stopping, fix, wifi, idle, file_open;
    bool start_confirmation_pending;
    uint64_t observations, rows, no_gps, unique, filtered, stats[21], elapsed_ms;
    uint32_t truncated, discarded, satellites, queued, ap_capacity, queue_capacity, memory[15];
    uint64_t scan_metrics[FBR_SCAN_METRIC_FIELDS];
    bool scan_metrics_known, capture_degraded;
    uint32_t config[13], gps_age, baud, boot, session;
    bool memory_known, storage_ready, paused, scanning, link_wait;
    char firmware[16], gps[11][32], utc_start[20], utc_stop[20], network_detail[1024],
        session_summary[768];
    unsigned network_page;
    bool network_loading;
    unsigned band, cycle_gap_ms, ap_count;
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
} UiSnapshot;
UiState ui_state(const UiSnapshot* s);
const char* ui_state_title(UiState state);
const char* ui_primary_action(UiState state);
bool ui_settings_allowed(const UiSnapshot* s);
bool ui_start_confirmation_needed(const UiSnapshot* s);
void ui_format_count(uint64_t value, char* output, unsigned capacity);
const char* ui_optional_measurement(const char* value);

/* The GUI drawing model contains only dashboard fields, not the AP table/journal details. */
typedef struct {
    UiState state;
    unsigned band;
    uint64_t unique, rows;
    uint32_t satellites;
    bool fix, capture_degraded;
    char hdop[16], message[48];
} DashboardSnapshot;
void ui_dashboard_snapshot(const UiSnapshot* source, DashboardSnapshot* output);

#define UI_CYCLE_GAP_COUNT 4
bool ui_continuous_supported(const char* firmware);
unsigned ui_effective_cycle_gap(const Model* model);
unsigned ui_cycle_gap_preset(const char* firmware, unsigned index);
const char* ui_observation_timing(const char* firmware);
