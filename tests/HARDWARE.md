# Hardware acceptance — FAP 0.4 + Feberis Pro 0.4.0

Run these checks on a physical Feberis/Flipper pair. Unchecked items are pending,
not passed tests. Firmware 0.1.0 or earlier-application results do not establish
revision 2 compatibility. See the [integration reference](../docs/PROTOCOL.md).

## Connection and device readiness

- [ ] Fixed USART: 3.3 V logic, ESP TX11/RX12, shared ground, 8N1; connect without a port-selection step.
- [ ] Busy USART produces a clear error; exiting releases the interface.
- [ ] HELLO 2 → INFO → CONFIG → MEMORY → STATE works without relying on initial READY.
- [ ] Firmware 0.2.0: GET_MEMORY UNKNOWN_COMMAND reports missing diagnostics and uses actual INFO limits.
- [ ] Firmware 0.3.0/0.3.1/0.4.0: INFO/MEMORY limits agree; storage_ready=0 blocks Start.
- [ ] SRAM/PSRAM are separate; queued/queue_capacity and unique/ap_capacity reflect actual limits.
- [ ] Baud 115200/460800/921600: switch after the complete OK, recover from lost OK, restart with NVS baud.
- [ ] Drop the first HELLO/GET_INFO/PING reply after a baud switch: retry at the same speed before probing; never replay SET_CONFIG or START.

## Scanning, configuration, and browsing

- [ ] Fresh install and legacy Interval preference migration: Cycle gap defaults to Continuous and START_SCAN sends 0.
- [ ] 50/100/250 ms presets, diagnostic long gaps, and saved millisecond values work; older firmware receives at least its required 1000 ms.
- [ ] Passive dwell starts from the device configuration (normally 120 ms), not a shorter hidden driving override.
- [ ] Scan metrics show actual start period/gap and queue pressure, with capture drops/cache overflows separate from observation loss.
- [ ] Extra scan/capture PSRAM allocations are displayed separately from GET_MEMORY's original reserved buffers.
- [ ] RADIO_CAPTURE_INIT shows degraded capture without stopping SDK recording.
- [ ] Missing/unsupported optional scan metrics does not block basic connection or final export.

- [ ] BOTH/2G/5G schedules and country channels work; SMART/FULL/UNIQUE and explicit Active settings behave correctly.
- [ ] PAUSE → batch completion → SET_CONFIG → RESUME preserves the session and AP recording baseline.
- [ ] Changing configuration in an already paused session leaves it paused.
- [ ] Hidden, 32-byte, and binary SSIDs, commas, quotes, and backslashes remain intact in full GET_AP details.
- [ ] Network pages contain at most 24 APs; selection stays stable as observations arrive; previous/next work.
- [ ] Native menu navigation and scrolling remain responsive during slow SD operations.

## GPS and exports

- [ ] Start without a fix opens Start without GPS?; Cancel and Back leave no new session and send no START_SCAN.
- [ ] Confirm starts once without position; subsequent valid GPS observations are recorded normally.
- [ ] Start with a fix proceeds directly; losing the fix between GUI refresh and the worker's Start check still prompts.
- [ ] A fix acquired during the prompt updates the text and waits for a decision; error/reconnect dismisses an invalid prompt.
- [ ] Repeated/stale confirmation never creates a second session or sends STOP; normal Stop remains available without a fix.

- [ ] Verified records keep paired strongest reception RSSI, receive timestamp, and historical fix; later dashboard GPS never fills missing positions.
- [ ] RX_UNVERIFIED records are journaled and ACKed after sync, retain reason/time/SSID bytes, recover without duplicates, and never appear in WiGLE.
- [ ] Unknown reason-mask bits do not reject otherwise supported records; embedded NUL SSIDs remain byte-exact in journal/hex export.
- [ ] Recovered metadata describes legacy scan-end timing versus 0.4.0 receive-callback/fallback timing and retains separate scan_metrics.

- [ ] GPS sends checksummed RMC/GGA at 115200; verify driving, fix loss, and fresh UTC without geolocation.
- [ ] Without a fix, journal/ACK continue, observation coordinates are empty, and WiGLE excludes the observation.
- [ ] Missing RMC speed/course is unavailable on the GPS page and empty in CSV; real numeric zero is preserved.
- [ ] HDOP is never converted into accuracy; unavailable altitude/accuracy stays empty; a real 0,0 fix is retained.
- [ ] Final GET_AP export retrieves exactly unique_aps slots and includes detections filtered by SMART.
- [ ] WiGLE headers use version 0.4.0 and exactly brand=Sapsan, with 11 data columns, LF, and no BOM; perform a real import with empty accuracy.
- [ ] Completion/interruption state, truncated/discarded/overflow counts, and full uint64 counters appear in summaries/metadata.

## Recovery and failure handling

- [ ] Lost ACK after sync causes an identical retransmission, one journal record, and another three-argument ACK.
- [ ] Restart FAP after sync but before ACK: recover the journal and RESUME_LINK with reconstructed committed.
- [ ] Restart FAP before/after START and before SESSION_STARTED; a timed-out START is not blindly repeated.
- [ ] Restart ESP: old session becomes interrupted, old-boot records receive no ACK, and a new session requires Start.
- [ ] Disconnect UART for more than five seconds: PING occurs and the frozen journal receives no further records.
- [ ] Retry recovers/archives; Recover and resume explicitly resumes the same retained running session.
- [ ] Removed/full SD, short write, sync failure, and tail-repair failure: no ACK, NACK STORAGE, retained ESP queue.
- [ ] STOPPED with queued records does not close the journal; completion waits for STATE and STATUS.IDLE.
- [ ] SCAN_TIMEOUT after PAUSE/STOP keeps the journal open while scanning=1 and blocks a new scan until driver completion or ESP restart.
- [ ] AP_TABLE_FULL drains and archives the session, then allows an intentional new session.
- [ ] Interrupted archival/file publication retains the journal and allows a successful repeated export.
- [ ] Torn settings.conf update preserves the previous valid preferences and active-session pointer.
- [ ] SD sessions: 16 directories per page, directory selection, summary, and re-export without replacing the journal.
- [ ] Exit confirmation works; the 3.5-second drain limit leaves a recoverable journal when archival cannot finish.

## Endurance

- [ ] Run with the target AP table and observation load: bounded Flipper RAM, no UART loss, SD sync within 2000 ms.
- [ ] Verify behavior after extended link loss, a full queue, low heap, and power loss on either device.

The upstream [firmware 0.4.0 report](https://github.com/bpmcircuits/feberis_pro_wardriver/blob/HEAD/docs/HARDWARE_TEST_2026-11-23_CONTINUOUS.md)
reports passing tests at all three baud rates with a PC receiver. Those results do
not mark any FAP/Flipper SD or native GUI item above as passed.
