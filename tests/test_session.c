#define FBR_HOST_TEST
#include "../feberis.c"
#include <assert.h>
static App app;
static void reset(void) {
    if(app.file) storage_file_free(app.file);
    memset(&app, 0, sizeof(app));
    app.file = storage_file_alloc(NULL);
    app.store.file = app.file;
    app.serial = (void*)1;
    app.boot = 7;
    app.boot_known = true;
    app.model.idle = app.model.wifi = app.model.storage_ready = true;
    app.baud = app.model.baud = 115200;
    const uint32_t config[] = {1, 0, 0, 0, 120, 15, 6, 30000, 2000, 3000, 50, 2000, 115200};
    memcpy(app.model.config, config, sizeof(config));
    tx_count = sync_count = 0;
    ticks = 100;
}
static void feed(const char* payload) {
    char frame[FBR_LINE_SIZE];
    size_t len = fbr_encode(payload, frame, sizeof(frame));
    assert(len);
    for(size_t i = 0; i < len; ++i)
        if(fbr_feed(&app.reader, frame[i])) receive_line(&app);
}
static void reply(const char* body) {
    char payload[1400];
    snprintf(payload, sizeof(payload), "FBR1\t%lu\t%s", (unsigned long)app.pending_id, body);
    feed(payload);
}
static void fixture(char* out, const char* payload) {
    size_t n = fbr_encode(payload, out, 256);
    assert(n);
    out[n - 1] = 0;
}
static void open_session(void) {
    fixture(app.info_frame, "FBR1\t1\tINFO\t0.3.0\t7\t115200\tPL\t10000\t2048\t2");
    fixture(app.config_frame,
            "FBR1\t2\tCONFIG\t1\t0\t0\t0\t120\t15\t6\t30000\t2000\t3000\t50\t2000\t115200");
    fixture(app.state_frame, "FBR1\t3\tSTATE\t7\t0\t0\t0\t0\t0\t0\t20\t20\t0");
    app.committed_remote = 20;
    assert(open_file(&app));
    app.pending = CmdStart;
    feed("FBR1\t0\tSESSION_STARTED\t7\t1");
    app.pending = CmdNone;
    assert(app.session == 1 && app.store.session == 1);
}
static void observation(unsigned sequence, bool fix) {
    char payload[1400];
    snprintf(
        payload, sizeof(payload),
        "FBR1\t0\tOBSERVATION\t7\t%u\t1\tAA:BB:CC:DD:EE:FF\t41222C5C0042\t[ESS]\t6\t-40\t%u\t2026-"
        "11-23 12:30:00\t%s\t%s\tnan\t1.2\t8\t100\t-\t2\t2437\t1\t1234567890123\tnan\tnan\tnan\t7",
        sequence, fix, fix ? "0" : "nan", fix ? "0" : "nan");
    feed(payload);
}
static uint64_t size(void) { return storage_file_size(app.file); }
static char* read_export(const char* name) {
    char path[220];
    snprintf(path, sizeof(path), "%s/%s", app.model.path, name);
    FILE* f = fopen(path, "rb");
    assert(f);
    char* out = calloc(1, 32768);
    assert(out);
    assert(fread(out, 1, 32767, f) < 32767);
    fclose(f);
    return out;
}
int main(void) {
    reset();
    open_session();
    uint64_t baseline = size();
    unsigned before_sync = sync_count;
    observation(21, false);
    assert(app.sequence == 21 && app.model.rows == 1 && size() > baseline && tx_count == 1);
    assert(sync_at_tx[0] > before_sync);
    assert(strstr(transmissions[0], "\tACK\t7\t1\t21*"));
    uint64_t committed_size = size();
    observation(21, false);
    assert(size() == committed_size && app.model.rows == 1 && tx_count == 2);
    observation(20, false);
    assert(tx_count == 2);
    feed("FBR1\t0\tOBSERVATION\t7\t22\t2\tAA:BB:CC:DD:EE:FF\t\t[ESS]\t6\t-40\t0\t-"
         "\tnan\tnan\tnan\t1\t0\t0\t-\t2\t2437\t1\t1\tnan\tnan\tnan\t0");
    assert(app.sequence == 21);
    app.closing = app.model.stopping = true;
    feed("FBR1\t0\tSTOPPED\t7\tUSER\t0");
    assert(app.store.open); /* STOPPED does not close revision 2 journal. */
    observation(22, true);
    assert(app.model.rows == 2);
    char path[STORE_PATH];
    strcpy(path, app.store.path);
    assert(store_close(&app.store));
    FILE* tail = fopen(strcat(strcpy((char[200]){0}, path), "/events.journal"), "ab");
    assert(tail);
    fputs("FBR1\t0\tOBSERVATION\t7\t23", tail);
    fclose(tail);
    assert(store_recover(&app.store));
    assert(app.store.committed == 22 && app.store.records == 2 && size() > committed_size);
    assert(store_close(&app.store));
    assert(store_export(&app.store));
    char* csv = read_export("observations.csv");
    assert(strstr(csv, "1234567890123,-40,6,2,,,,,8,0,1"));
    free(csv);
    csv = read_export("wigle.csv");
    assert(strstr(csv, "\"A\"\",\\x5C\\x00B\""));
    assert(strstr(csv, ",0,0,,,WIFI\n"));
    assert(!strstr(csv, "nan"));
    free(csv);
    csv = read_export("metadata.json");
    assert(strstr(csv, "\"aggregates_complete\":false"));
    free(csv);

    reset();
    open_session();
    baseline = size();
    app.file->write_limit = 2;
    observation(21, true);
    assert(app.failed && app.need_nack && app.store.committed == 20 && tx_count == 0 &&
           !app.store.open);
    app.file->write_limit = SIZE_MAX;
    assert(store_recover(&app.store) && app.store.committed == 20 && size() == baseline);
    service(&app);
    assert(app.pending == CmdNack && strstr(transmissions[0], "\tNACK\tSTORAGE*"));

    reset();
    open_session();
    app.file->sync_ok = false;
    observation(21, true);
    assert(app.failed && tx_count == 0 && app.store.committed == 20);
    app.file->sync_ok = true;
    assert(store_recover(&app.store) && app.store.committed == 20);

    reset();
    open_session();
    app.file->write_limit = 1;
    app.file->truncate_ok = false;
    observation(21, true);
    assert(app.failed && app.store.damaged && tx_count == 0);

    reset();
    recover(&app);
    assert(app.pending == CmdHello && strstr(app.payload, "HELLO\t2"));
    reply("OK\tHELLO\t0.3.0\t7\t115200\tPL\t128\t2000\t3");
    assert(app.pending == CmdInfo);
    reply("INFO\t0.3.0\t7\t115200\tPL\t10000\t2048\t2");
    assert(app.pending == CmdConfig);
    reply("CONFIG\t1\t0\t0\t0\t120\t15\t6\t30000\t2000\t3000\t50\t2000\t115200");
    assert(app.pending == CmdMemory);
    reply("MEMORY\t1\t1\t8388608\t5000000\t5000000\t4000000\t200000\t190000\t180000\t3000000\t200\t"
          "10000\t0\t2048\t0");
    assert(app.pending == CmdState && app.model.storage_ready);
    reply("STATE\t7\t0\t0\t0\t0\t0\t0\t20\t20\t0");
    assert(app.pending == CmdStop);
    reply("OK\tSTOP");
    assert(app.pending == CmdState);
    reply("STATE\t7\t0\t0\t0\t0\t0\t0\t20\t20\t0");
    assert(app.pending == CmdStatus);
    reply("STATUS\t7\t0\tIDLE\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0\t0.00\t4294967295\tnan\tnan\tnan\t-"
          "\t1\t1");
    assert(app.model.ready && !app.startup);

    reset();
    open_session();
    observation(21, true);
    recover(&app);
    reply("OK\tHELLO\t0.3.0\t7\t115200\tPL\t128\t2000\t3");
    reply("INFO\t0.3.0\t7\t115200\tPL\t10000\t2048\t2");
    reply("CONFIG\t1\t0\t0\t0\t120\t15\t6\t30000\t2000\t3000\t50\t2000\t115200");
    reply("MEMORY\t1\t1\t8388608\t5000000\t5000000\t4000000\t200000\t190000\t180000\t3000000\t200\t"
          "10000\t1\t2048\t1");
    reply("STATE\t7\t1\t1\t0\t0\t1\t1\t21\t20\t1000");
    assert(app.pending == CmdResumeLink && strstr(app.payload, "RESUME_LINK\t7\t1\t21"));
    reply("OK\tRESUME_LINK");
    reply("OK\tSTOP");
    reply("STATE\t7\t1\t0\t0\t0\t0\t0\t21\t21\t2000");
    reply("STATUS\t7\t0\tIDLE\t0\t1\t1\t1\t1\t0\t0\t0\t1\t8\t1.2\t100\t0\t0\tnan\t2026-11-23 "
          "12:30:00\t1\t1");
    assert(app.pending == CmdStats && app.finishing);
    reply("STATS\t0\t1\t18446744073709551615\t1\t1\t0\t1\t0\t1\t0\t1\t0\t1\t0\t0\t0\t0\t0\t0\t0\t21"
          "\t200000\t190000");
    assert(app.pending == CmdAp);
    reply("AP_UPDATED\t7\t1\t0\tAA:BB:CC:DD:EE:FF\t41222C5C0042\t2\t6\t[ESS]\t-40\t-30\t-35."
          "5\t10\t1\t1000\t2000\t2\t1\t0\t0\t0\tnan\tnan\t2026-11-23 12:30:00\t2026-11-23 "
          "12:30:00\t2437\t7");
    assert(app.pending == CmdStats);
    reply("STATS\t0\t1\t18446744073709551615\t1\t1\t0\t1\t0\t1\t0\t1\t0\t1\t0\t0\t0\t0\t0\t0\t0\t21"
          "\t200000\t190000");
    assert(!app.store.open && !app.finishing && app.pending == CmdState && !app.failed);
    reply("STATE\t7\t1\t0\t0\t0\t0\t0\t21\t21\t2000");
    reply("STATUS\t7\t0\tIDLE\t0\t1\t1\t1\t1\t0\t0\t0\t1\t8\t1.2\t100\t0\t0\tnan\t2026-11-23 "
          "12:30:00\t1\t1");
    assert(app.model.ready && !app.model.stopping);
    csv = read_export("access_points.csv");
    assert(strstr(csv, "10,1,-30,-35.5,-40"));
    free(csv);
    csv = read_export("stats.json");
    assert(strstr(csv, "18446744073709551615"));
    free(csv);
    csv = read_export("metadata.json");
    assert(strstr(csv, "\"aggregates_complete\":true"));
    free(csv);

    reset();
    open_session();
    observation(21, true);
    feed("FBR1\t0\tREADY\t0.3.0\t8");
    assert(app.boot == 8 && !app.store.open && app.failed);
    unsigned sent = tx_count;
    observation(22, true);
    assert(sent == tx_count);

    reset();
    command(&app, CmdStart);
    ticks += 2001;
    service(&app);
    assert(app.pending == CmdState); /* Never replay START on timeout. */
    reset();
    app.startup = true;
    command(&app, CmdHello);
    ticks += 2001;
    service(&app);
    assert(app.pending == CmdHello && app.baud == 460800);
    ticks += 2001;
    service(&app);
    assert(app.baud == 921600);
    ticks += 2001;
    service(&app);
    assert(app.failed);
    reset();
    open_session();
    ticks += 5001;
    service(&app);
    assert(app.failed && !app.store.open && app.pending == CmdPing);
    reset();
    open_session();
    observation(21, true);
    app.model.ready = app.model.running = true;
    handle_action(&app, (ActionRequest){ActionConfig, (1u << 24) | 2});
    assert(app.pending == CmdPause && app.resume_after_config);
    reply("OK\tPAUSE_SCAN");
    assert(app.pending == CmdState);
    reply("STATE\t7\t1\t1\t1\t0\t0\t0\t21\t21\t5000");
    assert(app.pending == CmdSetConfig);
    reply("OK\tSET_CONFIG");
    assert(app.pending == CmdConfig);
    reply("CONFIG\t1\t2\t0\t0\t120\t15\t6\t30000\t2000\t3000\t50\t2000\t115200");
    assert(app.pending == CmdResume && app.model.config[1] == 2);
    reply("OK\tRESUME_SCAN");
    assert(!app.model.paused && app.store.records == 1 && app.session == 1);
    sent = tx_count;
    handle_action(&app, (ActionRequest){ActionConfig, (12u << 24) | 1});
    assert(tx_count == sent && strstr(app.model.message, "Stop"));

    app.model.unique = 1;
    app.model.ap_capacity = 10000;
    handle_action(&app, (ActionRequest){ActionNetworks, 0});
    assert(app.pending == CmdAp);
    reply("AP_UPDATED\t7\t1\t0\tAA:BB:CC:DD:EE:FF\t41222C5C0042\t2\t6\t[ESS]\t-40\t-30\t-35."
          "5\t10\t1\t1000\t2000\t2\t1\t0\t0\t0\tnan\tnan\t2026-11-23 12:30:00\t2026-11-23 "
          "12:30:00\t2437\t7");
    assert(app.model.ap_count == 1 && app.model.aps[0].aggregate && !app.model.network_loading);
    observation(22, true);
    assert(app.model.ap_count == 1 && app.store.records == 2);
    handle_action(&app, (ActionRequest){ActionNetworkDetail, 0});
    assert(app.pending == CmdApDetail);
    reply("AP_UPDATED\t7\t1\t0\tAA:BB:CC:DD:EE:FF\t41222C5C0042\t2\t6\t[ESS]\t-40\t-30\t-35."
          "5\t10\t1\t1000\t2000\t2\t1\t0\t0\t0\tnan\tnan\t2026-11-23 12:30:00\t2026-11-23 "
          "12:30:00\t2437\t7");
    assert(strstr(app.model.network_detail, "Detections: 10") &&
           strstr(app.model.network_detail, "Strongest fix: 0\nLat: - / Lon: -"));

    reset();
    app.model.ready = true;
    handle_action(&app, (ActionRequest){ActionConfig, (12u << 24) | 1});
    assert(app.pending == CmdSetConfig);
    reply("OK\tSET_CONFIG");
    assert(app.baud == 460800 && app.pending == CmdHello);
    char saved_path[STORE_PATH];
    strcpy(saved_path, app.store.path);
    FILE* settings = fopen(CONFIG_DIR "/settings.conf", "ab");
    assert(settings);
    fputs("FBR1\t0\tSETTINGS\t0\t921600", settings);
    fclose(settings);
    app.baud = 115200;
    assert(load_settings(&app) && app.baud == 460800 && !strcmp(app.store.path, saved_path));
    app.baud = 921600;
    assert(save_settings(&app));
    app.baud = 115200;
    assert(load_settings(&app) && app.baud == 921600);

    reset();
    recover(&app);
    reply("OK\tHELLO\t0.2.0\t7\t115200\tPL\t128\t2000\t3");
    reply("INFO\t0.2.0\t7\t115200\tPL\t256\t128\t2");
    reply("CONFIG\t1\t0\t0\t0\t120\t15\t6\t30000\t2000\t3000\t50\t2000\t115200");
    assert(app.pending == CmdMemory);
    reply("ERR\tUNKNOWN_COMMAND");
    assert(app.pending == CmdState && !app.model.memory_known && app.model.storage_ready &&
           app.model.ap_capacity == 256);

    reset();
    open_session();
    observation(21, true);
    handle_action(&app, (ActionRequest){ActionRecoverScan, 0});
    assert(app.resume_requested && app.pending == CmdHello);
    /* Replay the boot/session STATE after negotiation; link recovery must not send START. */
    app.pending = CmdState;
    reply("STATE\t7\t1\t1\t1\t0\t1\t1\t21\t20\t5000");
    assert(app.pending == CmdResumeLink);
    reply("OK\tRESUME_LINK");
    assert(app.pending == CmdResume);
    reply("OK\tRESUME_SCAN");
    assert(app.model.ready && !app.startup && app.store.records == 1 && app.session == 1);
    assert(app.pending == CmdStatus);
    assert(store_close(&app.store));
    app.pending = CmdNone;

    for(unsigned i = 0; i < 40; ++i) {
        char directory[STORE_PATH];
        snprintf(directory, sizeof(directory), SESSION_DIR "/session_20990101_000000_%04u", i);
        assert(storage_common_mkdir(NULL, directory) == FSE_OK);
    }
    list_sessions(&app, 0);
    assert(app.session_count == 16);
    assert(strstr(app.sessions[0], "0039"));
    list_sessions(&app, 1);
    assert(app.session_count == 16 && strstr(app.sessions[0], "0023"));
    list_sessions(&app, 2);
    assert(app.session_count == 16 && strstr(app.sessions[0], "0039"));

    storage_file_free(app.file);
    app.file = NULL;
    puts("session: revision 2 handshake, journal recovery, durable ACK, queue drain, exports, SD "
         "faults, reboot OK");
}
