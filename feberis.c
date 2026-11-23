#ifdef FBR_HOST_TEST
#include "tests/platform.h"
#else
#include <expansion/expansion.h>
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#endif
#include "protocol.h"
#include "session_store.h"
#include "ui_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RX_ERROR (1u << 0)
#define TIMEOUT_MS 2000

typedef enum {
    CmdNone,
    CmdStop,
    CmdHello,
    CmdStatus,
    CmdStart,
    CmdPing,
    CmdInfo,
    CmdConfig,
    CmdMemory,
    CmdState,
    CmdStats,
    CmdAp,
    CmdResumeLink,
    CmdNack,
    CmdSetConfig,
    CmdPause,
    CmdResume,
    CmdGps,
    CmdApDetail
} Command;
typedef enum {
    ActionConnect,
    ActionToggle,
    ActionBand,
    ActionInterval,
    ActionQuit,
    ActionConfig,
    ActionPause,
    ActionGps,
    ActionDiagnostics,
    ActionExport,
    ActionResume,
    ActionSessions,
    ActionSelectSession,
    ActionNetworks,
    ActionNetworkDetail,
    ActionRecoverScan
} Action;
typedef struct {
    Action type;
    unsigned value;
} ActionRequest;
#define SESSION_LIMIT 16
typedef struct {
    Model model;
    FuriMutex* mutex;
    FuriMessageQueue* actions;
    FuriStreamBuffer* rx;
    FuriThread* thread;
    FuriHalSerialHandle* serial;
    FuriHalSerialId port;
    Storage* storage;
    File* file;
    SessionStore store;
    FbrReader reader;
    char raw[FBR_LINE_SIZE];
    FbrAp ap;
    uint32_t boot, session, sequence, next_id, pending_id, sent_at, last_status, last_poll;
    uint32_t committed_remote, last_sequence, config_edit[13], baud, quit_at;
    bool boot_known, have_sequence, failed, closing, startup, quit, state_known;
    bool need_nack, storage_failed, resume_after_config, config_wait, finishing, stop_confirmed;
    bool start_uncertain, quit_stop_sent, resume_requested;
    Command pending;
    unsigned retries, baud_probe, archive_slot, archive_count, archive_stage;
    char payload[256], info_frame[192], config_frame[256], state_frame[256];
    char sessions[SESSION_LIMIT][64];
    unsigned session_count;
    bool table_view;
    volatile bool quiet_rx;
    uint32_t detail_slot, network_next, network_end;
} App;
static const unsigned intervals[] = {1000, 2000, 5000, 10000};
static const uint32_t bauds[] = {115200, 460800, 921600};
static const char* const bands[] = {"AUTO", "2G", "5G"};
static uint32_t ms(void) { return furi_get_tick(); }
static bool elapsed(uint32_t since, uint32_t duration) {
    return (uint32_t)(ms() - since) >= furi_ms_to_ticks(duration);
}
static void message(App* a, const char* text) {
    snprintf(a->model.message, sizeof(a->model.message), "%s", text);
}
static void transmit(App* a, const char* payload) {
    char frame[272];
    size_t size = fbr_encode(payload, frame, sizeof(frame));
    if(size && a->serial) furi_hal_serial_tx(a->serial, (const uint8_t*)frame, size);
}
static uint32_t request_id(App* a) {
    if(++a->next_id == 0) ++a->next_id;
    return a->next_id;
}
static bool copy_frame(App* a, char* out, size_t capacity) {
    size_t n = strlen(a->raw);
    if(n >= capacity) return false;
    memcpy(out, a->raw, n + 1);
    return true;
}
static void set_baud(App* a, uint32_t baud) {
    a->baud = a->model.baud = baud;
#ifndef FBR_HOST_TEST
    furi_hal_serial_set_br(a->serial, baud);
#endif
    memset(&a->reader, 0, sizeof(a->reader));
}
static void command(App* a, Command cmd) {
    if(a->pending != CmdNone || !a->serial) return;
    static const char* const names[] = {
        "",           "STOP_SCAN",   "HELLO",       "STATUS",     "START_SCAN",
        "PING",       "GET_INFO",    "GET_CONFIG",  "GET_MEMORY", "GET_STATUS",
        "GET_STATS",  "GET_AP",      "RESUME_LINK", "NACK",       "SET_CONFIG",
        "PAUSE_SCAN", "RESUME_SCAN", "GET_GPS",     "GET_AP"};
    a->pending_id = request_id(a);
    size_t used = snprintf(a->payload, sizeof(a->payload), "FBR1\t%lu\t%s",
                           (unsigned long)a->pending_id, names[cmd]);
    if(cmd == CmdHello)
        snprintf(a->payload + used, sizeof(a->payload) - used, "\t2");
    else if(cmd == CmdStart)
        snprintf(a->payload + used, sizeof(a->payload) - used, "\t%u",
                 intervals[a->model.interval]);
    else if(cmd == CmdAp || cmd == CmdApDetail)
        snprintf(a->payload + used, sizeof(a->payload) - used, "\t%lu",
                 (unsigned long)(cmd == CmdApDetail ? a->detail_slot
                                 : a->finishing     ? a->archive_slot
                                                    : a->network_next));
    else if(cmd == CmdNack)
        snprintf(a->payload + used, sizeof(a->payload) - used, "\tSTORAGE");
    else if(cmd == CmdResumeLink)
        snprintf(a->payload + used, sizeof(a->payload) - used, "\t%lu\t%lu\t%lu",
                 (unsigned long)a->store.boot, (unsigned long)a->store.session,
                 (unsigned long)a->store.committed);
    else if(cmd == CmdSetConfig)
        for(unsigned i = 0; i < 13; ++i)
            used += snprintf(a->payload + used, sizeof(a->payload) - used, "\t%lu",
                             (unsigned long)a->config_edit[i]);
    a->pending = cmd;
    a->retries = 0;
    a->sent_at = ms();
    transmit(a, a->payload);
}
/* Long scans/exports run only before HELLO or after the transport queue drains.
 * Consume IRQ bytes without buffering periodic telemetry during these operations.
 * Retained ESP observations are requested again by HELLO after recovery. */
