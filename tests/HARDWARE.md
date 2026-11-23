# Hardware acceptance — FAP 0.3 + Feberis Pro 0.3.0

Run these checks on a physical Feberis/Flipper pair. Unchecked items are pending,
not passed tests. Firmware 0.1.0 or earlier-application results do not establish
revision 2 compatibility. See the [integration reference](../docs/PROTOCOL.md).

## Connection and device readiness

- [ ] USART and LPUART: 3.3 V logic, ESP TX11/RX12, shared ground, 8N1.
- [ ] Busy UART produces a clear error; exiting releases the interface.
- [ ] HELLO 2 → INFO → CONFIG → MEMORY → STATE works without relying on initial READY.
- [ ] Firmware 0.2.0: GET_MEMORY UNKNOWN_COMMAND reports missing diagnostics and uses actual INFO limits.
- [ ] Firmware 0.3.0: INFO/MEMORY limits agree; storage_ready=0 blocks Start.
- [ ] SRAM/PSRAM are separate; queued/queue_capacity and unique/ap_capacity reflect actual limits.
- [ ] Baud 115200/460800/921600: switch after the complete OK, recover from lost OK, restart with NVS baud.
- [ ] Connection recovery probes all three supported speeds without replaying START.

## Scanning, configuration, and browsing

- [ ] BOTH/2G/5G schedules and country channels work; SMART/FULL/UNIQUE and explicit Active settings behave correctly.
- [ ] PAUSE → batch completion → SET_CONFIG → RESUME preserves the session and AP recording baseline.
- [ ] Changing configuration in an already paused session leaves it paused.
- [ ] Hidden, 32-byte, and binary SSIDs, commas, quotes, and backslashes remain intact in full GET_AP details.
- [ ] Network pages contain at most 24 APs; selection stays stable as observations arrive; previous/next work.
- [ ] Native menu navigation and scrolling remain responsive during slow SD operations.

## GPS and exports

- [ ] GPS sends checksummed RMC/GGA at 115200; verify driving, fix loss, and fresh UTC without geolocation.
- [ ] Without a fix, journal/ACK continue, observation coordinates are empty, and WiGLE excludes the observation.
- [ ] HDOP is never converted into accuracy; unavailable altitude/accuracy stays empty; a real 0,0 fix is retained.
- [ ] Final GET_AP export retrieves exactly unique_aps slots and includes detections filtered by SMART.
- [ ] WiGLE output has two headers, 11 columns, LF, and no BOM; perform a real import with empty accuracy.
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
- [ ] AP_TABLE_FULL drains and archives the session, then allows an intentional new session.
- [ ] Interrupted archival/file publication retains the journal and allows a successful repeated export.
- [ ] Torn settings.conf update preserves the previous valid preferences and active-session pointer.
- [ ] SD sessions: 16 directories per page, directory selection, summary, and re-export without replacing the journal.
- [ ] Exit confirmation works; the 3.5-second drain limit leaves a recoverable journal when archival cannot finish.

## Endurance

- [ ] Run with the target AP table and observation load: bounded Flipper RAM, no UART loss, SD sync within 2000 ms.
- [ ] Verify behavior after extended link loss, a full queue, low heap, and power loss on either device.
