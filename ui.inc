/* Native Flipper UI. Included here so the UART/storage backend stays private. */
#include "dashboard.h"
#include <gui/modules/dialog_ex.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/view_dispatcher.h>

typedef enum {
    ScreenDashboard,
    ScreenMenu,
    ScreenSettings,
    ScreenScanDiagnostics,
    ScreenAps,
    ScreenSessions,
    ScreenText,
    ScreenExit,
    ScreenStartNoGps,
    ScreenCount
} Screen;
typedef enum {
    EventPrimary = 1,
    EventMenu,
    EventAps,
    EventStatus,
    EventCsv,
    EventSettings,
    EventHelp,
    EventScanDiagnostics,
    EventScanMetrics,
    EventExit,
    EventExitCancel,
    EventExitConfirm,
    EventStartNoGpsConfirm,
    EventStartNoGpsCancel,
    EventSession,
    EventGps,
    EventMemory,
    EventPause,
    EventSessions,
    EventExport,
    EventNextNetworks,
    EventPrevNetworks,
    EventNextSessions,
    EventPrevSessions,
    EventRecoverScan,
    EventConfig = 5000,
    EventSelectSession = 6000,
    EventSetBand = 100,
    EventSetCycleGap = 200,
    EventSetLongGap = 300,
    EventAp = 1000
} UiEvent;
typedef struct {
    App* app;
    ViewDispatcher* dispatcher;
    View* dashboard;
    Submenu* menu;
    Submenu* aps;
    Submenu* sessions;
    VariableItem* config_items[13];
    char session_names[SESSION_LIMIT][64];
    unsigned session_count;
    bool refresh_text, refresh_sessions, refresh_aps, refresh_detail;
    VariableItemList* settings;
    VariableItemList* scan_diagnostics;
    VariableItem* long_gap_item;
    VariableItem* band_item;
    VariableItem* cycle_gap_item;
    Widget* text;
    DialogEx* exit_dialog;
    DialogEx* start_dialog;
    UiSnapshot snapshot;
    ApView listed[AP_LIMIT];
    unsigned listed_count;
    Screen screen, text_parent;
    UiEvent text_kind;
    bool request_pending, quitting, start_dialog_fix;
    uint32_t request_at;
} FeberisUi;

