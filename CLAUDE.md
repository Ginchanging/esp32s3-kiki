# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an ESP32-S3 N16R8 desk dashboard on the SYSU campus Wi-Fi (WPA2-Enterprise PEAP), starring **Kiki**, an original pixel cat. The seven outer LVGL pages are Kiki's room, GitHub Actions, servers, Tools, mailbox, Mi Home (米家), and overhead flights. Tools opens pomodoro, iCloud agenda, LD2402 desk time, and Apple Health. The UI is bilingual (zh/en).

The 2.8" ILI9341V + FT6336G screen is wired and driven by `main/app_lcd.c`:
- `./esp.sh mirror` still streams the same frames to a Mac window; clicks there become touches alongside the real touch panel.
- `sim/` runs the same UI code on macOS with fixture data.

Also in this repo:
- `server/` holds the hub/agent backend running on three servers.
- `README.md` (Chinese) is the user-facing manual and has the wiring table for the screen.

## Commands

Everything goes through `./esp.sh`. ESP-IDF v6.1 must be at `~/esp/esp-idf-v6.1`; the script sources `export.sh` itself.

```sh
./esp.sh build                  # firmware (idf.py build); check size with: idf.py size (two 6 MiB OTA slots)
./esp.sh flash                  # ALWAYS this, never `idf.py flash` (see invariants)
./esp.sh ota [status|cancel|rollback]  # build/publish via esp-hub SSH; device pulls over pinned HTTPS
./esp.sh status | dash          # campus Wi-Fi status / dashboard status (heap, MQTT, last fetch)
./esp.sh monitor                # serial log; decodes @sysu muse/agent lines
./esp.sh mirror [--shot f.ppm --wait 4 --cmd "CAT react 1"]   # real-board frames (window or one-shot capture)
./esp.sh cat act 4|react 1|look fish|night on|time 3|pomo|auto  # force Kiki's state on the board
./esp.sh agent done|attention|error [text]   # inject an AI-agent push locally
./esp.sh lang zh|en · demo gh_failed|off · muse "文字" · led 48|38|off · mi lamp on|off|bri 60
./esp.sh knob cw|ccw [n]|hold cw [n]|press|long|rev   # EC11 knob stand-in (same queue as the real knob)
./esp.sh lcd [inv|rgb|mx|my|tmx|tmy|txy|bl N|mhz N|reset]   # screen status / per-module fixes (NVS dash/lcd_*)
./esp.sh wifips on|off          # Wi-Fi modem-sleep; default off because its current pulses flicker the 3V3 backlight
./esp.sh sit [status|on|off|range 120|demo]   # LD2402 desk presence and Kiki's stretch reminder
./esp.sh cat diary [week]       # write Kiki's diary / weekly report now

./esp.sh sim [fixtures/x.json]  # simulator window (keys listed at top of sim/main.c)
./esp.sh sim fixtures/23_sit.json --tool sit   # radar view with synthetic readings
./esp.sh shots                  # all simulator screenshots + shots/sheet_*.png contact sheets
./build-sim/dash_sim --shot out.bmp --fixture fixtures/1_ok.json --page 2 --lang en \
    [--act N] [--react N] [--agent done] [--night] [--muse "…"] [--ms 300] \
    [--diary day|week] [--knob click|long|turn:N|hold:N] [--swipe "x0,y0,x1,y1;tap:x,y,n"]   # one screenshot (BMP; `sips -s format png` it)
cmake -S sim -B build-sim -G Ninja && cmake --build build-sim                # first sim build

./scripts/gen_fonts.sh          # REQUIRED after adding any Chinese text (see Fonts)
python3 scripts/cat_art.py --sheet shots/cat_sheet.png && python3 scripts/gen_sprites.py   # after editing Kiki's art
cd server && ./deploy.sh hub|gcp|codex|all|ha   # backend (SSH aliases esp-hub, gcp-isbf, aliyun-codex); ha = Home Assistant
```

Flight tests: `python3 -m unittest discover -s server -p 'test_*.py'` and `ctest --test-dir build-sim --output-on-failure` after a simulator build. To verify UI changes:
- Run `./esp.sh shots` and look at the PNG sheets.
- On hardware, flash, then check with `./esp.sh mirror --shot` and `./esp.sh dash`.

## Architecture

