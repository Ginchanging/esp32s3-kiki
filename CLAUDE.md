# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for an ESP32-S3 N16R8 desk dashboard on the SYSU campus Wi-Fi (WPA2-Enterprise PEAP), starring **Kiki**, an original pixel cat. It has six LVGL pages: Kiki's room, GitHub Actions, servers, pomodoro, mailbox, and Mi Home (米家). The UI is bilingual (zh/en).

The 2.8" ILI9341V + FT6336G screen has **not arrived yet**:
- The board renders headless. `./esp.sh mirror` streams the real frames to a Mac window, and clicks in that window become touches.
- `sim/` runs the same UI code on macOS with fixture data.

Also in this repo:
- `server/` holds the hub/agent backend running on three servers.
- `README.md` (Chinese) is the user-facing manual and has the wiring table for the screen.

## Commands

Everything goes through `./esp.sh`. ESP-IDF v6.1 must be at `~/esp/esp-idf-v6.1`; the script sources `export.sh` itself.

```sh
./esp.sh build                  # firmware (idf.py build); check size with: idf.py size (app partition is 2 MB)
./esp.sh flash                  # ALWAYS this, never `idf.py flash` (see invariants)
./esp.sh status | dash          # campus Wi-Fi status / dashboard status (heap, MQTT, last fetch)
./esp.sh monitor                # serial log; decodes @sysu muse/agent lines
./esp.sh mirror [--shot f.ppm --wait 4 --cmd "CAT react 1"]   # real-board frames (window or one-shot capture)
./esp.sh cat act 4|react 1|look fish|night on|time 3|pomo|auto  # force Kiki's state on the board
./esp.sh agent done|attention|error [text]   # inject an AI-agent push locally
./esp.sh lang zh|en · demo gh_failed|off · muse "文字" · led 48|38|off · mi lamp on|off|bri 60
./esp.sh knob cw|ccw [n]|hold cw [n]|press|long|rev   # EC11 knob stand-in (same queue as the real knob)
./esp.sh cat diary [week]       # write Kiki's diary / weekly report now

./esp.sh sim [fixtures/x.json]  # simulator window (keys listed at top of sim/main.c)
./esp.sh shots                  # all simulator screenshots + shots/sheet_*.png contact sheets
./build-sim/dash_sim --shot out.bmp --fixture fixtures/1_ok.json --page 2 --lang en \
    [--act N] [--react N] [--agent done] [--night] [--muse "…"] [--ms 300] \
    [--diary day|week] [--knob click|long|turn:N|hold:N] [--swipe x0,y0,x1,y1]   # one screenshot (BMP; `sips -s format png` it)
cmake -S sim -B build-sim -G Ninja && cmake --build build-sim                # first sim build

./scripts/gen_fonts.sh          # REQUIRED after adding any Chinese text (see Fonts)
python3 scripts/cat_art.py --sheet shots/cat_sheet.png && python3 scripts/gen_sprites.py   # after editing Kiki's art
cd server && ./deploy.sh hub|gcp|codex|all|ha   # backend (SSH aliases esp-hub, gcp-isbf, aliyun-codex); ha = Home Assistant
```

There is no unit-test suite here. To verify UI changes:
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
| ui | `app_ui.c` | The **only** task that touches LVGL. Headless display; mirror streaming (`@F` RLE+base64 lines over USB); a touch queue fed by `TOUCH` |
| fetch | `app_fetch.c` | Every 30 s, HTTPS GET of the hub's `/api/device/status`, with the hub cert pinned (`main/certs/hub.pem`). Also sends Mi Home taps (`app_request_mi`) as `POST /api/device/mi` |
| muse | `app_muse.c` | Bemfa MQTT over TLS 9503. Topics `musealert` (Muse) and `agentalert` (AI-agent pushes from github.com/Ginchanging/agent-bemfa-notify). Per-topic replay dedupe, because Bemfa resends the last message on subscribe |
| led | `app_led.c` | Onboard WS2812 via RMT. Rules are shared: `dash_led_pattern()` / `dash_led_color_ex()` in dash_model |
| knob | `app_knob.c` | EC11 on GPIO 1/2/42: PCNT quadrature + polled button → `app_knob_push()` queue (the `KNOB` command feeds the same queue) → UI task → `dash_ui_knob()` |

- State lives in `app_state.c`: one `dash_state_t` behind a mutex (`app_lock`/`app_unlock`).
- Other tasks request UI work through flags the UI task polls (lang, `CAT`, mirror, dirty). They never call LVGL.
- The UI writes night/pomodoro status back into the state so the LED task can follow it.

**Data.**
- `dash_model_parse()` reads the hub JSON. The schema and the server side are in `server/dashd.py`:
  - `hub` mode on 8.138.138.222 polls GitHub and its contributions page, and aggregates `cpu_hist`.
  - `agent` mode on the other two servers pushes metrics to the hub.
- The `fixtures/*.json` files serve three purposes:
  - simulator input, plus an optional `_sim` section for device-side state;
  - firmware demo data, embedded via `EMBED_TXTFILES` and used by the `DEMO` command;
  - test cases for `shots.sh`.
- Mailbox messages (`dash_muse_msg_t`) carry a source (Muse / Claude / Codex / Mi Home / Kiki) and a kind (done / attention / error).
- **Kiki's diary.** At 23:00 (`dash_diary_due()`, weekly on Sundays) the UI task composes `dash_ui_diary_msg()` and pushes it to the mailbox.
  - Inputs: the hub's `days` (7 days of ci_ok/ci_fail/wash/down/commits) and the device's pomodoro ring.
  - The hub keeps `days` in `Ledger` (`/var/lib/esp-dash/ledger.json`, systemd `StateDirectory`), counting from `add_event()`.
  - The device keeps the pomodoro ring in NVS `dash/pomo_wk` (blob), plus `dash/diary_day`.

