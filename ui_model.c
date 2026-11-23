#include "ui_model.h"
#include <stdio.h>
#include <string.h>
const char* ui_optional_measurement(const char* value) {
    return value && *value && strcmp(value, "nan") ? value : "unavailable";
}
UiState ui_state(const UiSnapshot* s) {
    if(s->failed) return UiError;
    if(!s->serial_active) return UiOffline;
    if(s->startup) return UiConnecting;
    if(s->data.stopping) return UiStopping;
    if(s->busy) return UiWorking;
    if(s->data.running || s->data.file_open) return UiRecording;
    if(s->data.ready && !s->data.storage_ready) return UiNoMemory;
    if(s->data.ready && !s->data.wifi) return UiNoWifi;
    if(s->data.ready && s->data.idle) return UiReady;
    return UiConnecting;
}
const char* ui_state_title(UiState state) {
    static const char* const titles[] = {"Feberis Pro",   "Connecting",       "Ready",
                                         "Recording",     "Stopping",         "Please wait",
                                         "Session error", "WiFi unavailable", "PSRAM unavailable"};
    return titles[state];
}
const char* ui_primary_action(UiState state) {
    switch(state) {
    case UiOffline:
        return "Connect";
    case UiReady:
        return "Start";
    case UiRecording:
        return "Stop";
    case UiError:
        return "Retry";
    case UiNoWifi:
    case UiNoMemory:
        return "Retry";
    default:
        return 0;
    }
}
bool ui_settings_allowed(const UiSnapshot* s) {
    return !s->busy && !s->startup && !s->failed && !s->data.running && !s->data.file_open &&
           !s->data.stopping && !s->data.start_confirmation_pending &&
           (!s->serial_active || (s->data.ready && s->data.idle));
}
bool ui_start_confirmation_needed(const UiSnapshot* s) {
    return s->data.start_confirmation_pending && ui_state(s) == UiReady;
}
void ui_format_count(uint64_t value, char* output, unsigned capacity) {
    if(value >= 1000000000u)
        snprintf(output, capacity, "%llu.%lluG", (unsigned long long)(value / 1000000000u),
                 (unsigned long long)(value / 100000000u % 10));
    else if(value >= 1000000u)
        snprintf(output, capacity, "%llu.%lluM", (unsigned long long)(value / 1000000u),
                 (unsigned long long)(value / 100000u % 10));
    else if(value >= 100000u)
        snprintf(output, capacity, "%lluk", (unsigned long long)(value / 1000u));
    else
        snprintf(output, capacity, "%llu", (unsigned long long)value);
}

void ui_dashboard_snapshot(const UiSnapshot* source, DashboardSnapshot* out) {
    out->state = ui_state(source);
    out->band = source->data.band;
    out->unique = source->data.unique;
    out->rows = source->data.rows;
    out->satellites = source->data.satellites;
    out->fix = source->data.fix;
    out->capture_degraded = source->data.capture_degraded;
    snprintf(out->hdop, sizeof(out->hdop), "%s", source->data.hdop);
    snprintf(out->message, sizeof(out->message), "%s", source->data.message);
}

bool ui_continuous_supported(const char* firmware) {
    return !*firmware || !strcmp(firmware, "0.4.0");
}
unsigned ui_effective_cycle_gap(const Model* model) {
    return !ui_continuous_supported(model->firmware) && model->cycle_gap_ms < 1000
               ? 1000
               : model->cycle_gap_ms;
}
unsigned ui_cycle_gap_preset(const char* firmware, unsigned index) {
    static const unsigned continuous[] = {0, 50, 100, 250};
    static const unsigned legacy[] = {1000, 2000, 5000, 10000};
    if(index >= UI_CYCLE_GAP_COUNT) return 0;
    return ui_continuous_supported(firmware) ? continuous[index] : legacy[index];
}
const char* ui_observation_timing(const char* firmware) {
    return !strcmp(firmware, "0.4.0") ? "Verified: reception callback\nUnverified: no position"
                                      : "Legacy: scan end";
}
