# iCloud agenda and holiday-room validation

Validated on 2026-11-15, against the macOS simulator, ESP32-S3 N16R8 board, and the deployed `esp-hub` service.

## Implemented behavior

- Eight pages, with a wall-calendar shortcut directly opening page 7. Seven selectable dates, eight events per batch, scrollable rows, and an expanded event title view. Calendar updates are read-only and do not generate alerts, LED effects, or ledger events.
- HA's `Kiki iCloud` CalDAV connection supplies all its readable calendar entities. The hub refreshes a seven-day window every 15 minutes; device requests are immediate on selection and repeat every minute. Query generations discard superseded replies.
- Independent authenticated `/api/device/calendar` endpoint, bounded below 8 KiB. Failed polls preserve the last complete cache; cache is memory-only. Titles and credentials are not written to logs.
- Eleven room decorations: Spring Festival, Lantern Festival, Dragon Boat Festival, Qixi, Double Ninth, Winter Solstice, National Day, Halloween, US Thanksgiving, Christmas, and Gregorian August 7 birthday. Spring and National Day last seven days; others last one day. No exam-week feature.
- Lunar and solstice dates cover 2026–2099, using the attributed Hong Kong Observatory snapshot in `assets/calendar/holidays_2026_2099.json`. The date generator is offline by default and reproduces the checked-in include byte-for-byte. The regression suite specifically checks 2028-12-21 Winter Solstice and the leap fifth lunar month.

## Automated checks

| Command | Observed result |
|---|---|
| `python3 -m unittest discover -s server -p 'test_*.py'` | 19 passed on local Python 3.13 and in an isolated temporary directory on hub Python 3.10 |
| `cmake --build build-sim` | Passed |
| `ctest --test-dir build-sim --output-on-failure` | 3 passed: existing flight model, calendar model, real LVGL calendar interactions |
| `./scripts/gen_fonts.sh` | Generated zh12/zh16/zh20; 427 UI characters, 3788 full-set characters |
| `./esp.sh shots` and `scripts/holiday_shots.py` | Calendar normal/empty/cached/unconfigured states and all 22 holiday day/night rooms rendered and visually inspected |
| `./esp.sh build` | Passed ESP-IDF 6.1 with `-Werror`; final app 1,920,560 bytes, 176,592 bytes free in the unchanged 2 MiB partition |
| `git diff --check`, Python compilation, shell syntax checks | Passed |

Calendar tests cover all 74 supported years, local-midnight and multi-day boundaries, overlapping calendars, canceled events and recurring instances, long UTF-8 titles, paging, malformed packets, atomic parser failures, source failures, and stale windows. Input tests cover the wall entry, date selection, pagination, eight-page wraparound, double-tap home, night wake, and language rebuild.

The deployed Python 3.10 host exposed an empty-query parsing difference from Python 3.13; the endpoint now handles an absent query explicitly, and was revalidated on both interpreters.

## Deployment and real-board checks

- `server/deploy.sh hub` completed, with the systemd service active. Actual HA connection discovery returned seven calendar entities. The deployed endpoint returned HTTP 200 with `configured=true`, `ok=true`, and real calendar counts.
- Flashed only through `./esp.sh flash`; image hashes verified. Partition table and NVS layout are unchanged. Existing campus network credentials, hub token, Bemfa UID and language preference were retained.
- Wall-calendar touch opened the agenda. Selecting an upcoming date returned four real events in a 602-byte reply; Chinese event titles rendered in both UI languages. No titles are transcribed into this record.
- Mirror captures reported zero bad frames. Captures are in the ignored `shots/` directory: `board_calendar_final.png`, `board_calendar_zh.png`, and `sheet_holidays_day.png` / `sheet_holidays_night.png`.
- JSON allocation hooks prefer PSRAM. LCD DMA buffers use 32 lines, releasing 7680 bytes compared with the previous 40-line pair. SPI clock and screen geometry are unchanged.
- Language rebuilds now delete the old screen tree before constructing its replacement, using a temporary screen between loads. This fixes the observed transient internal-heap exhaustion from holding two complete screen trees at once.
- After the fix and real-data language switches, internal heap was about 28 KiB free with a minimum of 15,204 bytes; PSRAM had about 7885 KiB free. Dashboard, calendar, flights, MQTT and Mi Home were healthy with zero consecutive fetch failures. This is the observed validation interval, not a long-duration soak test.

## Defaults and limits

- All day boundaries and display times use UTC+8; the firmware process timezone remains `UTC0`.
- Calendar event titles are UTF-8 bounded to 96 bytes and source names to 48 bytes, with an ellipsis when truncated. Up to 4096 expanded events may occupy the hub's seven-day window; eight are returned per request.
- New iCloud calendars require reloading the HA CalDAV integration for entity discovery. No Mac bridge is required for routine synchronization.
- The first version displays events and room decorations only; edits and additional calendar reminders remain on Apple devices.
