#pragma once
/* Included after platform/storage headers. No table of APs is kept in RAM. */
#define DATA_DIR EXT_PATH("apps_data/wardriving")
#define SESSION_DIR DATA_DIR "/sessions"
#define CONFIG_DIR DATA_DIR "/config"
#define LOG_DIR DATA_DIR "/logs"
#define STORE_PATH 160

typedef struct {
    Storage* storage;
    File* file;
    char path[STORE_PATH];
    uint32_t boot, session, committed;
    uint64_t records;
    bool open, interrupted, complete, damaged;
} SessionStore;
static const char wigle_header[] =
    "WigleWifi-1.4,appRelease=0.3.0,model=Feberis "
    "Pro,release=0.3.0,device=ESP32-C5,display=Flipper Zero,board=Feberis Pro,brand=Feberis\n"
    "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,"
    "AccuracyMeters,Type\n";
static const char observation_header[] =
    "boot,session,sequence,bssid,ssid_hex,utc,monotonic_ms,rssi,channel,band,latitude,longitude,"
    "altitude_m,hdop,satellites,gps_valid,reason_mask,frequency_mhz,speed_kmh,course_deg,"
    "horizontal_accuracy_m,phy_mask\n";
static const char aggregate_header[] =
    "boot,session,bssid,ssid_hex,band,channel,auth,detection_count,observation_count,strongest_"
    "rssi,mean_rssi,last_rssi,first_seen_ms,last_seen_ms,best_fix_valid,best_lat,best_lon,last_fix_"
    "valid,last_lat,last_lon,metadata_change_count,first_utc,last_utc,frequency_mhz,phy_mask\n";
static const char* const stat_names[] = {"total_detections",
                                         "unique_aps",
                                         "observations_saved",
                                         "observations_filtered",
                                         "gps_valid_observations",
                                         "gps_invalid_observations",
                                         "wifi_2g_aps",
                                         "wifi_5g_aps",
                                         "open_aps",
                                         "secured_aps",
                                         "scan_cycles",
                                         "scan_errors",
                                         "uart_retransmissions",
                                         "uart_dropped_frames",
                                         "storage_errors",
                                         "queue_overflows",
                                         "table_overflows",
                                         "queued",
                                         "committed",
                                         "free_heap",
                                         "minimum_free_heap"};