static void quiet_io(App* a, bool begin) {
    if(begin) {
        a->quiet_rx = true;
        return;
    }
#ifndef FBR_HOST_TEST
    furi_stream_buffer_reset(a->rx);
    furi_thread_flags_clear(RX_ERROR);
#endif
    memset(&a->reader, 0, sizeof(a->reader));
    a->last_status = ms();
    a->quiet_rx = false;
}
static bool export_store(App* a, SessionStore* store) {
    quiet_io(a, true);
    bool ok = store_export(store);
    quiet_io(a, false);
    return ok;
}
static bool settings_fields(char** f, size_t n, unsigned* port, uint32_t* baud,
                            unsigned* interval) {
    uint32_t p, i;
    if(n != 7 || strcmp(f[1], "0") || strcmp(f[2], "SETTINGS") || !fbr_u32(f[3], &p) || p > 1 ||
       !fbr_u32(f[4], baud) || (*baud != 115200 && *baud != 460800 && *baud != 921600) ||
       !fbr_u32(f[5], &i) || i >= COUNT_OF(intervals))
        return false;
    if(strcmp(f[6], "-")) {
        const char* prefix = SESSION_DIR "/session_";
        if(strncmp(f[6], prefix, strlen(prefix)) || strlen(f[6]) >= STORE_PATH) return false;
        for(const char* c = f[6] + strlen(prefix); *c; ++c)
            if((*c < '0' || *c > '9') && *c != '_') return false;
    }
    *port = p;
    *interval = i;
    return true;
}
/* Append framed preferences. A torn update retains the previous valid pointer;
 * committed always comes from events.journal, never from this file. SDK rename
 * uses copy+remove, so it cannot safely publish a single recovery pointer. */