**Shared UI and model, two targets.**
- `components/dash_model` (pure C plus cJSON) and `components/dash_ui` (pure LVGL) compile unchanged into both the firmware and `sim/`. Never include ESP-IDF headers in them.
- Both builds use the vendored LVGL 9.6.0 (`third_party/lvgl`) and one config, `third_party/lv_conf.h`:
  - The firmware build defines `LV_KCONFIG_IGNORE`, so LVGL's Kconfig is never used.
  - `DASH_LV_SIM` switches SDL on only for the simulator.
  - `app_ui.c` `#error`s if the wrong lv_conf is picked up.
- Each component's CMakeLists has an `if(ESP_PLATFORM) idf_component_register … else() add_library …` split.
- `set(COMPONENTS main)` trims the build, so `main` must list every component it needs.

**Firmware tasks (`main/`).**

| Task | Where | Job |
|---|---|---|
| campus | `components/campus_net` (derived from the original `campus.c`) | PEAP connect, backoff retry, SNTP, the `@sysu` USB-serial command protocol. Extra commands go through the `on_command` hook into `main/app_cmd.c` |
| ui | `app_ui.c` + `app_lcd.c` | The **only** task that touches LVGL and the panel. ILI9341 over SPI2 (40 MHz DMA, byte-swapped in place; the DMA-done ISR only gives a semaphore that `flush_wait_cb` takes), FT6336 polled over I2C, LEDC backlight on GPIO7. Also mirror streaming (`@F` RLE+base64 lines over USB) and a touch queue fed by `TOUCH`. Falls back to headless if SPI init fails |
| fetch | `app_fetch.c` | Every 30 s, HTTPS GET of the hub's `/api/device/status`, with the hub cert pinned (`main/certs/hub.pem`). Also sends Mi Home taps (`app_request_mi`) as `POST /api/device/mi` |
| muse | `app_muse.c` | Bemfa MQTT over TLS 9503. Topics `musealert` (Muse), `iPhone` (plain-text phone notifications), and `agentalert` (AI-agent pushes from github.com/Ginchanging/agent-bemfa-notify). Per-topic replay dedupe, because Bemfa resends the last message on subscribe |
| led | `app_led.c` | Onboard WS2812 via RMT + common-cathode KY-016 (R/G/B = GPIO4/5/6) via LEDC timer 1, channels 1/2/3; backlight keeps timer/channel 0. Rules are shared: `dash_led_pattern()` / `dash_led_color_ex()` in dash_model |
| knob | `app_knob.c` | EC11 on GPIO 1/2/42: PCNT quadrature + polled button → `app_knob_push()` queue (the `KNOB` command feeds the same queue) → UI task → `dash_ui_knob()` |
| sensor | `app_sensor.c` + `sht3x.c` | SHT3x single-shot read every 5 s on its own I2C controller, SDA GPIO13 / SCL GPIO14 (user-confirmed 2026-11-16), VCC 3V3. Touch stays on GPIO15/16. Probe 0x44/0x45; validate both CRCs; invalidate after 3 failed samples and retry. Updates existing `sensor_valid` / `temp_c` / `humidity` under the state mutex, never LVGL |
| sit | `app_sit.c` + `dash_sit.c` | HLK-LD2402 J2 `T` to GPIO21 (UART1 RX only). Auto-probes 115200/921600 text reports. The local monotonic desk timer pauses after 10 s of uninterrupted out-of-range or OFF reports and resets after 30 s of absence or 3 s without reports. Only `dash/sit_en` and `dash/sit_range` live in NVS. After one hour, the UI consumes the once-per-stay reminder on an in-range report when higher-priority notices are clear. `SIT demo` / `--sit-demo` play the labelled A stretch without changing the session |

- State lives in `app_state.c`: one `dash_state_t` behind a mutex (`app_lock`/`app_unlock`).
- Other tasks request UI work through flags the UI task polls (lang, `CAT`, mirror, dirty). They never call LVGL.
- The UI writes night/pomodoro status back into the state so the LED task can follow it.
- The Tools tile has four child views and one shared back button. The desk-time view allocates its readout while open. The UI task reads its radar snapshot under the same mutex as the serial diagnostics. Simulator desk-time snapshots live in fixture _sim.sit.
- SHT3x temperature/humidity appears in the existing home-page environment label. `./esp.sh dash` includes sensor freshness/errors and KY-016 PWM diagnostics. `LED 48|38|off` still configures only the onboard WS2812; the external KY-016 follows the same shared light rules independently.
- Hardware validation 2026-11-16: OTA completed with boot verification; the user confirmed live temperature/humidity numbers on the home page and normal touch. KY-016 physical colours have not yet been visually verified.

