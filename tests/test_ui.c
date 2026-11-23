#include "../ui_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    UiSnapshot s = {0};
    assert(ui_state(&s) == UiOffline && !strcmp(ui_primary_action(ui_state(&s)), "Connect"));
    assert(ui_settings_allowed(&s));
    s.serial_active = true;
    s.startup = true;
    assert(ui_state(&s) == UiConnecting && !ui_primary_action(ui_state(&s)));
    assert(!ui_settings_allowed(&s));
    s.startup = false;
    s.data.ready = s.data.idle = s.data.wifi = s.data.storage_ready = true;
    assert(ui_state(&s) == UiReady && !strcmp(ui_primary_action(ui_state(&s)), "Start"));
    assert(ui_settings_allowed(&s));
    s.busy = true;
    assert(ui_state(&s) == UiWorking && !ui_primary_action(ui_state(&s)));
    assert(!ui_settings_allowed(&s));
    s.busy = false;
    s.data.file_open = s.data.running = true;
    assert(ui_state(&s) == UiRecording && !strcmp(ui_primary_action(ui_state(&s)), "Stop"));
    assert(!ui_settings_allowed(&s));
    s.data.stopping = true;
    assert(ui_state(&s) == UiStopping && !ui_primary_action(ui_state(&s)));
    s.failed = true;
    assert(ui_state(&s) == UiError && !strcmp(ui_primary_action(ui_state(&s)), "Retry"));
    assert(!ui_settings_allowed(&s));
    s.failed = s.data.stopping = s.data.running = s.data.file_open = s.data.wifi = false;
    assert(ui_state(&s) == UiNoWifi && !strcmp(ui_primary_action(ui_state(&s)), "Retry"));
    s.data.storage_ready = false;
    assert(ui_state(&s) == UiNoMemory);
    char count[24];
    ui_format_count(UINT32_MAX, count, sizeof(count));
    assert(!strcmp(count, "4.2G"));
    ui_format_count(1837, count, sizeof(count));
    assert(!strcmp(count, "1837"));
    ui_format_count(1037000, count, sizeof(count));
    assert(!strcmp(count, "1.0M"));
    ui_format_count(999999, count, sizeof(count));
    assert(!strcmp(count, "999k"));
    ui_format_count(UINT64_MAX, count, sizeof(count));
    assert(!strcmp(count, "18446744073.7G"));
    DashboardSnapshot compact;
    s.data.unique = UINT64_MAX;
    s.data.rows = 123;
    ui_dashboard_snapshot(&s, &compact);
    assert(compact.unique == UINT64_MAX && compact.rows == 123 && compact.state == UiNoMemory);
    assert(sizeof(compact) < 128);
    puts("ui: state priority, visible actions and setting locks OK");
}