typedef struct {
    unsigned port, interval;
    uint32_t baud;
    char path[STORE_PATH];
} LocalSettings;
static bool settings_read(LocalSettings* local, File* file, uint32_t* good) {
    StoreReader* r = calloc(1, sizeof(StoreReader));
    if(!r) return false;
    char* f[FBR_FIELDS];
    *good = 0;
    int result;
    while((result = store_next(file, r)) == 1) {
        unsigned port, interval;
        uint32_t baud;
        size_t n = fbr_decode(r->line.data, f);
        if(!settings_fields(f, n, &port, &baud, &interval) || r->end > UINT32_MAX) break;
        local->port = port;
        local->baud = baud;
        local->interval = interval;
        const char* path = strcmp(f[6], "-") ? f[6] : "";
        memcpy(local->path, path, strlen(path) + 1);
        *good = r->end;
    }
    bool ok = storage_file_get_error(file) == FSE_OK;
    free(r);
    return ok;
}
static bool save_settings(App* a) {
    quiet_io(a, true);
    if(!store_dirs(a->storage)) {
        quiet_io(a, false);
        return false;
    }
    File* f = storage_file_alloc(a->storage);
    bool ok = storage_file_open(f, CONFIG_DIR "/settings.conf", FSAM_READ_WRITE, FSOM_OPEN_APPEND);
    /* Scan without overwriting the user's newly chosen values. */
    LocalSettings previous = {0};
    uint32_t good = 0;
    if(ok) {
        ok = storage_file_seek(f, 0, true) && settings_read(&previous, f, &good);
        if(ok) ok = storage_file_seek(f, good, true) && storage_file_truncate(f);
    }
    char data[STORE_PATH + 96], frame[STORE_PATH + 112];
    snprintf(data, sizeof(data), "FBR1\t0\tSETTINGS\t%u\t%lu\t%u\t%s",
             a->port == FuriHalSerialIdLpuart, (unsigned long)a->baud, a->model.interval,
             *a->store.path ? a->store.path : "-");
    size_t size = fbr_encode(data, frame, sizeof(frame));
    ok = ok && size && store_write(f, frame, size) && storage_file_sync(f);
    if(!storage_file_close(f)) ok = false;
    storage_file_free(f);
    quiet_io(a, false);
    return ok;
}
static bool load_settings(App* a) {
    File* f = storage_file_alloc(a->storage);
    bool ok = true;
    if(storage_file_open(f, CONFIG_DIR "/settings.conf", FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint32_t good;
        LocalSettings local = {0};
        ok = settings_read(&local, f, &good);
        if(ok && good) {
            a->port = local.port ? FuriHalSerialIdLpuart : FuriHalSerialIdUsart;
            a->baud = a->model.baud = local.baud;
            a->model.interval = local.interval;
            strcpy(a->store.path, local.path);
        }
        if(!storage_file_close(f)) ok = false;
    }
    storage_file_free(f);
    return ok;
}
static void fail(App* a, const char* text) {
    a->failed = true;
    a->startup = false;
    a->model.ready = false;
    message(a, text);
    store_log(a->storage, text);
}
static void storage_fail(App* a) {
    a->need_nack = a->storage_failed = true;
    fail(a, a->store.damaged ? "Journal damaged; recover on SD" : "SD error; repair SD and Retry");
    store_close(&a->store);
    a->model.file_open = false;
}
static bool journal(App* a, const char* frame) {
    if(store_append(&a->store, frame)) return true;
    storage_fail(a);
    return false;
}
static bool local_record(App* a, const char* type, const char* value) {
    if(store_local(&a->store, type, value)) return true;
    storage_fail(a);
    return false;
}
static bool check_boot(App* a, uint32_t boot) {
    if(!a->boot_known) {
        a->boot_known = true;
        a->boot = a->model.boot = boot;
        return true;
    }
    if(a->boot == boot) return true;
    /* Only HELLO/INFO/READY/STATE may establish a new boot, never stale data frames. */
    if(a->store.open) {
        a->store.interrupted = true;
        a->store.complete = false;
        store_finish(&a->store, false);
        store_close(&a->store);
        export_store(a, &a->store);
    }
    a->boot = a->model.boot = boot;
    a->have_sequence = false;
    a->sequence = 0;
    a->model.file_open = a->model.running = false;
    a->model.ready = false;
    a->model.ap_count = 0;
    a->store.path[0] = 0;
    save_settings(a);
    a->pending = CmdNone;
    fail(a, "ESP restarted; old session interrupted");
    return false;
}
static void acknowledge(App* a, uint32_t sequence) {
    char payload[112];
    snprintf(payload, sizeof(payload), "FBR1\t%lu\tACK\t%lu\t%lu\t%lu",
             (unsigned long)request_id(a), (unsigned long)a->boot, (unsigned long)a->session,
             (unsigned long)sequence);
    transmit(a, payload);
}
static bool open_file(App* a) {
    a->store.boot = a->boot;
    a->store.session = a->session;
    a->store.committed = a->committed_remote;
    if(!store_create(&a->store)) {
        storage_fail(a);
        return false;
    }
    a->model.file_open = true;
    a->model.session_summary[0] = 0;
    snprintf(a->model.path, sizeof(a->model.path), "%s", a->store.path);
    if(!save_settings(a) || !journal(a, a->info_frame) || !journal(a, a->config_frame) ||
       !journal(a, a->state_frame)) {
        storage_fail(a);
        return false;
    }
    if(a->session == UINT32_MAX) {
        fail(a, "Session exhausted; restart ESP");
        return false;
    }
    a->store.session = a->session + 1;
    if(!local_record(a, "START_INTENT", "START")) return false;
    a->model.rows = 0;
    a->have_sequence = true;
    a->sequence = a->store.committed;
    return true;
}
static void receive_observation(App* a, char** f, size_t n) {
    FbrAp* ap = &a->ap;
    if(!fbr_observation(f, n, ap) || !a->boot_known || ap->boot != a->boot ||
       ap->scan != a->session || !a->store.open || a->store.boot != ap->boot ||
       a->store.session != ap->scan || a->storage_failed)
        return;
    if(a->have_sequence && ap->sequence <= a->sequence) {
        if(ap->sequence == a->sequence) acknowledge(a, ap->sequence);
        return;
    }
    if(!journal(a, a->raw)) return;
    a->sequence = a->store.committed = ap->sequence;
    a->have_sequence = true;
    ++a->store.records;
    a->model.rows = a->store.records;
    if(!a->table_view) {
        if(a->model.ap_count == AP_LIMIT) {
            memmove(a->model.aps, a->model.aps + 1, sizeof(ApView) * (AP_LIMIT - 1));
            --a->model.ap_count;
        }
        ApView* view = &a->model.aps[a->model.ap_count++];
        fbr_ssid(f[7], view->ssid, sizeof(view->ssid));
        strcpy(view->bssid, ap->bssid);
        strcpy(view->auth, ap->auth);
        view->rssi = ap->rssi;
        view->channel = ap->channel;
        view->fix = ap->fix;
        view->boot = ap->boot;
        view->sequence = ap->sequence;
        view->session = ap->scan;
        view->aggregate = false;
    }
    if(strcmp(f[12], "-")) {
        if(!*a->model.utc_start) strcpy(a->model.utc_start, f[12]);
        strcpy(a->model.utc_stop, f[12]);
    }
    acknowledge(a, ap->sequence);
}
static void recover(App* a) {
    a->pending = CmdNone;
    a->failed = a->storage_failed = a->need_nack = false;
    a->startup = true;
    a->model.ready = false;
    a->state_known = a->stop_confirmed = false;
    a->finishing = a->config_wait = false;
    a->last_status = ms();
    a->baud_probe = 0;
    if(a->store.open && !store_close(&a->store)) {
        storage_fail(a);
        return;
    }
    if(*a->store.path) {
        quiet_io(a, true);
        bool recovered = store_recover(&a->store);
        quiet_io(a, false);
        if(!recovered) {
            storage_fail(a);
            return;
        }
        a->model.file_open = true;
        a->sequence = a->store.committed;
        a->have_sequence = true;
        a->model.rows = a->store.records;
        /* Establish live boot via HELLO before accepting retransmissions. */
        a->boot_known = false;
    }
    command(a, CmdHello);
    message(a, "HELLO 2 / checking device");
}
static void ready(App* a) {
    a->startup = a->closing = false;
    a->model.stopping = false;
    a->model.ready = true;
    message(a, a->model.storage_ready ? "Ready. OK: start" : "AP memory unavailable");
}
static void finish_export(App* a) {
    if(!local_record(a, "ARCHIVE_END", "OK")) return;
    if(!store_finish(&a->store, true)) {
        storage_fail(a);
        return;
    }
    a->store.interrupted = false;
    a->store.complete = true;
    if(!store_close(&a->store)) {
        storage_fail(a);
        return;
    }
    a->model.file_open = false;
    if(!export_store(a, &a->store)) {
        fail(a, "Export failed; journal retained");
        return;
    }
    a->finishing = false;
    a->archive_stage = 0;
    a->store.path[0] = 0; /* settings pointer clears only after successful export */
    if(!save_settings(a)) {
        storage_fail(a);
        return;
    }
    /* Recheck the live device after a potentially long SD export. */
    a->state_known = false;
    command(a, CmdState);
    message(a, "Session archived; checking IDLE");
}
static void state_received(App* a, char** f, bool response) {
    uint32_t boot;
    fbr_u32(f[3], &boot);
    if(!check_boot(a, boot)) return;
    fbr_u32(f[4], &a->session);
    a->model.session = a->session;
    a->model.running = !strcmp(f[5], "1");
    a->model.paused = !strcmp(f[6], "1");
    a->model.scanning = !strcmp(f[7], "1");
    a->model.link_wait = !strcmp(f[8], "1");
    fbr_u32(f[9], &a->model.queued);
    fbr_u32(f[10], &a->last_sequence);
    fbr_u32(f[11], &a->committed_remote);
    fbr_u64(f[12], &a->model.elapsed_ms);
    if(!copy_frame(a, a->state_frame, sizeof(a->state_frame))) return;
    a->state_known = true;
    if(a->startup && response) {
        if(a->store.open && !a->store.records && a->store.boot == a->boot &&
           a->store.session == a->session + 1) {
            store_finish(&a->store, false);
            store_close(&a->store);
            export_store(a, &a->store);
            a->model.file_open = false;
            a->store.path[0] = 0;
            save_settings(a);
        }
        if(a->store.open && a->store.session &&
           (a->store.boot != a->boot || a->store.session != a->session)) {
            a->store.complete = false;
            a->store.interrupted = true;
            store_finish(&a->store, false);
            store_close(&a->store);
            export_store(a, &a->store);
            a->model.file_open = false;
            a->store.path[0] = 0;
            save_settings(a);
            fail(a, "Session changed; old log interrupted");
            return;
        }
        if(a->store.open && a->store.session) {
            if(a->store.committed < a->committed_remote || a->store.committed > a->last_sequence) {
                fail(a, "Journal checkpoint mismatch");
                return;
            }
            a->session = a->store.session;
            command(a, CmdResumeLink);
        } else if(a->model.queued || a->model.running) {
            fail(a, "Device session has no SD journal");
        } else {
            command(a, CmdStop);
        }
    } else if(a->config_wait && response && !a->model.scanning && !a->model.queued &&
              a->model.idle) {
        a->config_wait = false;
        command(a, CmdSetConfig);
    } else if(a->closing && a->stop_confirmed && response) {
        command(a, CmdStatus);
    }
}
static void status_received(App* a, char** f) {
    uint32_t boot;
    fbr_u32(f[3], &boot);
    if(!a->boot_known || boot != a->boot) return;
    a->last_status = ms();
    a->model.connected = true;
    a->model.running = !strcmp(f[4], "1");
    a->model.idle = !strcmp(f[5], "IDLE");
    snprintf(a->model.phase, sizeof(a->model.phase), "%s", f[5]);
    fbr_u32(f[12], &a->model.truncated);
    fbr_u32(f[13], &a->model.discarded);
    a->model.fix = !strcmp(f[14], "1");
    fbr_u32(f[15], &a->model.satellites);
    fbr_u32(f[17], &a->model.gps_age);
    snprintf(a->model.hdop, sizeof(a->model.hdop), "%.15s", f[16]);
    a->model.wifi = !strcmp(f[23], "1");
    if(a->closing && a->stop_confirmed && a->state_known && !a->model.running &&
       !a->model.scanning && !a->model.queued && a->model.idle && a->pending == CmdNone &&
       !a->finishing) {
        if(a->store.open) {
            if(!journal(a, a->raw)) return;
            a->finishing = true;
            a->archive_stage = 1;
            command(a, CmdStats);
            message(a, "Drained; archiving AP table");
        } else
            ready(a);
    }
}
static void receive_line(App* a) {
    /* Preserve exact wire bytes (CRC included) before tokenization. */
    snprintf(a->raw, sizeof(a->raw), "%s", a->reader.data);
    char* f[FBR_FIELDS];
    size_t n = fbr_decode(a->reader.data, f);
    if(!n) return;
    uint32_t id;
    fbr_u32(f[1], &id);
    if(!strcmp(f[2], "OBSERVATION")) {
        receive_observation(a, f, n);
        return;
    }
    if(id && (id != a->pending_id || a->pending == CmdNone)) return;
    Command cmd = id ? a->pending : CmdNone;
    if(!strcmp(f[2], "ERR") && n == 4 && id) {
        a->pending = CmdNone;
        if(cmd == CmdMemory && !strcmp(f[3], "UNKNOWN_COMMAND") &&
           !strcmp(a->model.firmware, "0.2.0")) {
            a->model.memory_known = false;
            a->model.storage_ready = a->model.ap_capacity > 0 && a->model.queue_capacity > 0;
            command(a, CmdState);
        } else if(cmd == CmdSetConfig) {
            a->config_wait = false;
            /* Refresh authoritative configuration even after a rejected update. */
            command(a, CmdConfig);
            message(a, f[3]);
        } else
            fail(a, f[3]);
        return;
    }
    if(!strcmp(f[2], "OK") && id) {
        if(cmd == CmdHello) {
            uint32_t boot, value;
            if(n != 11 || strcmp(f[3], "HELLO") ||
               (strcmp(f[4], "0.3.0") && strcmp(f[4], "0.2.0")) || !fbr_u32(f[5], &boot) ||
               !fbr_u32(f[6], &value) || value != a->baud || strlen(f[7]) != 2 ||
               !fbr_u32(f[8], &value) || strcmp(f[9], "2000") || strcmp(f[10], "3")) {
                a->pending = CmdNone;
                fail(a, "Unsupported firmware / HELLO");
                return;
            }
            a->pending = CmdNone;
            if(!check_boot(a, boot)) return;
            snprintf(a->model.firmware, sizeof(a->model.firmware), "%s", f[4]);
            command(a, CmdInfo);
            return;
        }
        if(cmd == CmdAp && n == 4 && !strcmp(f[3], "EMPTY")) {
            a->pending = CmdNone;
            if(a->finishing)
                fail(a, "Incomplete AP table; journal kept");
            else
                a->model.network_loading = false;
            return;
        }
        const char* expected = cmd == CmdStart        ? "START"
                               : cmd == CmdStop       ? "STOP"
                               : cmd == CmdPing       ? "PONG"
                               : cmd == CmdResumeLink ? "RESUME_LINK"
                               : cmd == CmdNack       ? "NACK"
                               : cmd == CmdSetConfig  ? "SET_CONFIG"
                               : cmd == CmdPause      ? "PAUSE_SCAN"
                               : cmd == CmdResume     ? "RESUME_SCAN"
                                                      : "";
        if(n != 4 || strcmp(f[3], expected)) return;
        a->pending = CmdNone;
        if(cmd == CmdStop) {
            a->stop_confirmed = true;
            a->closing = true;
            a->model.stopping = true;
            a->startup = false;
            command(a, CmdState);
        } else if(cmd == CmdResumeLink) {
            if(a->resume_requested && a->model.running && !a->store.complete)
                command(a, CmdResume);
            else
                command(a, CmdStop);
        } else if(cmd == CmdStart) {
            a->start_uncertain = false;
            a->model.running = true;
            message(a, "Recording to SD journal");
        } else if(cmd == CmdPause) {
            a->model.paused = true;
            command(a, CmdState);
        } else if(cmd == CmdResume) {
            if(a->startup && a->resume_requested) {
                a->startup = a->closing = a->model.stopping = false;
                a->model.ready = true;
                command(a, CmdStatus);
            }
            a->resume_requested = false;
            a->model.paused = false;
            a->resume_after_config = false;
            message(a, "Scanning resumed");
        } else if(cmd == CmdSetConfig) {
            uint32_t baud = a->config_edit[12];
            if(baud != a->baud) {
                set_baud(a, baud);
                if(!save_settings(a)) {
                    storage_fail(a);
                    return;
                }
                a->startup = true;
                command(a, CmdHello);
            } else
                command(a, CmdConfig);
        } else if(cmd == CmdNack)
            a->need_nack = false;
        return;
    }
    if(!strcmp(f[2], "SESSION_STARTED") && !id && n == 5) {
        uint32_t boot, session;
        if(!fbr_u32(f[3], &boot) || !fbr_u32(f[4], &session) || !session || boot != a->boot ||
           !a->store.open || (a->pending != CmdStart && !a->start_uncertain))
            return;
        a->session = a->store.session = a->model.session = session;
        if(!journal(a, a->raw)) return;
        a->model.rows = a->store.records = 0;
        a->model.ap_count = 0;
        a->table_view = a->model.network_loading = false;
        a->model.utc_start[0] = a->model.utc_stop[0] = 0;
        a->model.running = true;
        return;
    }
    if(!strcmp(f[2], "READY") && !id && n == 5) {
        uint32_t boot;
        if(fbr_u32(f[4], &boot) && a->boot_known) check_boot(a, boot);
        return;
    }
    if(!strcmp(f[2], "STOPPED") && !id && n == 6) {
        uint32_t boot, discarded;
        if(!fbr_u32(f[3], &boot) || boot != a->boot || !fbr_u32(f[5], &discarded)) return;
        a->model.running = false;
        a->model.stopping = a->closing = true;
        a->model.discarded = discarded;
        if(a->store.open && !journal(a, a->raw)) return;
        /* STOPPED is not the end of revision 2 queue delivery. */
        if(strcmp(f[4], "USER")) message(a, f[4]);
        return;
    }
    if(!strcmp(f[2], "ERROR") && !id && (n == 4 || n == 5)) {
        if(!strcmp(f[3], "AP_TABLE_FULL")) {
            a->closing = a->model.stopping = true;
            message(a, "AP table full; archiving session");
            if(a->pending == CmdNone) command(a, CmdStop);
        } else if(!strcmp(f[3], "ACK_TIMEOUT")) {
            fail(a, "ACK timeout; Retry to recover journal");
        } else if(!strcmp(f[3], "AP_MEMORY_UNAVAILABLE")) {
            a->model.storage_ready = false;
            message(a, f[3]);
        } else
            message(a, f[3]);
        return;
    }
    if(!fbr_validate(f, n)) return;
    if(!strcmp(f[2], "STATUS")) {
        if(id && cmd != CmdStatus) return;
        if(id) a->pending = CmdNone;
        status_received(a, f);
    } else if(!strcmp(f[2], "INFO") && cmd == CmdInfo) {
        uint32_t boot;
        fbr_u32(f[4], &boot);
        if(!check_boot(a, boot)) return;
        if(strcmp(f[3], a->model.firmware)) return;
        fbr_u32(f[7], &a->model.ap_capacity);
        fbr_u32(f[8], &a->model.queue_capacity);
        if(!copy_frame(a, a->info_frame, sizeof(a->info_frame))) return;
        a->pending = CmdNone;
        command(a, CmdConfig);
    } else if(!strcmp(f[2], "CONFIG") && cmd == CmdConfig) {
        for(unsigned i = 0; i < 13; ++i) fbr_u32(f[i + 3], &a->model.config[i]);
        a->model.band = a->model.config[2];
        if(!copy_frame(a, a->config_frame, sizeof(a->config_frame))) return;
        a->pending = CmdNone;
        if(a->startup)
            command(a, CmdMemory);
        else {
            if(a->store.open && !journal(a, a->raw)) return;
            if(a->resume_after_config)
                command(a, CmdResume);
            else
                message(a, "Configuration read from ESP");
        }
    } else if(!strcmp(f[2], "MEMORY") && cmd == CmdMemory) {
        for(unsigned i = 0; i < 15; ++i) fbr_u32(f[i + 3], &a->model.memory[i]);
        a->model.memory_known = true;
        a->model.storage_ready = a->model.memory[0] && a->model.memory[1] &&
                                 a->model.memory[11] == a->model.ap_capacity &&
                                 a->model.memory[13] == a->model.queue_capacity &&
                                 a->model.ap_capacity >= 64 && a->model.queue_capacity >= 32;
        a->pending = CmdNone;
        if(a->startup) command(a, CmdState);
    } else if(!strcmp(f[2], "STATE") && cmd == CmdState) {
        a->pending = CmdNone;
        state_received(a, f, true);
    } else if(!strcmp(f[2], "GPS") && cmd == CmdGps) {
        a->pending = CmdNone;
        for(unsigned i = 0; i < 11; ++i)
            snprintf(a->model.gps[i], sizeof(a->model.gps[i]), "%.31s", f[i + 3]);
    } else if(!strcmp(f[2], "STATS")) {
        if(strcmp(f[3], "0")) return;
        uint32_t session;
        fbr_u32(f[4], &session);
        if(session != a->session) return;
        for(unsigned i = 0; i < 21; ++i) fbr_u64(f[i + 5], &a->model.stats[i]);
        a->model.observations = a->model.stats[0];
        a->model.unique = a->model.stats[1];
        a->model.filtered = a->model.stats[3];
        a->model.no_gps = a->model.stats[5];
        if(a->model.stats[17] <= a->model.queue_capacity) a->model.queued = a->model.stats[17];
        if(cmd == CmdStats) {
            a->pending = CmdNone;
            if(a->finishing) {
                if(!journal(a, a->raw)) return;
                if(a->archive_stage == 1) {
                    if(a->model.unique > a->model.ap_capacity) {
                        fail(a, "Invalid AP count");
                        return;
                    }
                    a->archive_count = a->model.unique;
                    a->archive_slot = 0;
                    char count[16];
                    snprintf(count, sizeof(count), "%u", a->archive_count);
                    if(!local_record(a, "ARCHIVE_BEGIN", count)) return;
                    a->archive_stage = 2;
                    if(a->archive_count)
                        command(a, CmdAp);
                    else {
                        a->archive_stage = 3;
                        command(a, CmdStats);
                    }
                } else
                    finish_export(a);
            }
        }
    } else if((!strcmp(f[2], "AP_DISCOVERED") || !strcmp(f[2], "AP_UPDATED")) &&
              (cmd == CmdAp || cmd == CmdApDetail)) {
        uint32_t boot, session, slot;
        fbr_u32(f[3], &boot);
        fbr_u32(f[4], &session);
        fbr_u32(f[5], &slot);
        uint32_t expected = cmd == CmdApDetail ? a->detail_slot
                            : a->finishing     ? a->archive_slot
                                               : a->network_next;
        if(boot != a->boot || session != a->session || slot != expected) return;
        a->pending = CmdNone;
        if(cmd == CmdApDetail) {
            char ssid[129];
            fbr_ssid(f[7], ssid, sizeof(ssid));
            snprintf(a->model.network_detail, sizeof(a->model.network_detail),
                     "%s\n%s\nSSID hex: %s\nBand: %s GHz / ch %s\nFrequency: %s MHz\nPHY mask: "
                     "%s\nAuth: %s\n\n"
                     "RSSI last/strong/mean\n%s / %s / %s dBm\nDetections: %s\nObservations: "
                     "%s\nChanges: %s\n"
                     "First ms: %s\nLast ms: %s\nFirst UTC: %s\nLast UTC: %s\n\n"
                     "Last fix: %s\nLat: %s / Lon: %s\nStrongest fix: %s\nLat: %s / Lon: %s\n\n"
                     "Points sampled at scan end.\nNot the router location.",
                     *ssid ? ssid : "Hidden network", f[6], f[7], f[8], f[9], f[27], f[28], f[10],
                     f[11], f[12], f[13], f[14], f[15], f[18], f[16], f[17], f[25], f[26], f[19],
                     !strcmp(f[19], "1") ? f[20] : "-", !strcmp(f[19], "1") ? f[21] : "-", f[22],
                     !strcmp(f[22], "1") ? f[23] : "-", !strcmp(f[22], "1") ? f[24] : "-");
        } else if(a->finishing) {
            if(!journal(a, a->raw)) return;
            snprintf(a->model.message, sizeof(a->model.message), "Archiving AP %u / %u",
                     a->archive_slot + 1, a->archive_count);
            if(++a->archive_slot < a->archive_count)
                command(a, CmdAp);
            else {
                a->archive_stage = 3;
                command(a, CmdStats);
            }
        } else if(a->model.network_loading && a->model.ap_count < AP_LIMIT) {
            ApView* view = &a->model.aps[a->model.ap_count++];
            fbr_ssid(f[7], view->ssid, sizeof(view->ssid));
            strcpy(view->bssid, f[6]);
            strcpy(view->auth, f[10]);
            view->rssi = atoi(f[11]);
            view->channel = atoi(f[9]);
            view->fix = !strcmp(f[19], "1");
            view->slot = slot;
            view->aggregate = true;
            view->boot = boot;
            view->session = session;
            if(++a->network_next < a->network_end)
                command(a, CmdAp);
            else
                a->model.network_loading = false;
        }
    }
}
static void service(App* a) {
    if(!a->serial) return;
    if(a->pending != CmdNone && elapsed(a->sent_at, TIMEOUT_MS)) {
        Command timed = a->pending;
        a->pending = CmdNone;
        if(timed == CmdHello && a->startup && a->baud_probe < 2) {
            unsigned index = 0;
            while(index < COUNT_OF(bauds) && bauds[index] != a->baud) ++index;
            set_baud(a, bauds[(index + 1) % COUNT_OF(bauds)]);
            ++a->baud_probe;
            command(a, CmdHello);
        } else if(timed == CmdStart || timed == CmdStop || timed == CmdSetConfig) {
            /* No blind retry of commands without response caching. Query state first. */
            a->start_uncertain = timed == CmdStart;
            a->closing = a->model.stopping = true;
            a->stop_confirmed = false;
            command(a, CmdState);
            message(a, "Command timeout; checking state");
        } else
            fail(a, "No response; Retry to recover");
    }
    if(!a->failed && !a->startup && elapsed(a->last_status, 5000)) {
        fail(a, "Link unknown; Retry to recover");
        a->model.connected = false;
        /* Freeze writes until journal recovery; ESP retains unacknowledged RAM. */
        store_close(&a->store);
        a->model.file_open = false;
        a->storage_failed = true;
        if(a->pending == CmdNone) command(a, CmdPing);
    }
    if(a->need_nack && a->pending == CmdNone)
        command(a, CmdNack);
    else if(a->quit && !a->quit_stop_sent && a->pending == CmdNone) {
        a->quit_stop_sent = true;
        command(a, CmdStop);
    } else if(!a->failed && a->pending == CmdNone && elapsed(a->last_poll, 1000)) {
        if(a->start_uncertain || (a->closing && !a->stop_confirmed)) {
            /* State has been inspected; a deliberate STOP safely resolves uncertainty. */
            command(a, a->state_known ? CmdStop : CmdState);
        } else if(a->closing || a->config_wait || a->model.running)
            command(a, CmdState);
        a->last_poll = ms();
    }
}
#ifndef FBR_HOST_TEST
static void uart_rx(FuriHalSerialHandle* serial, FuriHalSerialRxEvent event, void* context) {
    App* a = context;
    if(event & FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(serial);
        if(a->quiet_rx) return;
        if(furi_stream_buffer_send(a->rx, &byte, 1, 0) != 1)
            furi_thread_flags_set(furi_thread_get_id(a->thread), RX_ERROR);
    }
    if(event & (FuriHalSerialRxEventFrameError | FuriHalSerialRxEventNoiseError |
                FuriHalSerialRxEventOverrunError | FuriHalSerialRxEventParityError))
        furi_thread_flags_set(furi_thread_get_id(a->thread), RX_ERROR);
}
#endif
static void list_sessions(App* a, unsigned direction) {
    char boundary[64] = {0};
    if(a->session_count && direction)
        strcpy(boundary, a->sessions[direction == 1 ? a->session_count - 1 : 0]);
    File* dir = storage_file_alloc(a->storage);
    a->session_count = 0;
    if(storage_dir_open(dir, SESSION_DIR)) {
        FileInfo info;
        char name[64];
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(!(info.flags & FSF_DIRECTORY) || strncmp(name, "session_", 8)) continue;
            if(*boundary &&
               (direction == 1 ? strcmp(name, boundary) >= 0 : strcmp(name, boundary) <= 0))
                continue;
            /* Keep the newest bounded page sorted descending. */
            unsigned pos = 0;
            while(pos < a->session_count && (direction == 2 ? strcmp(a->sessions[pos], name) < 0
                                                            : strcmp(a->sessions[pos], name) > 0))
                ++pos;
            if(pos >= SESSION_LIMIT) continue;
            unsigned end =
                a->session_count < SESSION_LIMIT ? a->session_count++ : SESSION_LIMIT - 1;
            while(end > pos) {
                strcpy(a->sessions[end], a->sessions[end - 1]);
                --end;
            }
            snprintf(a->sessions[pos], sizeof(a->sessions[pos]), "%s", name);
        }
        storage_dir_close(dir);
    }
    storage_file_free(dir);
    if(direction == 2) {
        for(unsigned i = 0; i < a->session_count / 2; ++i) {
            char name[64];
            strcpy(name, a->sessions[i]);
            strcpy(a->sessions[i], a->sessions[a->session_count - i - 1]);
            strcpy(a->sessions[a->session_count - i - 1], name);
        }
    }
}
static void handle_action(App* a, ActionRequest request) {
    Action action = request.type;
    if(action == ActionQuit) {
        a->quit = true;
        a->quit_at = ms();
        a->quit_stop_sent = false;
        a->closing = a->model.stopping = true;
        a->stop_confirmed = false;
        return;
    }
    if(action == ActionConnect || action == ActionRecoverScan) {
        a->resume_requested = action == ActionRecoverScan;
        if(a->resume_requested && !*a->store.path) {
            message(a, "No journal to resume; choose Connect");
            return;
        }
        if(!a->serial) {
#ifndef FBR_HOST_TEST
            a->serial = furi_hal_serial_control_acquire(a->port);
            if(!a->serial) {
                message(a, "UART busy. Try another port");
                return;
            }
            furi_hal_serial_init(a->serial, a->baud);
            furi_hal_serial_configure_framing(a->serial, FuriHalSerialDataBits8,
                                              FuriHalSerialParityNone, FuriHalSerialStopBits1);
            furi_hal_serial_async_rx_start(a->serial, uart_rx, a, true);
#else
            a->serial = (void*)1;
#endif
        }
        recover(a);
        return;
    }
    if(action == ActionSessions) {
        if(a->store.open) {
            message(a, "Stop before browsing archived sessions");
            return;
        }
        if(a->pending != CmdNone) {
            message(a, "Wait for device response");
            return;
        }
        quiet_io(a, true);
        list_sessions(a, request.value);
        quiet_io(a, false);
        return;
    }
    if((action == ActionExport || action == ActionSelectSession) && a->pending != CmdNone) {
        message(a, "Wait for device response");
        return;
    }
    if((action == ActionExport || action == ActionSelectSession) && a->store.open) {
        message(a, "Stop before opening archived session");
        return;
    }
    if(action == ActionSelectSession && !a->store.open && request.value < a->session_count) {
        snprintf(a->model.path, sizeof(a->model.path), SESSION_DIR "/%s",
                 a->sessions[request.value]);
        SessionStore* s = calloc(1, sizeof(SessionStore));
        StoreReader* reader = calloc(1, sizeof(StoreReader));
        StoreSummary* summary = calloc(1, sizeof(StoreSummary));
        bool ok = s && reader && summary;
        quiet_io(a, true);
        if(ok) {
            s->storage = a->storage;
            s->file = storage_file_alloc(a->storage);
            strcpy(s->path, a->model.path);
            ok = store_recover(s) && storage_file_seek(s->file, 0, true) &&
                 store_summary(s->file, reader, summary);
            if(ok) {
                snprintf(a->model.session_summary, sizeof(a->model.session_summary),
                         "Boot: %lu / session: %lu\n%s\nAP aggregates: %s\nSD records: "
                         "%llu\nCommitted: %lu\n"
                         "Unique: %llu\nDetections: %llu\nFiltered: %llu\nNo GPS: %llu\nTruncated: "
                         "%lu\nDiscarded: %lu\n"
                         "Queue overflows: %llu\nTable overflows: %llu\nFirst UTC: %s\nLast UTC: "
                         "%s\nEnd: %s",
                         (unsigned long)s->boot, (unsigned long)s->session,
                         s->interrupted ? "Interrupted" : "Complete",
                         s->complete ? "complete" : "incomplete", (unsigned long long)s->records,
                         (unsigned long)s->committed, (unsigned long long)summary->stats[1],
                         (unsigned long long)summary->stats[0],
                         (unsigned long long)summary->stats[3],
                         (unsigned long long)summary->stats[5], (unsigned long)summary->truncated,
                         (unsigned long)summary->discarded, (unsigned long long)summary->stats[15],
                         (unsigned long long)summary->stats[16], summary->first_utc,
                         summary->last_utc, *summary->reason ? summary->reason : "interrupted");
            }
            if(!store_close(s)) ok = false;
            storage_file_free(s->file);
        }
        quiet_io(a, false);
        free(s);
        free(reader);
        free(summary);
        if(!ok)
            snprintf(a->model.session_summary, sizeof(a->model.session_summary),
                     "Cannot read session journal");
        message(a, ok ? "Selected session; Export in menu" : "Session SD error");
        return;
    }
    if(action == ActionExport && !a->store.open && *a->model.path) {
        SessionStore* s = calloc(1, sizeof(SessionStore));
        if(!s) return;
        s->storage = a->storage;
        s->file = storage_file_alloc(a->storage);
        snprintf(s->path, sizeof(s->path), "%s", a->model.path);
        quiet_io(a, true);
        bool ok = store_recover(s) && store_close(s) && store_export(s);
        quiet_io(a, false);
        storage_file_free(s->file);
        free(s);
        message(a, ok ? "CSV export complete" : "Export failed; journal retained");
        return;
    }
    if(action == ActionInterval && request.value < COUNT_OF(intervals) && !a->model.running &&
       !a->store.open) {
        a->model.interval = request.value;
        if(!save_settings(a)) message(a, "Settings SD error");
        return;
    }
    if(!a->serial || a->pending != CmdNone || !a->model.ready || a->failed) return;
    if(action == ActionNetworks) {
        if(a->model.unique > a->model.ap_capacity) return;
        uint64_t first = (uint64_t)request.value * AP_LIMIT;
        if(first >= a->model.unique && first) return;
        a->table_view = true;
        a->model.ap_count = 0;
        a->model.network_page = request.value;
        a->network_next = first;
        a->network_end = first + AP_LIMIT < a->model.unique ? first + AP_LIMIT : a->model.unique;
        a->model.network_loading = a->network_next < a->network_end;
        if(a->model.network_loading) command(a, CmdAp);
        return;
    }
    if(action == ActionNetworkDetail) {
        if(request.value >= a->model.unique) return;
        a->detail_slot = request.value;
        a->model.network_detail[0] = 0;
        command(a, CmdApDetail);
        return;
    }
    if(action == ActionToggle) {
        if(a->model.running || a->store.open) {
            a->closing = a->model.stopping = true;
            a->stop_confirmed = false;
            command(a, CmdStop);
        } else if(!a->closing && a->model.idle && a->model.wifi && a->model.storage_ready) {
            /* Refresh STATE before START to persist the global sequence baseline. */
            if(open_file(a)) {
                a->start_uncertain = true;
                a->stop_confirmed = false;
                command(a, CmdStart);
            }
        }
    } else if(action == ActionGps)
        command(a, CmdGps);
    else if(action == ActionDiagnostics)
        command(a, CmdMemory);
    else if(action == ActionPause && a->model.running)
        command(a, a->model.paused ? CmdResume : CmdPause);
    else if(action == ActionResume && a->model.paused && a->store.open)
        command(a, CmdResume);
    else if(action == ActionBand || action == ActionConfig) {
        memcpy(a->config_edit, a->model.config, sizeof(a->config_edit));
        unsigned field = action == ActionBand ? 2 : request.value >> 24;
        uint32_t value = action == ActionBand ? request.value : request.value & 0xffffff;
        if(field == 12) value = value < COUNT_OF(bauds) ? bauds[value] : 0;
        if(field >= 13 || field == 0) return;
        a->config_edit[field] = value;
        if(!fbr_config(a->config_edit)) {
            message(a, "Invalid configuration range");
            return;
        }
        if(a->model.running || a->store.open) {
            if(field == 12) {
                message(a, "Stop before changing baud");
                return;
            }
            a->resume_after_config = !a->model.paused;
            a->config_wait = true;
            command(a, CmdPause);
        } else
            command(a, CmdSetConfig);
    }
}
#ifndef FBR_HOST_TEST
static int32_t worker(void* context) {
    App* a = context;
    uint8_t bytes[128];
    for(;;) {
        size_t count =
            furi_stream_buffer_receive(a->rx, bytes, sizeof(bytes), furi_ms_to_ticks(25));
        furi_mutex_acquire(a->mutex, FuriWaitForever);
        uint32_t flags = furi_thread_flags_get();
        if(flags & RX_ERROR) {
            furi_thread_flags_clear(RX_ERROR);
            a->reader.dropping = true;
            if(a->serial) {
                a->storage_failed = true;
                if(!store_close(&a->store))
                    storage_fail(a);
                else
                    fail(a, "UART error; Retry recovers journal");
                a->model.file_open = false;
            }
        }
        for(size_t i = 0; i < count; ++i)
            if(fbr_feed(&a->reader, bytes[i])) receive_line(a);
        ActionRequest request;
        while(furi_message_queue_get(a->actions, &request, 0) == FuriStatusOk)
            handle_action(a, request);
        service(a);
        bool done =
            a->quit && (!a->serial || (!a->store.open && !a->model.running && a->model.idle) ||
                        elapsed(a->quit_at, 3500));
        furi_mutex_release(a->mutex);
        if(done) break;
    }
    if(a->serial) {
        furi_hal_serial_async_rx_stop(a->serial);
        furi_hal_serial_tx_wait_complete(a->serial);
        furi_hal_serial_deinit(a->serial);
        furi_hal_serial_control_release(a->serial);
    }
    furi_mutex_acquire(a->mutex, FuriWaitForever);
    if(a->store.open) {
        a->store.interrupted = true;
        store_finish(&a->store, false);
        if(!store_close(&a->store)) message(a, "SD close error; recover journal");
        a->model.file_open = false;
    }
    furi_mutex_release(a->mutex);
    return 0;
}
#include "ui.inc"
#endif