**Data.**
- `dash_model_parse()` reads the hub JSON. The schema and the server side are in `server/dashd.py`:
  - `hub` mode on 8.138.138.222 polls GitHub and its contributions page, and aggregates `cpu_hist`.
  - `agent` mode on the other two servers pushes metrics to the hub.
- The `fixtures/*.json` files serve three purposes:
  - simulator input, plus an optional `_sim` section for device-side state;
  - firmware demo data, embedded via `EMBED_TXTFILES` and used by the `DEMO` command;
  - test cases for `shots.sh`.
- Mailbox messages (`dash_muse_msg_t`) carry a source (iPhone / Muse / Claude / Codex / Mi Home / Kiki) and a kind (info / done / attention / error).
- **Kiki's diary.** At 23:00 (`dash_diary_due()`, weekly on Sundays) the UI task composes `dash_ui_diary_msg()` and pushes it to the mailbox.
  - Inputs: the hub's `days` (7 days of ci_ok/ci_fail/wash/down/commits) and the device's pomodoro ring.
  - The hub keeps `days` in `Ledger` (`/var/lib/esp-dash/ledger.json`, systemd `StateDirectory`), counting from `add_event()`.
  - The device keeps the pomodoro ring in NVS `dash/pomo_wk` (blob), plus `dash/diary_day`.

**Mi Home (米家).**
- Mi Home has no personal API. The only official route is Xiaomi's Home Assistant integration, so the hub runs HA in Docker on `127.0.0.1:8123` only (`server/homeassistant/`, deployed with `./deploy.sh ha`, image and integration versions pinned).
- First-install HA bootstrap uses bridge networking with only a loopback port publication. Host networking is enabled only after the admin API confirms a stable loopback HTTP configuration; failed setup retains the isolated bootstrap.
- `dashd.py` (`MiHome` class) polls the entities listed in `server/mihome.json` through HA's REST API, using the long-lived token in `/etc/esp-dash/ha_token` (written by `server/set_ha_token.sh`). It stays stdlib-only.
- The hub JSON gains a `mihome` section. The washer card shows an expected finish time (`eta`, epoch s), not a countdown: the washer reports its time left only now and then, so the hub dates the last report (HA `last_changed`) and adds the minutes. A washer going from running to done becomes a hub event `mi_done` (subject = device id). The UI task turns that event into a mailbox message via `dash_ui_mi_event_msg()`.
- Lamp taps flow `dash_ui_set_mi_cb` → `app_request_mi` → fetch task POST. The reply's `mihome` object is parsed with `dash_model_parse_mihome()`.
- `gen_fonts.sh` also scans `server/mihome.json`, so the Chinese device names get into zh12/zh20.
- Kinds: lamp, switch, washer, dispenser (state/temp/tds/filters), scale.
- **The scale never sends its weight to the device.** The S400 only exposes event entities. `MiHome.track_weighing()` stores absolute kg in `/var/lib/esp-dash/scale.json`, and the hub JSON carries relative values only (`d_last`, `d_30`, `hist`, in 0.1 kg). Never log weights.

**Kiki** (`components/dash_ui/dash_cat.c`).
- **Art workflow (user preference):** always look up online pose/animation references before drawing art. Produce viewable drafts with actual clickable PNG/GIF file links, wait for the user's selection, then integrate. Tool-internal image output alone has repeatedly been invisible in this client. For belly rubs the user selected A (gentle side flop): head tilts with shoulders and rests low, forepaws fold at the chest, limbs overlap naturally. Pose references: https://toffeecraft.itch.io/cat-retro and https://nekochan.jp/column/article/24459 . Native finished grids live in `cat_art.py`.
- Layer priority: reaction > mood > pomodoro > routine.
  - Reactions come from hub events and agent pushes.
  - Moods come from system health.
  - The routine is a schedule table in `components/dash_model/dash_schedule.c`, in UTC+8.
- Looks are layer stacks composited at 4× into a 128×128 ARGB buffer shown by an `lv_image` (`dash_pix.c`).
- Art source of truth: `scripts/cat_art.py`, which writes `assets/cat/*.txt` and the `LOOKS` table. `components/dash_ui/sprites/dash_sprites.c` is **generated**; don't edit it.
- The user rejected a white muzzle patch, so `MUZZLE="none"` stays.
- Coding/pomodoro-focus looks use `side_*` layers: Kiki in 3/4 view turned toward a laptop whose screen faces Kiki (the user rejected a laptop seen from behind, and a strict profile looked ugly). The face is baked into `side_body`, so mood faces don't swap there; `compose()` shifts mood props left for these looks. The Muse sign is held at the chest by both paws.