static void ui_show(FeberisUi* ui, Screen screen);
static bool ui_event(void* context, uint32_t event);
static bool ui_snapshot(FeberisUi* ui) {
    App* a = ui->app;
    if(furi_mutex_acquire(a->mutex, 0) != FuriStatusOk) return false;
    ui->snapshot.data = a->model;
    ui->snapshot.serial_active = a->serial != NULL;
    ui->snapshot.failed = a->failed;
    ui->snapshot.startup = a->startup;
    ui->snapshot.busy = a->pending != CmdNone;
    ui->session_count = a->session_count;
    memcpy(ui->session_names, a->sessions, sizeof(ui->session_names));
    furi_mutex_release(a->mutex);
    if(ui->request_pending && elapsed(ui->request_at, 150)) ui->request_pending = false;
    return true;
}
static void ui_send(FeberisUi* ui, Action action, unsigned value) {
    ActionRequest request = {.type = action, .value = value};
    if(furi_message_queue_put(ui->app->actions, &request, 0) == FuriStatusOk) {
        ui->request_pending = true;
        ui->request_at = ms();
    }
}
static void ui_dashboard_update(FeberisUi* ui) {
    DashboardSnapshot* model = view_get_model(ui->dashboard);
    ui_dashboard_snapshot(&ui->snapshot, model);
    if(ui->request_pending && !ui->snapshot.failed && ui->snapshot.serial_active &&
       !ui->snapshot.startup && !ui->snapshot.data.stopping)
        model->state = UiWorking;
    if(ui->quitting) {
        model->state = UiStopping;
        snprintf(model->message, sizeof(model->message), "Saving journal and closing UART");
    }
    view_commit_model(ui->dashboard, true);
}
static void ui_menu_selected(void* context, uint32_t event) {
    FeberisUi* ui = context;
    view_dispatcher_send_custom_event(ui->dispatcher, event);
}
static void ui_setting_changed(VariableItem* item) {
    FeberisUi* ui = variable_item_get_context(item);
    for(unsigned i = 1; i < 13; ++i) {
        if(i != 2 && item == ui->config_items[i]) {
            view_dispatcher_send_custom_event(
                ui->dispatcher, EventConfig + i * 3 + variable_item_get_current_value_index(item));
            return;
        }
    }
    uint32_t base = item == ui->band_item       ? EventSetBand
                    : item == ui->long_gap_item ? EventSetLongGap
                                                : EventSetCycleGap;
    view_dispatcher_send_custom_event(ui->dispatcher,
                                      base + variable_item_get_current_value_index(item));
}
static void ui_setting_enter(void* context, uint32_t index) {
    FeberisUi* ui = context;
    if(index == 13) view_dispatcher_send_custom_event(ui->dispatcher, EventScanDiagnostics);
    if(index == 14) view_dispatcher_send_custom_event(ui->dispatcher, EventHelp);
}
static void ui_scan_diagnostic_enter(void* context, uint32_t index) {
    FeberisUi* ui = context;
    if(index == 1) view_dispatcher_send_custom_event(ui->dispatcher, EventScanMetrics);
}
static void ui_settings_update(FeberisUi* ui) {
    static const char* const band_text[] = {"Both", "2.4 GHz", "5 GHz"};
    static const char* const cycle_text[] = {"Continuous", "50 ms", "100 ms", "250 ms"};
    static const char* const legacy_text[] = {"1 s", "2 s", "5 s", "10 s"};
    const UiSnapshot* s = &ui->snapshot;
    bool allowed = ui_settings_allowed(s) && !ui->request_pending;
    bool radio_allowed = s->serial_active && s->data.ready && !s->busy && !s->startup &&
                         !s->failed && !s->data.stopping && !ui->request_pending;
    const char* locked = s->failed                                        ? "Reconnect"
                         : (s->busy || ui->request_pending || s->startup) ? "Wait..."
                                                                          : "Stop first";
    variable_item_set_values_count(ui->band_item, radio_allowed ? 3 : 1);
    variable_item_set_values_count(ui->cycle_gap_item, allowed ? 4 : 1);
    variable_item_set_current_value_index(ui->band_item, radio_allowed ? s->data.band : 0);
    unsigned gap = ui_effective_cycle_gap(&s->data), gap_index = 0;
    bool preset = false;
    for(unsigned i = 0; i < UI_CYCLE_GAP_COUNT; ++i)
        if(gap == ui_cycle_gap_preset(s->data.firmware, i)) {
            gap_index = i;
            preset = true;
        }
    variable_item_set_current_value_index(ui->cycle_gap_item, allowed ? gap_index : 0);
    variable_item_set_current_value_text(
        ui->band_item,
        radio_allowed ? band_text[s->data.band] : (s->serial_active ? locked : "Connect first"));
    char custom_gap[24];
    snprintf(custom_gap, sizeof(custom_gap), "%u ms (diag)", gap);
    variable_item_set_current_value_text(
        ui->cycle_gap_item, !allowed                                    ? locked
                            : !preset                                   ? custom_gap
                            : ui_continuous_supported(s->data.firmware) ? cycle_text[gap_index]
                                                                        : legacy_text[gap_index]);
    static const unsigned long_gaps[] = {0, 1000, 2000, 5000, 10000, 60000};
    unsigned long_index = 0;
    for(unsigned i = 0; i < COUNT_OF(long_gaps); ++i)
        if(gap == long_gaps[i]) long_index = i;
    variable_item_set_values_count(ui->long_gap_item, allowed ? COUNT_OF(long_gaps) : 1);
    variable_item_set_current_value_index(ui->long_gap_item, allowed ? long_index : 0);
    char long_text[24];
    snprintf(long_text, sizeof(long_text), "%u ms", gap);
    variable_item_set_current_value_text(ui->long_gap_item, allowed ? long_text : locked);
    static const char* const modes[] = {"SMART", "FULL", "UNIQUE"};
    for(unsigned i = 1; i < 13; ++i) {
        if(i == 2) continue;
        VariableItem* item = ui->config_items[i];
        bool edit = radio_allowed && (i != 12 || allowed);
        unsigned index = i == 1 || i == 3 ? s->data.config[i]
                         : i == 12        ? (s->data.config[12] == 115200   ? 0
                                             : s->data.config[12] == 460800 ? 1
                                                                            : 2)
                                          : 1;
        variable_item_set_values_count(item, edit ? (i == 3 ? 2 : 3) : 1);
        variable_item_set_current_value_index(item, edit ? index : 0);
        char value[24];
        snprintf(value, sizeof(value), "%lu", (unsigned long)s->data.config[i]);
        variable_item_set_current_value_text(
            item, !edit    ? (s->serial_active ? locked : "Connect first")
                  : i == 1 ? modes[s->data.config[1]]
                  : i == 3 ? (s->data.config[3] ? "ON (probe)" : "OFF passive")
                           : value);
    }
}
static void ui_text_page(FeberisUi* ui, const char* title, const char* body, Screen parent) {
    widget_reset(ui->text);
    widget_add_string_element(ui->text, 4, 10, AlignLeft, AlignBottom, FontPrimary, title);
    widget_add_line_element(ui->text, 4, 14, 123, 14);
    widget_add_text_scroll_element(ui->text, 4, 18, 120, 44, body);
    ui->text_parent = parent;
    ui_show(ui, ScreenText);
}
static void ui_information(FeberisUi* ui, UiEvent kind) {
    const Model* m = &ui->snapshot.data;
    FuriString* text = furi_string_alloc();
    ui->text_kind = kind;
    if(kind == EventScanMetrics) {
        furi_string_printf(text, "Capture: %s\nCycle gap: %u ms\n\n",
                           m->capture_degraded     ? "degraded / unlocalized"
                           : m->scan_metrics_known ? "ready"
                                                   : "unknown",
                           ui_effective_cycle_gap(m));
        if(m->scan_metrics_known) {
            static const char* const labels[FBR_SCAN_METRIC_FIELDS] = {"Boot",
                                                                       "Session",
                                                                       "Requested gap ms",
                                                                       "Dwell ms",
                                                                       "Capture ready",
                                                                       "Radio ms",
                                                                       "Ingest wall ms",
                                                                       "Actual gap ms",
                                                                       "Longest gap ms",
                                                                       "Start period ms",
                                                                       "Ingest CPU us",
                                                                       "Captured frames",
                                                                       "Capture queue drops",
                                                                       "Malformed frames",
                                                                       "Capture cache overflows",
                                                                       "Metadata matched",
                                                                       "Metadata missing",
                                                                       "Obs queue high-water",
                                                                       "Queue pressure ms",
                                                                       "Scan buffer bytes",
                                                                       "Capture cache bytes",
                                                                       "ESP stack free bytes",
                                                                       "Scan capacity"};
            for(unsigned i = 0; i < FBR_SCAN_METRIC_FIELDS; ++i)
                furi_string_cat_printf(text, "%s: %llu\n", labels[i],
                                       (unsigned long long)m->scan_metrics[i]);
        } else
            furi_string_cat(text, "Metrics unavailable\nRequires firmware 0.4.0\n");
        furi_string_cat(
            text, "\nCapture drops lose reception metadata; observation drops are separate.\n"
                  "Continuous adds no delay. Radio sweeps still take seconds.\nReopen to refresh");
        ui_text_page(ui, "Scan metrics", furi_string_get_cstr(text), ScreenScanDiagnostics);
    } else if(kind == EventStatus || kind == EventMemory) {
        furi_string_printf(text,
                           "%s\n%s\n\nFirmware: %s / revision 2\nBaud: %lu\nGPS: %s, %lu "
                           "sat\nHDOP: %s\nGPS age: %lu ms\n\n"
                           "Mode: %s / %s\nPaused: %s\nUnique AP: %llu / %lu\nDetections: %llu\nSD "
                           "records: %llu\nFiltered: %llu\nNo GPS: %llu\n"
                           "Queued: %lu / %lu\nElapsed: %llu s\nTruncated: %lu\nDiscarded: %lu\n\n",
                           ui_state_title(ui_state(&ui->snapshot)), m->message, m->firmware,
                           (unsigned long)m->baud, m->fix ? "valid" : "invalid",
                           (unsigned long)m->satellites, m->hdop, (unsigned long)m->gps_age,
                           m->config[1] == 0   ? "SMART"
                           : m->config[1] == 1 ? "FULL"
                                               : "UNIQUE",
                           bands[m->band], m->paused ? "yes" : "no", (unsigned long long)m->unique,
                           (unsigned long)m->ap_capacity, (unsigned long long)m->observations,
                           (unsigned long long)m->rows, (unsigned long long)m->filtered,
                           (unsigned long long)m->no_gps, (unsigned long)m->queued,
                           (unsigned long)m->queue_capacity,
                           (unsigned long long)(m->elapsed_ms / 1000), (unsigned long)m->truncated,
                           (unsigned long)m->discarded);
        furi_string_cat_printf(text, "Cycle gap: %u ms\nCapture: %s\n\n", ui_effective_cycle_gap(m),
                               m->capture_degraded     ? "degraded (RADIO_CAPTURE_INIT)"
                               : m->scan_metrics_known ? "ready"
                                                       : "not measured");
        if(m->memory_known) {
            furi_string_cat_printf(text, "PSRAM: %s\nStorage: %s\n",
                                   m->memory[1] ? "initialized" : "unavailable",
                                   m->storage_ready ? "ready" : "unavailable");
            static const char* const labels[] = {
                "PSRAM total", "PSRAM free",   "PSRAM min",        "PSRAM largest",  "SRAM free",
                "SRAM min",    "SRAM largest", "Storage reserved", "AP record bytes"};
            for(unsigned i = 2; i <= 10; ++i)
                furi_string_cat_printf(text, "%s: %lu\n", labels[i - 2],
                                       (unsigned long)m->memory[i]);
        } else
            furi_string_cat(text, "Extended memory diagnostics unavailable\n");
        furi_string_cat(text, "\nSession counters (ESP RAM)\n");
        for(unsigned i = 0; i < 21; ++i)
            furi_string_cat_printf(text, "%s: %llu\n", stat_names[i],
                                   (unsigned long long)m->stats[i]);
        furi_string_cat(text, "\nBoth = alternating scan bands\nReopen to refresh");
        ui_text_page(ui, kind == EventMemory ? "Memory / diagnostics" : "Session status",
                     furi_string_get_cstr(text), ScreenMenu);
    } else if(kind == EventGps) {
        const char(*g)[32] = m->gps;
        bool valid = !strcmp(g[0], "1");
        furi_string_printf(
            text,
            "Fix: %s\nUTC: %s\nLatitude: %s\nLongitude: %s\nAltitude m: %s\nHDOP: %s\nSatellites: "
            "%s\nAge ms: %s\nSpeed km/h: %s\nCourse deg: %s\nAccuracy m: %s\n\nObservation timing\n"
            "%s\nNot the router location.\nReopen to refresh",
            valid ? "valid" : "invalid", g[1], valid ? g[2] : "-", valid ? g[3] : "-",
            valid ? ui_optional_measurement(g[4]) : "-", ui_optional_measurement(g[5]), g[6], g[7],
            ui_optional_measurement(g[8]), ui_optional_measurement(g[9]),
            valid && strcmp(g[10], "nan") ? g[10] : "unavailable",
            ui_observation_timing(m->firmware));
        ui_text_page(ui, "GPS", furi_string_get_cstr(text), ScreenMenu);
    } else if(kind == EventCsv) {
        furi_string_printf(
            text,
            "%s\n\n%s\n\nevents.journal: source of truth\nobservations.csv: all "
            "accepted\naccess_points.csv: final AP table\nwigle.csv: valid GPS + "
            "UTC\nmetadata.json / stats.json\n\nExport after STOP + queue drain.\nIncomplete AP "
            "table is marked.\nSession name: Flipper clock\nObservation UTC: GPS only",
            m->file_open ? "Journal open" : "Journal closed",
            *m->path ? m->path : "No session selected");
        if(*m->session_summary) furi_string_cat_printf(text, "\n\n%s", m->session_summary);
        ui_text_page(ui, "Session files", furi_string_get_cstr(text), ScreenMenu);
    } else {
        furi_string_set(text, "Feberis Pro UART0\n115200 / 8N1 / 3.3V\n\n"
                              "ESP GPIO11 TX\n  -> Flipper UART RX\nESP GPIO12 RX\n  <- Flipper "
                              "UART TX\nGND <-> GND\n\n"
                              "Fixed USART connection\nPress Connect to begin\nUART stays in use "
                              "until\nyou exit this app\n\n"
                              "Power Feberis separately\nNo 5V output is enabled\n\n"
                              "GPS stays on Feberis:\nUART1 at 115200\nGPIO14 RX / GPIO13 TX\n\n"
                              "No GPS fix?\nJournal still records\nWiGLE needs valid GPS + UTC");
        ui_text_page(ui, "Connection help", furi_string_get_cstr(text),
                     ui->screen == ScreenSettings ? ScreenSettings : ScreenMenu);
    }
    furi_string_free(text);
}
static void ui_ap_detail(FeberisUi* ui, unsigned index) {
    if(index >= ui->listed_count) return;
    const ApView* ap = &ui->listed[index];
    if(ap->aggregate) {
        if(ap->boot != ui->snapshot.data.boot || ap->session != ui->snapshot.data.session) {
            ui_text_page(ui, "Network details",
                         "Previous session snapshot.\nOpen its CSV on SD for aggregates.",
                         ScreenAps);
            return;
        }
        ui_send(ui, ActionNetworkDetail, ap->slot);
        ui->refresh_detail = true;
        ui_text_page(ui, "Network details", "Reading aggregate from ESP...", ScreenAps);
        return;
    }
    FuriString* text = furi_string_alloc_printf(
        "%s\n\nBSSID\n%s\n\nSignal: %d dBm\nChannel: %d\nGPS: %s\n\nSecurity\n%s\n\n"
        "%s\n%s\nCaptured AP snapshot\nBack: return to AP list",
        *ap->ssid ? ap->ssid : "Hidden network", ap->bssid, ap->rssi, ap->channel,
        ap->fix ? "Valid fix" : "No fix", ap->auth,
        ap->rx_unverified      ? "RX_UNVERIFIED: no position"
        : ap->reception_timing ? "Verified reception metadata"
                               : "Legacy scan-end observation",
        ui_observation_timing(ap->reception_timing ? "0.4.0" : "0.3.1"));
    ui_text_page(ui, "Network details", furi_string_get_cstr(text), ScreenAps);
    furi_string_free(text);
}
static void ui_ap_list(FeberisUi* ui) {
    submenu_reset(ui->aps);
    ui->listed_count = ui->snapshot.data.ap_count;
    submenu_set_header(ui->aps, "Networks / snapshot");
    if(ui->snapshot.data.network_page)
        submenu_add_item(ui->aps, "Previous page", EventPrevNetworks, ui_menu_selected, ui);
    if(((uint64_t)ui->snapshot.data.network_page + 1) * AP_LIMIT < ui->snapshot.data.unique)
        submenu_add_item(ui->aps, "Next page", EventNextNetworks, ui_menu_selected, ui);
    submenu_add_item(ui->aps, "Refresh list", EventAps, ui_menu_selected, ui);
    for(unsigned i = 0; i < ui->listed_count; ++i) {
        /* Newest first; freeze names and details together until explicit refresh. */
        ui->listed[i] = ui->snapshot.data
                            .aps[ui->snapshot.data.aps[0].aggregate ? i : ui->listed_count - 1 - i];
        const ApView* ap = &ui->listed[i];
        char label[64];
        snprintf(label, sizeof(label), "%ddB  %s", ap->rssi, *ap->ssid ? ap->ssid : "[hidden]");
        submenu_add_item(ui->aps, label, EventAp + i, ui_menu_selected, ui);
    }
    if(!ui->listed_count)
        submenu_add_item(ui->aps, "No AP yet - refresh", EventAps, ui_menu_selected, ui);
    submenu_set_selected_item(ui->aps, ui->listed_count ? EventAp : EventAps);
    ui_show(ui, ScreenAps);
}
static void ui_exit_result(DialogExResult result, void* context) {
    FeberisUi* ui = context;
    view_dispatcher_send_custom_event(
        ui->dispatcher, result == DialogExResultRight ? EventExitConfirm : EventExitCancel);
}
static void ui_start_result(DialogExResult result, void* context) {
    FeberisUi* ui = context;
    view_dispatcher_send_custom_event(ui->dispatcher, result == DialogExResultRight
                                                          ? EventStartNoGpsConfirm
                                                          : EventStartNoGpsCancel);
}
static void ui_start_without_gps(FeberisUi* ui) {
    ui->start_dialog_fix = ui->snapshot.data.fix;
    dialog_ex_set_header(ui->start_dialog,
                         ui->start_dialog_fix ? "GPS fix acquired" : "Start without GPS?", 64, 8,
                         AlignCenter, AlignTop);
    dialog_ex_set_text(ui->start_dialog,
                       ui->start_dialog_fix ? "GPS position available.\nStart this session?"
                                            : "GPS fix unavailable.\nStart without position?",
                       64, 31, AlignCenter, AlignCenter);
    dialog_ex_set_left_button_text(ui->start_dialog, "Cancel");
    dialog_ex_set_right_button_text(ui->start_dialog, "Start");
    ui_show(ui, ScreenStartNoGps);
}
static void ui_finish(FeberisUi* ui) {
    if(ui->quitting) return;
    ActionRequest request = {.type = ActionQuit};
    if(furi_message_queue_put(ui->app->actions, &request, 0) != FuriStatusOk) return;
    ui->quitting = true;
    ui_show(ui, ScreenDashboard);
}
static void ui_exit_request(FeberisUi* ui) {
    if(ui->snapshot.data.running || ui->snapshot.data.file_open || ui->snapshot.busy ||
       ui->snapshot.startup) {
        dialog_ex_set_header(ui->exit_dialog, "Stop and exit?", 64, 8, AlignCenter, AlignTop);
        dialog_ex_set_text(ui->exit_dialog, "Save the journal\nand stop Feberis Pro", 64, 31,
                           AlignCenter, AlignCenter);
        dialog_ex_set_left_button_text(ui->exit_dialog, "Stay");
        dialog_ex_set_right_button_text(ui->exit_dialog, "Exit");
        ui_show(ui, ScreenExit);
    } else
        ui_finish(ui);
}
static void ui_show(FeberisUi* ui, Screen screen) {
    ui->screen = screen;
    if(screen == ScreenSettings || screen == ScreenScanDiagnostics) ui_settings_update(ui);
    if(screen == ScreenDashboard) ui_dashboard_update(ui);
    view_dispatcher_switch_to_view(ui->dispatcher, screen);
}
static bool ui_back(void* context) {
    FeberisUi* ui = context;
    if(ui->quitting) return true;
    switch(ui->screen) {
    case ScreenDashboard:
        ui_exit_request(ui);
        break;
    case ScreenMenu:
        ui_show(ui, ScreenDashboard);
        break;
    case ScreenScanDiagnostics:
        ui_show(ui, ScreenSettings);
        break;
    case ScreenSettings:
    case ScreenAps:
    case ScreenSessions:
        ui_show(ui, ScreenMenu);
        break;
    case ScreenText:
        ui_show(ui, ui->text_parent);
        break;
    case ScreenStartNoGps:
        ui_send(ui, ActionCancelStart, 0);
        ui_show(ui, ScreenDashboard);
        break;
    case ScreenExit:
        ui_show(ui, ScreenDashboard);
        break;
    default:
        break;
    }
    return true;
}
static bool ui_dashboard_input(InputEvent* event, void* context) {
    FeberisUi* ui = context;
    if(event->type != InputTypeShort) return false;
    UiEvent command;
    if(event->key == InputKeyLeft)
        command = EventMenu;
    else if(event->key == InputKeyRight)
        command = EventAps;
    else if(event->key == InputKeyOk)
        command = EventPrimary;
    else
        return false;
    view_dispatcher_send_custom_event(ui->dispatcher, command);
    return true;
}
static void ui_sessions(FeberisUi* ui) {
    submenu_reset(ui->sessions);
    submenu_set_header(ui->sessions, "Sessions / SD");
    submenu_add_item(ui->sessions, "Newer page", EventPrevSessions, ui_menu_selected, ui);
    submenu_add_item(ui->sessions, "Older page", EventNextSessions, ui_menu_selected, ui);
    submenu_add_item(ui->sessions, "Refresh", EventSessions, ui_menu_selected, ui);
    for(unsigned i = 0; i < ui->session_count; ++i)
        submenu_add_item(ui->sessions, ui->session_names[i], EventSelectSession + i,
                         ui_menu_selected, ui);
    ui_show(ui, ScreenSessions);
}
static void ui_config_change(FeberisUi* ui, unsigned field, unsigned index) {
    const UiSnapshot* s = &ui->snapshot;
    if(field >= 13 || !field || field == 2 || s->busy || s->startup || s->failed ||
       !s->data.ready || ui->request_pending)
        return;
    static const uint32_t steps[] = {0, 1, 1, 1, 20, 1, 1, 1000, 100, 100, 1, 100, 1};
    static const uint32_t minimum[] = {1, 0, 0, 0, 20, 1, 1, 1000, 0, 100, 1, 0, 0};
    static const uint32_t maximum[] = {1,       2,     2,     1,   1500,  10000, 100,
                                       3600000, 60000, 10000, 500, 60000, 2};
    uint32_t value = s->data.config[field];
    if(field == 1 || field == 3 || field == 12)
        value = index;
    else {
        if(index == 1) return;
        if(index == 0)
            value = value > minimum[field] + steps[field] ? value - steps[field] : minimum[field];
        else
            value = value + steps[field] < maximum[field] ? value + steps[field] : maximum[field];
    }
    ui_send(ui, ActionConfig, (field << 24) | value);
}
static bool ui_event(void* context, uint32_t event) {
    FeberisUi* ui = context;
    if(ui->quitting) return true;
    /* Never block the dispatcher or GUI on UART/SD. Leave the last frame visible. */
    ui_snapshot(ui);
    if(ui->screen == ScreenStartNoGps && event != EventStartNoGpsConfirm &&
       event != EventStartNoGpsCancel)
        return true;
    if(event >= EventConfig && event < EventConfig + 39) {
        ui_config_change(ui, (event - EventConfig) / 3, (event - EventConfig) % 3);
    } else if(event >= EventSelectSession && event < EventSelectSession + SESSION_LIMIT) {
        ui_send(ui, ActionSelectSession, event - EventSelectSession);
        ui->text_kind = EventCsv;
        ui->refresh_text = true;
        ui_information(ui, EventCsv);
    } else if(event >= EventAp && event < EventAp + AP_LIMIT) {
        ui_ap_detail(ui, event - EventAp);
    } else if(event >= EventSetBand && event < EventSetBand + 3) {
        if(ui->snapshot.data.ready && !ui->snapshot.busy && !ui->snapshot.failed &&
           !ui->request_pending)
            ui_send(ui, ActionBand, event - EventSetBand);
    } else if(event >= EventSetLongGap && event < EventSetLongGap + 6) {
        static const unsigned gaps[] = {0, 1000, 2000, 5000, 10000, 60000};
        if(ui_settings_allowed(&ui->snapshot) && !ui->request_pending)
            ui_send(ui, ActionCycleGap, gaps[event - EventSetLongGap]);
    } else if(event >= EventSetCycleGap && event < EventSetCycleGap + UI_CYCLE_GAP_COUNT) {
        if(ui_settings_allowed(&ui->snapshot) && !ui->request_pending)
            ui_send(ui, ActionCycleGap,
                    ui_cycle_gap_preset(ui->snapshot.data.firmware, event - EventSetCycleGap));
    } else
        switch(event) {
        case EventStartNoGpsConfirm:
            if(ui->screen == ScreenStartNoGps && ui->snapshot.data.start_confirmation_pending) {
                ui_send(ui, ActionConfirmStart, 0);
                ui_show(ui, ScreenDashboard);
            }
            break;
        case EventStartNoGpsCancel:
            if(ui->screen == ScreenStartNoGps) {
                ui_send(ui, ActionCancelStart, 0);
                ui_show(ui, ScreenDashboard);
            }
            break;
        case EventPrimary: {
            if(ui->request_pending) break;
            UiState state = ui_state(&ui->snapshot);
            if(state == UiOffline || state == UiError || state == UiNoWifi || state == UiNoMemory)
                ui_send(ui, ActionConnect, 0);
            else if(state == UiReady || state == UiRecording)
                ui_send(ui, ActionToggle, 0);
            ui_show(ui, ScreenDashboard);
            break;
        }
        case EventSession:
            ui_show(ui, ScreenDashboard);
            break;
        case EventMenu:
            ui_show(ui, ScreenMenu);
            break;
        case EventPrevNetworks:
        case EventNextNetworks:
        case EventAps: {
            unsigned page = event == EventAps ? 0 : ui->snapshot.data.network_page;
            if(event == EventNextNetworks) ++page;
            if(event == EventPrevNetworks && page) --page;
            if(ui->snapshot.data.ready && !ui->snapshot.busy) {
                ui_send(ui, ActionNetworks, page);
                ui->refresh_aps = true;
            }
            ui_ap_list(ui);
        } break;
        case EventScanDiagnostics:
            ui_show(ui, ScreenScanDiagnostics);
            break;
        case EventScanMetrics:
            ui_send(ui, ActionScanMetrics, 0);
            ui->refresh_text = true;
            ui_information(ui, event);
            break;
        case EventSettings:
            ui_show(ui, ScreenSettings);
            break;
        case EventRecoverScan:
            ui_send(ui, ActionRecoverScan, 0);
            ui_show(ui, ScreenDashboard);
            break;
        case EventPrevSessions:
        case EventNextSessions:
        case EventSessions:
            if(ui->snapshot.data.file_open) {
                ui_text_page(ui, "Sessions", "Stop before browsing archived sessions.", ScreenMenu);
                break;
            }
            ui_send(ui, ActionSessions,
                    event == EventNextSessions   ? 1
                    : event == EventPrevSessions ? 2
                                                 : 0);
            ui->refresh_sessions = true;
            ui_sessions(ui);
            break;
        case EventExport:
            ui_send(ui, ActionExport, 0);
            ui_show(ui, ScreenDashboard);
            break;
        case EventPause:
            ui_send(ui, ActionPause, 0);
            ui_show(ui, ScreenDashboard);
            break;
        case EventGps:
        case EventMemory:
            ui_send(ui, event == EventGps ? ActionGps : ActionDiagnostics, 0);
            ui->refresh_text = true;
            ui_information(ui, event);
            break;
        case EventStatus:
        case EventCsv:
        case EventHelp:
            ui_information(ui, event);
            break;
        case EventExit:
            ui_exit_request(ui);
            break;
        case EventExitCancel:
            ui_show(ui, ScreenDashboard);
            break;
        case EventExitConfirm:
            ui_finish(ui);
            break;
        default:
            return false;
        }
    return true;
}
static void ui_tick(void* context) {
    FeberisUi* ui = context;
    if(ui->quitting && furi_thread_get_state(ui->app->thread) == FuriThreadStateStopped) {
        view_dispatcher_stop(ui->dispatcher);
        return;
    }
    if(!ui_snapshot(ui)) return;
    if(!ui->quitting && !ui->request_pending && ui_start_confirmation_needed(&ui->snapshot) &&
       ui->screen != ScreenStartNoGps)
        ui_start_without_gps(ui);
    if(ui->screen == ScreenStartNoGps && !ui->snapshot.data.start_confirmation_pending)
        ui_show(ui, ScreenDashboard);
    if(ui->screen == ScreenStartNoGps) {
        if(ui->start_dialog_fix != ui->snapshot.data.fix) ui_start_without_gps(ui);
        return;
    }
    if(ui->refresh_detail && !ui->snapshot.busy && !ui->request_pending) {
        ui->refresh_detail = false;
        if(ui->screen == ScreenText)
            ui_text_page(ui, "Network details",
                         *ui->snapshot.data.network_detail
                             ? ui->snapshot.data.network_detail
                             : "No aggregate available; reopen to retry",
                         ScreenAps);
    }
    if(ui->refresh_aps && !ui->snapshot.busy && !ui->request_pending &&
       !ui->snapshot.data.network_loading) {
        ui->refresh_aps = false;
        if(ui->screen == ScreenAps) ui_ap_list(ui);
    }
    if(ui->refresh_sessions && !ui->request_pending) {
        ui->refresh_sessions = false;
        ui_sessions(ui);
    }
    if(ui->refresh_text && !ui->snapshot.busy && !ui->request_pending) {
        ui->refresh_text = false;
        if(ui->screen == ScreenText) ui_information(ui, ui->text_kind);
    }
    if(ui->screen == ScreenDashboard) ui_dashboard_update(ui);
    if(ui->screen == ScreenSettings || ui->screen == ScreenScanDiagnostics) ui_settings_update(ui);
}
static FeberisUi* ui_alloc(App* app, Gui* gui) {
    FeberisUi* ui = calloc(1, sizeof(FeberisUi));
    ui->app = app;
    ui->dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(ui->dispatcher, ui);
    view_dispatcher_set_custom_event_callback(ui->dispatcher, ui_event);
    view_dispatcher_set_navigation_event_callback(ui->dispatcher, ui_back);
    view_dispatcher_set_tick_event_callback(ui->dispatcher, ui_tick, furi_ms_to_ticks(200));
    ui->dashboard = view_alloc();
    view_allocate_model(ui->dashboard, ViewModelTypeLocking, sizeof(DashboardSnapshot));
    view_set_context(ui->dashboard, ui);
    view_set_draw_callback(ui->dashboard, dashboard_draw);
    view_set_input_callback(ui->dashboard, ui_dashboard_input);
    ui->menu = submenu_alloc();
    submenu_set_header(ui->menu, "Feberis Pro");
    submenu_add_item(ui->menu, "Session", EventSession, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Networks", EventAps, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Session status", EventStatus, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Session files", EventCsv, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Recover and resume", EventRecoverScan, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Pause / resume", EventPause, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "GPS", EventGps, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Memory diagnostics", EventMemory, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Sessions", EventSessions, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Export selected session", EventExport, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Settings", EventSettings, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Connection help", EventHelp, ui_menu_selected, ui);
    submenu_add_item(ui->menu, "Exit", EventExit, ui_menu_selected, ui);
    ui->aps = submenu_alloc();
    ui->sessions = submenu_alloc();
    ui->settings = variable_item_list_alloc();
    ui->band_item = variable_item_list_add(ui->settings, "Band", 3, ui_setting_changed, ui);
    ui->cycle_gap_item = variable_item_list_add(ui->settings, "Cycle gap", UI_CYCLE_GAP_COUNT,
                                                ui_setting_changed, ui);
    static const char* const config_labels[] = {"",
                                                "Mode",
                                                "",
                                                "Active probing",
                                                "Dwell ms",
                                                "Distance m",
                                                "RSSI delta dB",
                                                "Max gap ms",
                                                "Min gap ms",
                                                "GPS max age ms",
                                                "GPS HDOP x10",
                                                "FULL gap ms",
                                                "Baud"};
    for(unsigned i = 1; i < 13; ++i) {
        if(i != 2)
            ui->config_items[i] = variable_item_list_add(ui->settings, config_labels[i],
                                                         i == 3 ? 2 : 3, ui_setting_changed, ui);
    }
    variable_item_list_add(ui->settings, "Scan diagnostics", 0, NULL, NULL);
    variable_item_list_add(ui->settings, "Connection help", 0, NULL, NULL);
    variable_item_list_set_enter_callback(ui->settings, ui_setting_enter, ui);
    ui->scan_diagnostics = variable_item_list_alloc();
    ui->long_gap_item =
        variable_item_list_add(ui->scan_diagnostics, "Long cycle gap", 6, ui_setting_changed, ui);
    variable_item_list_add(ui->scan_diagnostics, "Read metrics", 0, NULL, NULL);
    variable_item_list_set_enter_callback(ui->scan_diagnostics, ui_scan_diagnostic_enter, ui);
    ui->text = widget_alloc();
    ui->start_dialog = dialog_ex_alloc();
    dialog_ex_set_context(ui->start_dialog, ui);
    dialog_ex_set_result_callback(ui->start_dialog, ui_start_result);
    ui->exit_dialog = dialog_ex_alloc();
    dialog_ex_set_context(ui->exit_dialog, ui);
    dialog_ex_set_result_callback(ui->exit_dialog, ui_exit_result);
    View* views[] = {ui->dashboard,
                     submenu_get_view(ui->menu),
                     variable_item_list_get_view(ui->settings),
                     variable_item_list_get_view(ui->scan_diagnostics),
                     submenu_get_view(ui->aps),
                     submenu_get_view(ui->sessions),
                     widget_get_view(ui->text),
                     dialog_ex_get_view(ui->exit_dialog),
                     dialog_ex_get_view(ui->start_dialog)};
    for(unsigned i = 0; i < ScreenCount; ++i) view_dispatcher_add_view(ui->dispatcher, i, views[i]);
    ui_snapshot(ui);
    ui_show(ui, ScreenDashboard);
    view_dispatcher_attach_to_gui(ui->dispatcher, gui, ViewDispatcherTypeFullscreen);
    return ui;
}
static void ui_free(FeberisUi* ui) {
    for(unsigned i = 0; i < ScreenCount; ++i) view_dispatcher_remove_view(ui->dispatcher, i);
    view_free(ui->dashboard);
    submenu_free(ui->menu);
    submenu_free(ui->aps);
    submenu_free(ui->sessions);
    variable_item_list_free(ui->settings);
    variable_item_list_free(ui->scan_diagnostics);
    widget_free(ui->text);
    dialog_ex_free(ui->exit_dialog);
    dialog_ex_free(ui->start_dialog);
    view_dispatcher_free(ui->dispatcher);
    free(ui);
}
int32_t feberis_app(void* context) {
    UNUSED(context);
    App* a = calloc(1, sizeof(App));
    a->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    a->actions = furi_message_queue_alloc(8, sizeof(ActionRequest));
    a->rx = furi_stream_buffer_alloc(4096, 1);
    a->storage = furi_record_open(RECORD_STORAGE);
    a->file = storage_file_alloc(a->storage);
    a->store.storage = a->storage;
    a->store.file = a->file;
    a->baud = a->model.baud = 115200;
    a->model.cycle_gap_ms = 0;
    if(!load_settings(a)) message(a, "Settings invalid; using defaults");
    message(a, "Press OK to connect");
    Expansion* expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(expansion);
    a->thread = furi_thread_alloc_ex("FeberisWorker", 8192, worker, a);
    furi_thread_start(a->thread);
    Gui* gui = furi_record_open(RECORD_GUI);
    FeberisUi* ui = ui_alloc(a, gui);
    view_dispatcher_run(ui->dispatcher);
    ActionRequest quit = {.type = ActionQuit};
    if(!ui->quitting) furi_message_queue_put(a->actions, &quit, FuriWaitForever);
    furi_thread_join(a->thread);
    ui_free(ui);
    furi_thread_free(a->thread);
    storage_file_free(a->file);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    expansion_enable(expansion);
    furi_record_close(RECORD_EXPANSION);
    furi_stream_buffer_free(a->rx);
    furi_message_queue_free(a->actions);
    furi_mutex_free(a->mutex);
    free(a);
    return 0;
}