**Mi Home (米家).**
- Mi Home has no personal API. The only official route is Xiaomi's Home Assistant integration, so the hub runs HA in Docker on `127.0.0.1:8123` only (`server/homeassistant/`, deployed with `./deploy.sh ha`, image and integration versions pinned).
- `dashd.py` (`MiHome` class) polls the entities listed in `server/mihome.json` through HA's REST API, using the long-lived token in `/etc/esp-dash/ha_token` (written by `server/set_ha_token.sh`). It stays stdlib-only.
- The hub JSON gains a `mihome` section. A washer going from running to done becomes a hub event `mi_done` (subject = device id). The UI task turns that event into a mailbox message via `dash_ui_mi_event_msg()`.
- Lamp taps flow `dash_ui_set_mi_cb` → `app_request_mi` → fetch task POST. The reply's `mihome` object is parsed with `dash_model_parse_mihome()`.
- `gen_fonts.sh` also scans `server/mihome.json`, so the Chinese device names get into zh12/zh20.
- Kinds: lamp, switch, washer, dispenser (state/temp/tds/filters), scale.
- **The scale never sends its weight to the device.** The S400 only exposes event entities. `MiHome.track_weighing()` stores absolute kg in `/var/lib/esp-dash/scale.json`, and the hub JSON carries relative values only (`d_last`, `d_30`, `hist`, in 0.1 kg). Never log weights.

**Kiki** (`components/dash_ui/dash_cat.c`).
- Layer priority: reaction > mood > pomodoro > routine.
  - Reactions come from hub events and agent pushes.
  - Moods come from system health.
  - The routine is a schedule table in `components/dash_model/dash_schedule.c`, in UTC+8.
- Looks are layer stacks composited at 4× into a 128×128 ARGB buffer shown by an `lv_image` (`dash_pix.c`).
- Art source of truth: `scripts/cat_art.py`, which writes `assets/cat/*.txt` and the `LOOKS` table. `components/dash_ui/sprites/dash_sprites.c` is **generated**; don't edit it.
- The user rejected a white muzzle patch, so `MUZZLE="none"` stays.

**Fonts.**
- Montserrat is used for Latin text. The `dash_font_zh{12,16,20}.c` files are generated from Noto Sans SC:
  - zh16 holds GB2312 level-1 plus every UI character.
  - zh12 and zh20 hold only the characters found in `dash_ui`, `dash_model` and `sim` sources.
- New Chinese strings go in `dash_i18n.c`, then re-run `gen_fonts.sh`; otherwise glyphs render as boxes.
- Muse/agent text always uses zh16, even in English mode.

## Invariants and gotchas

- **Never change `partitions.csv` or the existing NVS layout.**
  - The partition `campus_nvs` with namespaces `profile` and `diagnostics` holds the NetID credentials.
  - `campus.py flash` erases `campus_nvs` if the partition table changes.
  - New settings belong in the `dash` or `muse` namespaces.
- **Process TZ stays `UTC0`.** `parse_http_date()` and certificate time checks depend on it; display code adds `DASH_TZ_OFFSET_S`.
- **macOS serial port.**
  - Open it with DTR high and RTS low; dropping DTR while RTS is high resets the S3.
  - Only one process can hold the port. An open `mirror` window makes `flash`, `dash` and other serial commands fail.
- **ESP-IDF builds with `-Werror`** (e.g. `format-truncation`).
- **Paging wraps.** The first/last tiles can't scroll outwards, so that swipe becomes an `LV_EVENT_GESTURE` on the screen; `page_gesture()` wraps it with `dash_ui_step_page()`. The knob and the sim arrows use the same function.
- **Montserrat has no "·".** English strings shown in Montserrat must avoid U+00B7 (it renders as nothing); Chinese fonts include it.
- **LVGL 9.6 API.** Deprecated calls such as `lv_obj_add_flag` warn, so use `lv_obj_set_hidden`/`set_clickable`/`set_scrollable`.
- **Labels.** In LVGL 9, DOTS/CLIP labels need an explicit height or they wrap; `label_w()` handles it.
- **Redraws.** UI update code goes through `set_text`/`set_color`/`show`, which skip unchanged values to avoid redraws, since SPI bandwidth matters once the screen arrives.
- **FreeRTOS tick is 10 ms.** Use delays of 10 ms or more.
- **Memory.** PSRAM (8 MB octal) is enabled. TLS, Wi-Fi and LwIP buffers live there; watch `min_heap` in `./esp.sh dash`.
- **Secrets.** `server/secrets/`, `server/certs/hub.key` and the Bemfa UID are never committed or printed:
  - The device token and the Bemfa UID are written to the board's NVS via `./esp.sh token` and `./esp.sh bemfa`.
  - The Home Assistant token lives only on the hub (`/etc/esp-dash/ha_token`), and so does the Xiaomi login (inside HA's config).
  - The hub certificate exists twice, `server/certs/hub.pem` and `main/certs/hub.pem`. Regenerate both together.
- **Screen bring-up (round 2).** Replace the headless display in `main/app_ui.c` with esp_lcd ILI9341 + FT5x06 touch, using the README pin table. Nothing else should need to change.