**Overhead flights.** `FlightRadar` in `server/dashd.py` polls adsb.fi at 10 s and enriches
reference routes on a separate worker via adsbdb. `/api/device/flights` uses the existing device
token and stays below 8 KiB. The optional `flights.json` credential comes from ignored
`server/secrets/flights.json`; coordinates explicitly declare WGS84/GCJ02/BD09. Upstream `now`
is milliseconds; the shared model uses UTC seconds/metres and separate freshness/failure state.
`dash_flight_ui.c` owns the seventh page, a temporary six-second outdoor scene, and an actionable
eight-second card. Critical notifications preempt it. `CAT flight` / sim `v` / `--flyover` are
labelled demos and never create live events. The outdoor scene is a rooftop terrace with a
small, rear-facing Kiki at 2× (64×64 canvas), leaving most of the scene for the sky. Kiki's head
and seated body stay completely still: the user rejected head tracking. Only the tail sways,
using `sky back`, `sky tail mid`, `sky tail up`, `sky tail mid` at 450 ms per frame. Draw the tail
behind the fixed body and merge head/body before outlining to avoid a neck seam. Never use a
front-facing idle pose inside the outdoor scene, including its opening and closing moments. Its four time palettes also recolour
the clouds, distant roofs and parapet; night dimming still follows the existing global rule.

**Calendar and holidays.**
Calendar is independent: `CalendarHub` in `server/dashd.py` reads the HA CalDAV entry named by `server/calendar.json` (default `Kiki iCloud`), all its calendar entities, every 15 minutes. `/api/device/calendar` is token-protected and paginated at 8 events/8 KiB; `dash_calendar_t` and its failures never alter dashboard freshness. UI requests go through a latest-query sequence in `app_state.c`, consumed by fetch (never LVGL on fetch). Titles are not logged. HA UI setup includes all-day events; credentials stay in HA. `dash_calendar_ui.c` owns the calendar tool and its wall-calendar shortcut. `dash_season.c` draws code-native decorations and pauses animation behind the flyover scene. Holidays use offline HKO dates for 2026–2099 (`scripts/gen_holidays.py`), not LVGL's inaccurate solar-term table. Birthday is Gregorian August 7. Never edit the generated date include manually.

**Apple Health.** `server/health.py` reads only the iPhone entities explicitly mapped in `/etc/esp-dash/health.json`, in one local HA template call per minute. The small `kiki_health_reports` HA component tracks only actual `mobile_app` update timestamps for those entities, never values; its markers are not restored after HA restarts. Accept a state only when its timestamp matches that report evidence. `ha_setup.py bind` discovers official health unique IDs within one device, excludes their HA Recorder history, and never prints values. HA HTTP settings are managed through the versioned admin API (2026.8+), not the old YAML `http:` block; `ha.kiki-home.top` uses Caddy and HA's native login. `health` is an optional, at-most-2-KiB part of the existing 8-KiB status reply. Its parsing/failures are independent of dashboard freshness and OTA verification. Absolute weights stay in mode-0600 hub state; only relative 0.1-kg values reach the board. Records are observations from onboarding, not HealthKit measurement dates; never invent dates or carry yesterday's totals into today. Private cache v2 discards unverified v1 observations; the wire schema remains 1. `dash_health_ui.c` owns the health tool (activity/sleep/body) with four objects and code-native drawing to spare internal heap. `main/app_ui.c` keeps its non-DMA state snapshot in PSRAM. `DEMO health` is synthetic and never updates server history. `health_watch.py` records resource counters only; device heap/uptime piggyback on existing status requests and the diagnostics endpoint requires the device token.

**Fonts.**

- Montserrat is used for Latin text. The `dash_font_zh{12,16,20}.c` files are generated from Noto Sans SC:
  - zh16 holds GB2312 level-1 plus every UI character.
  - zh12 and zh20 hold only the characters found in `dash_ui`, `dash_model` and `sim` sources.
- New Chinese strings go in `dash_i18n.c`, then re-run `gen_fonts.sh`; otherwise glyphs render as boxes.
- Muse/agent text always uses zh16, even in English mode.

## Invariants and gotchas

