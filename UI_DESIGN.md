# Feberis Pro UI design

## Direction

A field controller for a Wi-Fi recorder: clear state, trustworthy SD recording
counts, and an obvious Start/Stop action. The target is the Flipper's monochrome
128×64 LCD with native fonts, menus, and one-handed navigation.

## Interaction and layout

- A 4 px margin, header, separator, two primary counters, one GPS line, and a
  dedicated native-button footer establish the dashboard hierarchy.
- FontPrimary highlights state and counters; FontSecondary presents labels.
  Large counts use compact k/M/G notation, with full uint64 values in details.
- ViewDispatcher connects Submenu, VariableItemList, Widget scrolling pages,
  DialogEx exit confirmation, and the custom dashboard.
- Left opens Menu, OK performs the visible primary action, and Right opens Networks.
  Back follows the navigation hierarchy and confirms exit during an active session.
- Settings are visible and state-aware. In-use UART selection, configuration
  transitions, and stopped-session requirements are reflected in the controls.
- Network lists are frozen snapshots. A background observation cannot silently
  move the AP under the cursor. Returning from details preserves selection.
- Full SSID/security strings, file paths, GPS information, and statistics are
  available through scrolling detail pages.

No decorative animation or desktop-oriented visuals are needed for the LCD.
Waiting screens describe the operation, such as queue drain or AP archival.

## Revision 2 behavior

The dashboard displays **Unique AP** from STATS and **SD saved** from synchronized
journal records, including records without GPS. GPS, diagnostics, and detailed
session statistics remain separate scrolling pages.

The network browser retrieves 24 GET_AP slots per page and fetches full aggregate
details on demand. Archived sessions are listed 16 directories at a time after
recording stops. A selected session exposes completion/interruption state and
loss counters before re-export.

Settings comes from the ESP's CONFIG. The GUI exposes every configurable field
except the fixed schema version. Active probing is a deliberate, clearly labeled
choice. Configuration changes during recording pause and wait for batch completion;
an already paused session stays paused.

PSRAM initialization, AP-store readiness, actual AP capacity, and queue capacity
are separate from generic heap information. Unavailable AP storage blocks Start.
SRAM and PSRAM figures remain distinct in diagnostics.

Retry recovers and archives. **Recover and resume** is the explicit action that
requests continuation of the same retained session. An ESP restart requires an
intentional new Start.

## Responsiveness and memory

The GUI reads a protected backend snapshot without waiting for the SD mutex.
While the worker performs I/O, the last valid display remains visible. The locked
dashboard drawing model is a compact structure containing only drawing fields,
not the entire AP list or session metadata. UART parsing and SD operations stay
outside GUI callbacks.

## References and attribution

Native interaction patterns were checked against official Flipper sources:

- [GPIO start scene](https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/applications/main/gpio/scenes/gpio_scene_start.c): settings and event separation.
- [Bad USB view](https://github.com/flipperdevices/flipperzero-firmware/blob/1.4.3/applications/main/bad_usb/views/bad_usb_view.c): native footer buttons and state-dependent actions.
- [ViewDispatcher API](https://developer.flipper.net/flipperzero/doxygen/view__dispatcher_8h.html) and the installed SDK 1.4.3 headers.

A local Sapsan companion application was also consulted for navigation patterns.
The views in this project are its own implementation, not copied application modules.

The earlier interface audit used the **unslop** skill by
[Damir Sokolovsky (@heydamir)](https://x.com/heydamir), CC BY 4.0. This attribution
refers to that design process and does not assign a license to this repository.

## Validation and historical previews

Host tests cover UI state priority, available actions, setting locks, large
counters, and the compact dashboard snapshot. Protocol/session tests run with
AddressSanitizer and UndefinedBehaviorSanitizer.

Earlier local previews showed **version 0.2**. Generated previews in `dist/ui/`
are not tracked in Git. Host renders use the firmware's font decoder and fonts;
tiny button glyphs are approximations. They are not emulator output or physical
LCD captures.

To regenerate the current dashboard with a local firmware checkout:

```sh
python3 tests/render_ui.py --u8g2 /path/to/flipperzero-firmware/lib/u8g2
```

The optional renderer requires Pillow and a C compiler. Native menus, scrolling,
physical button handling, and slow-SD behavior still require the
[hardware acceptance tests](tests/HARDWARE.md).