static bool store_dirs(Storage* storage) {
    const char* dirs[] = {DATA_DIR, CONFIG_DIR, SESSION_DIR, LOG_DIR};
    for(unsigned i = 0; i < COUNT_OF(dirs); ++i)
        if(!storage_simply_mkdir(storage, dirs[i])) return false;
    return true;
}
static bool store_write(File* file, const void* data, size_t size) {
    return storage_file_write(file, data, size) == size;
}
static bool store_text(File* file, const char* text) {
    return store_write(file, text, strlen(text));
}
static void store_log(Storage* storage, const char* message) {
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, LOG_DIR "/errors.log", FSAM_WRITE, FSOM_OPEN_APPEND)) {
        /* Keep diagnostic growth bounded. Existing sessions are never removed. */
        if(storage_file_size(file) < 65536) {
            bool ok =
                store_text(file, message) && store_text(file, "\n") && storage_file_sync(file);
            (void)ok;
        }
        storage_file_close(file);
    }
    storage_file_free(file);
}
static bool store_close(SessionStore* s) {
    if(!s->open) return true;
    bool ok = storage_file_sync(s->file);
    if(!storage_file_close(s->file)) ok = false;
    s->open = false;
    return ok;
}
static bool store_append(SessionStore* s, const char* frame) {
    if(!s->open || s->damaged) return false;
    uint64_t offset = storage_file_tell(s->file);
    if(offset > UINT32_MAX - FBR_LINE_SIZE) return false;
    if(store_text(s->file, frame) && store_text(s->file, "\n") && storage_file_sync(s->file))
        return true;
    /* A failed sync never advances committed, even if all bytes were written. */
    if(offset > UINT32_MAX || !storage_file_seek(s->file, offset, true) ||
       !storage_file_truncate(s->file) || !storage_file_sync(s->file))
        s->damaged = true;
    return false;
}
static bool store_local(SessionStore* s, const char* type, const char* detail) {
    char payload[192], frame[208];
    snprintf(payload, sizeof(payload), "FBR1\t0\t%s\t%lu\t%lu\t%s", type, (unsigned long)s->boot,
             (unsigned long)s->session, detail);
    size_t n = fbr_encode(payload, frame, sizeof(frame));
    if(!n) return false;
    frame[n - 1] = 0;
    return store_append(s, frame);
}
static bool store_finish(SessionStore* s, bool complete) {
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    char local[40];
    snprintf(local, sizeof(local), "%04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day,
             now.hour, now.minute, now.second);
    return store_local(s, "FINISH_LOCAL", local) &&
           store_local(s, "FINISH", complete ? "COMPLETE" : "INTERRUPTED");
}
/* Buffered SD traversal: bounded memory, no per-byte Storage RPC. */
typedef struct {
    FbrReader line;
    uint8_t block[256];
    size_t used, index;
    uint64_t offset, end;
} StoreReader;
static int store_next(File* file, StoreReader* r) {
    while(true) {
        if(r->index == r->used) {
            r->used = storage_file_read(file, r->block, sizeof(r->block));
            r->index = 0;
            if(!r->used) return storage_file_get_error(file) == FSE_OK ? 0 : -1;
        }
        char byte = r->block[r->index++];
        ++r->offset;
        bool ready = fbr_feed(&r->line, byte);
        if(byte == '\n') {
            r->end = r->offset;
            return ready ? 1 : -1;
        }
    }
}
static bool store_frame_valid(char** f, size_t n) {
    if(fbr_validate(f, n)) return true;
    uint32_t boot, session, value;
    if(n < 5 || strcmp(f[1], "0") || !fbr_u32(f[3], &boot)) return false;
    if(!strcmp(f[2], "STOPPED")) {
        if(n != 6 || strlen(f[4]) > 40 || !fbr_u32(f[5], &value)) return false;
        for(const char* p = f[4]; *p; ++p)
            if((*p < 'A' || *p > 'Z') && *p != '_') return false;
        return *f[4] != 0;
    }
    if(!fbr_u32(f[4], &session)) return false;
    if(!strcmp(f[2], "SESSION_STARTED")) return n == 5 && session > 0;
    if(!strcmp(f[2], "FINISH_LOCAL")) return n == 6 && fbr_utc(f[5]) && strcmp(f[5], "-");
    if(!strcmp(f[2], "START_INTENT")) return n == 6 && session > 0 && !strcmp(f[5], "START");
    if(!strcmp(f[2], "ARCHIVE_BEGIN")) return n == 6 && fbr_u32(f[5], &value) && value <= 12000;
    if(!strcmp(f[2], "ARCHIVE_END")) return n == 6 && !strcmp(f[5], "OK");
    if(!strcmp(f[2], "FINISH"))
        return n == 6 && (!strcmp(f[5], "COMPLETE") || !strcmp(f[5], "INTERRUPTED"));
    return false;
}
static bool store_recover(SessionStore* s) {
    char path[STORE_PATH + 32];
    snprintf(path, sizeof(path), "%s/events.journal", s->path);
    if(!storage_file_open(s->file, path, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) return false;
    s->open = true;
    s->boot = s->session = s->committed = 0;
    s->records = 0;
    s->complete = s->damaged = false;
    s->interrupted = true;
    StoreReader* r = calloc(1, sizeof(StoreReader));
    if(!r) {
        store_close(s);
        return false;
    }
    uint64_t good = 0;
    int result;
    uint32_t expected = 0, received = 0;
    bool archive_ready = false;
    char* f[FBR_FIELDS];
    while((result = store_next(s->file, r)) == 1) {
        size_t n = fbr_decode(r->line.data, f);
        if(!n || !store_frame_valid(f, n)) break;
        if(!strcmp(f[2], "STATE")) {
            uint32_t boot, session, committed;
            fbr_u32(f[3], &boot);
            fbr_u32(f[4], &session);
            fbr_u32(f[11], &committed);
            if(s->records && (boot != s->boot || session != s->session)) break;
            s->boot = boot;
            s->session = session;
            if(!s->records) s->committed = committed;
        } else if((!strcmp(f[2], "SESSION_STARTED") || !strcmp(f[2], "START_INTENT"))) {
            if(s->records) break;
            fbr_u32(f[3], &s->boot);
            fbr_u32(f[4], &s->session);
        } else if(!strcmp(f[2], "OBSERVATION")) {
            FbrAp ap;
            if(!fbr_observation(f, n, &ap) || ap.boot != s->boot || ap.scan != s->session ||
               ap.sequence <= s->committed)
                break;
            s->committed = ap.sequence;
            ++s->records;
        } else if(!strcmp(f[2], "ARCHIVE_BEGIN")) {
            fbr_u32(f[5], &expected);
            received = 0;
            archive_ready = s->complete = false;
        } else if(!strcmp(f[2], "AP_DISCOVERED") || !strcmp(f[2], "AP_UPDATED")) {
            uint32_t boot, session, slot;
            fbr_u32(f[3], &boot);
            fbr_u32(f[4], &session);
            fbr_u32(f[5], &slot);
            if(boot != s->boot || session != s->session || slot != received || received >= expected)
                break;
            ++received;
        } else if(!strcmp(f[2], "ARCHIVE_END")) {
            if(received != expected) break;
            archive_ready = true;
        } else if(!strcmp(f[2], "FINISH")) {
            s->complete = !strcmp(f[5], "COMPLETE") && archive_ready;
            s->interrupted = !s->complete;
        }
        good = r->end;
    }
    bool ok = storage_file_get_error(s->file) == FSE_OK && good <= UINT32_MAX;
    if(ok) {
        ok = storage_file_seek(s->file, good, true);
        if(ok && good != storage_file_size(s->file))
            ok = storage_file_truncate(s->file) && storage_file_sync(s->file);
    }
    free(r);
    if(!ok) {
        s->damaged = true;
        store_close(s);
    }
    return ok;
}
static bool store_create(SessionStore* s) {
    if(!store_dirs(s->storage)) return false;
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    for(unsigned i = 0; i < 10000; ++i) {
        snprintf(s->path, sizeof(s->path), SESSION_DIR "/session_%04u%02u%02u_%02u%02u%02u_%04u",
                 now.year, now.month, now.day, now.hour, now.minute, now.second, i);
        FS_Error error = storage_common_mkdir(s->storage, s->path);
        if(error == FSE_EXIST) continue;
        if(error != FSE_OK) return false;
        char path[STORE_PATH + 32];
        snprintf(path, sizeof(path), "%s/events.journal", s->path);
        if(!storage_file_open(s->file, path, FSAM_READ_WRITE, FSOM_CREATE_NEW)) return false;
        s->open = true;
        s->records = 0;
        s->complete = s->damaged = false;
        s->interrupted = true;
        return storage_file_sync(s->file);
    }
    return false;
}
typedef struct {
    char firmware[16], first_utc[20], last_utc[20], finished_local[20], reason[48];
    uint32_t config[13], ap_capacity, queue_capacity, truncated, discarded;
    uint64_t stats[21], archive_start;
    bool stats_known, config_known;
} StoreSummary;
static bool store_json_string(File* f, const char* text) {
    if(!store_text(f, "\"")) return false;
    char escaped[256];
    size_t used = 0;
    for(const unsigned char* p = (const unsigned char*)text; *p; ++p) {
        if(used + 7 >= sizeof(escaped)) {
            if(!store_write(f, escaped, used)) return false;
            used = 0;
        }
        if(*p == '"' || *p == '\\') {
            escaped[used++] = '\\';
            escaped[used++] = *p;
        } else if(*p < 32)
            used += snprintf(escaped + used, sizeof(escaped) - used, "\\u%04x", *p);
        else
            escaped[used++] = *p;
    }
    return store_write(f, escaped, used) && store_text(f, "\"");
}
static bool store_summary(File* input, StoreReader* r, StoreSummary* m) {
    int result;
    char* f[FBR_FIELDS];
    while((result = store_next(input, r)) == 1) {
        size_t n = fbr_decode(r->line.data, f);
        if(!n || !store_frame_valid(f, n)) return false;
        if(!strcmp(f[2], "INFO")) {
            snprintf(m->firmware, sizeof(m->firmware), "%.15s", f[3]);
            fbr_u32(f[7], &m->ap_capacity);
            fbr_u32(f[8], &m->queue_capacity);
        } else if(!strcmp(f[2], "CONFIG")) {
            for(unsigned i = 0; i < 13; ++i) fbr_u32(f[3 + i], &m->config[i]);
            m->config_known = true;
        } else if(!strcmp(f[2], "STATS") && !strcmp(f[3], "0")) {
            for(unsigned i = 0; i < 21; ++i) fbr_u64(f[5 + i], &m->stats[i]);
            m->stats_known = true;
        } else if(!strcmp(f[2], "OBSERVATION") && strcmp(f[12], "-")) {
            if(!*m->first_utc) strcpy(m->first_utc, f[12]);
            strcpy(m->last_utc, f[12]);
        } else if(!strcmp(f[2], "ARCHIVE_BEGIN"))
            m->archive_start = r->end;
        else if(!strcmp(f[2], "FINISH_LOCAL"))
            strcpy(m->finished_local, f[5]);
        else if(!strcmp(f[2], "STOPPED")) {
            if(!*m->reason || !strcmp(m->reason, "USER") || strcmp(f[4], "USER"))
                snprintf(m->reason, sizeof(m->reason), "%.40s", f[4]);
            fbr_u32(f[5], &m->discarded);
        } else if(!strcmp(f[2], "STATUS")) {
            fbr_u32(f[12], &m->truncated);
            fbr_u32(f[13], &m->discarded);
        }
    }
    return result == 0 && !r->line.used && !r->line.dropping;
}
static bool store_json_stats(File* output, const StoreSummary* m) {
    char row[128];
    if(!store_text(output,
                   m->stats_known ? ",\"stats_available\":true" : ",\"stats_available\":false"))
        return false;
    for(unsigned i = 0; m->stats_known && i < 21; ++i) {
        snprintf(row, sizeof(row), ",\"%s\":%llu", stat_names[i], (unsigned long long)m->stats[i]);
        if(!store_text(output, row)) return false;
    }
    return true;
}
static bool store_publish(Storage* storage, File* file, const char* tmp, const char* target,
                          uint64_t size) {
    if(storage_common_rename(storage, tmp, target) != FSE_OK) return false;
    if(!storage_file_open(file, target, FSAM_READ_WRITE, FSOM_OPEN_EXISTING)) return false;
    /* SDK rename is copy+remove. Detect a short copy and explicitly sync the destination. */
    bool ok = storage_file_size(file) == size && storage_file_sync(file);
    if(!storage_file_close(file)) ok = false;
    return ok;
}
/* Each output is synced before rename. Journal is retained if any step fails. */
static bool store_export(SessionStore* s) {
    if(s->open && !storage_file_sync(s->file)) return false;
    const char* names[] = {"observations.csv", "wigle.csv", "access_points.csv", "stats.json",
                           "metadata.json"};
    const char* heads[] = {observation_header, wigle_header, aggregate_header};
    char path[STORE_PATH + 32], target[STORE_PATH + 32], tmp[STORE_PATH + 40];
    File* input = storage_file_alloc(s->storage);
    File* output = storage_file_alloc(s->storage);
    StoreReader* r = calloc(1, sizeof(StoreReader));
    StoreSummary* m = calloc(1, sizeof(StoreSummary));
    char* row = malloc(FBR_LINE_SIZE);
    snprintf(path, sizeof(path), "%s/events.journal", s->path);
    bool ok = r && m && row && storage_file_open(input, path, FSAM_READ, FSOM_OPEN_EXISTING);
    if(ok) ok = store_summary(input, r, m);
    for(unsigned kind = 0; ok && kind < COUNT_OF(names); ++kind) {
        snprintf(target, sizeof(target), "%s/%s", s->path, names[kind]);
        snprintf(tmp, sizeof(tmp), "%s.tmp", target);
        ok = storage_file_seek(input, 0, true) &&
             storage_file_open(output, tmp, FSAM_WRITE, FSOM_CREATE_ALWAYS);
        if(!ok) break;
        memset(r, 0, sizeof(*r));
        if(kind < 3) {
            ok = store_text(output, heads[kind]);
            int result = 0;
            char* f[FBR_FIELDS];
            while(ok && (result = store_next(input, r)) == 1) {
                size_t n = fbr_decode(r->line.data, f);
                if(!n || !store_frame_valid(f, n)) {
                    ok = false;
                    break;
                }
                size_t len = 0;
                if(kind < 2 && !strcmp(f[2], "OBSERVATION")) {
                    len = kind == 0 ? fbr_observation_csv(f, row, FBR_LINE_SIZE)
                                    : fbr_wigle(f, row, FBR_LINE_SIZE);
                    if(kind == 0 && !len) ok = false;
                } else if(kind == 2 && s->complete && r->end > m->archive_start &&
                          (!strcmp(f[2], "AP_DISCOVERED") || !strcmp(f[2], "AP_UPDATED"))) {
                    len = fbr_aggregate_csv(f, row, FBR_LINE_SIZE);
                    if(!len) ok = false;
                }
                if(len) ok = ok && store_write(output, row, len);
            }
            if(result < 0 || r->line.used || r->line.dropping) ok = false;
        } else {
            snprintf(
                row, FBR_LINE_SIZE,
                "{\"schema_version\":1,\"boot\":%lu,\"session\":%lu,\"aggregates_complete\":%s",
                (unsigned long)s->boot, (unsigned long)s->session, s->complete ? "true" : "false");
            ok = store_text(output, row) && store_json_stats(output, m);
            if(kind == 4) {
                snprintf(row, FBR_LINE_SIZE,
                         ",\"protocol_revision\":2,\"journal_committed\":%lu,\"journal_records\":%"
                         "llu,\"interrupted\":%s,"
                         "\"ap_capacity\":%lu,\"queue_capacity\":%lu,\"truncated\":%lu,"
                         "\"discarded\":%lu,"
                         "\"position_timestamp\":\"scan_end\",\"firmware\":",
                         (unsigned long)s->committed, (unsigned long long)s->records,
                         s->interrupted ? "true" : "false", (unsigned long)m->ap_capacity,
                         (unsigned long)m->queue_capacity, (unsigned long)m->truncated,
                         (unsigned long)m->discarded);
                ok = ok && store_text(output, row) && store_json_string(output, m->firmware);
                const char* keys[] = {"local_session_name", "first_observation_utc",
                                      "last_observation_utc", "finished_local", "end_reason"};
                const char* values[] = {strrchr(s->path, '/') + 1, m->first_utc, m->last_utc,
                                        m->finished_local, *m->reason ? m->reason : "INTERRUPTED"};
                for(unsigned i = 0; ok && i < COUNT_OF(keys); ++i) {
                    snprintf(row, FBR_LINE_SIZE, ",\"%s\":", keys[i]);
                    ok = store_text(output, row) && store_json_string(output, values[i]);
                }
                ok = ok && store_text(output, ",\"config\":[");
                for(unsigned i = 0; ok && m->config_known && i < 13; ++i) {
                    snprintf(row, FBR_LINE_SIZE, "%s%lu", i ? "," : "",
                             (unsigned long)m->config[i]);
                    ok = store_text(output, row);
                }
                ok = ok && store_text(output, "]");
            }
            ok = ok && store_text(output, "}\n");
        }
        uint64_t size = storage_file_tell(output);
        ok = ok && storage_file_sync(output);
        if(!storage_file_close(output)) ok = false;
        if(ok) ok = store_publish(s->storage, output, tmp, target, size);
    }
    if(!storage_file_close(input)) ok = false;
    storage_file_close(output);
    storage_file_free(input);
    storage_file_free(output);
    free(r);
    free(m);
    free(row);
    return ok;
}