- **Keep the existing NVS layout unchanged.** The user authorized the one-time `dual6-v1` OTA migration: `campus_nvs` remains at 0x210000/0x6000, `otadata` is at 0x216000, and 6 MiB slots begin at 0x220000 and 0x820000. Further partition changes require an explicit migration plan and USB.
  - The partition `campus_nvs` with namespaces `profile` and `diagnostics` holds the NetID credentials.
  - `campus.py flash` refuses unknown layouts and protects all existing NVS sectors. It snapshots the current flash before migration/recovery and verifies protected bytes before reset. Never restore a whole-chip snapshot for a routine rollback.
  - New settings belong in the `dash` or `muse` namespaces.
  - Normal USB updates write the inactive OTA slot; never blindly flash `ota_data_initial.bin`. `./esp.sh flash --recover` explicitly reinitializes boot selection to ota_0 while retaining NVS and ota_1.
  - OTA is application-only, with `ota1-` app versions, image SHA-256/ELF identity checks, and boot rollback enabled. Only the UI task draws its progress/Kiki/countdown; each of 3,2,1 is acknowledged after refresh and displayed for a full second. A verification task confirms after UI/storage/Hub health checks (120 s deadline). The `DASH_OTA_TEST_FAIL_BOOT` build option is for explicit rollback validation only.
- **Process TZ stays `UTC0`.** `parse_http_date()` and certificate time checks depend on it; display code adds `DASH_TZ_OFFSET_S`.
- **macOS serial port.**
  - Open it with DTR high and RTS low; dropping DTR while RTS is high resets the S3.
  - Only one process can hold the port. An open `mirror` window makes `flash`, `dash` and other serial commands fail.
- **ESP-IDF builds with `-Werror`** (e.g. `format-truncation`).
- **Paging wraps.** The first/last tiles can't scroll outwards, so that swipe becomes an `LV_EVENT_GESTURE` on the screen; `page_gesture()` wraps it with `dash_ui_step_page()`. The knob and the sim arrows use the same function.
- **Double-tap = home.** `double_tap()` is an event callback on the pointer *indev* (LVGL's short-click streak), not on objects. `dash_ui_create()` attaches it to existing pointer indevs; one created later needs `dash_ui_attach_indev()`. Taps on objects with event callbacks (buttons) don't count. `mirror --cmd` sends commands 0.3 s apart, which is too slow for a double-tap test.
- **Montserrat has no "·".** English strings shown in Montserrat must avoid U+00B7 (it renders as nothing); Chinese fonts include it.
- **LVGL 9.6 API.** Deprecated calls such as `lv_obj_add_flag` warn, so use `lv_obj_set_hidden`/`set_clickable`/`set_scrollable`.
- **Labels.** In LVGL 9, DOTS/CLIP labels need an explicit height or they wrap; `label_w()` handles it.
- **Redraws.** UI update code goes through `set_text`/`set_color`/`show`, which skip unchanged values to avoid redraws, since SPI bandwidth matters once the screen arrives.
- **FreeRTOS tick is 10 ms.** Use delays of 10 ms or more.
- **Memory.** PSRAM (8 MB octal) is enabled. TLS, Wi-Fi and LwIP buffers live there; watch `min_heap` in `./esp.sh dash`.
- JSON allocation hooks are installed once in `app_main`, before tasks, to prefer PSRAM for cJSON nodes. LCD double buffers are 32 lines (30 KiB total internal DMA RAM); keep SPI bus and display geometry unchanged when tuning buffers.
- Language rebuilds switch through a temporary screen and delete the old object tree before constructing the replacement. Keeping two complete eight-page trees alive briefly exhausted internal RAM on the real board.
- **Secrets.** `server/secrets/`, `server/certs/hub.key` and the Bemfa UID are never committed or printed:
  - The device token and the Bemfa UID are written to the board's NVS via `./esp.sh token` and `./esp.sh bemfa`.
  - The Home Assistant token lives only on the hub (`/etc/esp-dash/ha_token`), and so does the Xiaomi login (inside HA's config).
  - The hub certificate exists twice, `server/certs/hub.pem` and `main/certs/hub.pem`. Regenerate both together.
- **Screen power.** The board's `5Vin` pin is input-only until its `IN-OUT` pads are bridged, so the screen's VCC is on 3V3.
- **Screen quirks are settings, not code.** Colour order, inversion, mirroring and the touch transform are `LCD_F_*` bits in NVS `dash/lcd_flags` (default MADCTL 0x48 = MX|BGR, touch untransformed). The backlight stays off until `lv_refr_now()` has drawn the first frame.
