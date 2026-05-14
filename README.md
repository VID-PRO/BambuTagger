# <img alt="logo" src="Logo/bambutagger.png" height="36" /> BambuTagger

An ESP32-based tool for reading, cloning, and writing Bambu Lab filament spool RFID tags.  
Designed around the MIFARE Classic 1K tags embedded in Bambu Lab spools, with full HKDF-SHA256 key derivation, a rotary-encoder OLED menu, a WS2812B status LED, and a built-in web interface.

---

## Features

| Category | Details |
|----------|---------|
| **RFID** | Read, clone, and write Bambu Lab MIFARE Classic 1K spool tags |
| **Gen1A / Gen2 / Gen3 / Gen4 magic card support** | Gen1A: 0x40/0x43 backdoor, all 64 blocks verbatim; Gen4 (GTU/GDM): CF-command backdoor, all 64 blocks verbatim; Gen3 (APDU): block 0 via `90 F0 CC CC` APDU; Gen2 (CUID/FUID): implicit detection via block 0 write |
| **Key derivation** | HKDF-SHA256 with Bambu Lab salt — no hardcoded keys |
| **OLED menu** | 7-item navigable menu on a 128×64 SH110X / SH1106G display |
| **Rotary encoder** | ENC11/KY-040 encoder for scroll + click navigation |
| **WS2812B LED** | Single addressable LED showing filament colour and status |
| **Web UI** | Five-tab interface: Files / Dumps / Status / WiFi / BambuMan |
| **GitHub browser** | Browse & download dump files directly on the OLED (no PC needed) |
| **GitHub API token** | Optional personal access token for higher API rate limits (5 000 req/hr) |
| **OTA firmware update** | Check for and flash the latest release from GitHub — both from the OLED menu and the web UI — with live progress bar and automatic reboot |
| **Firmware version tracking** | `FIRMWARE_VERSION` define keeps the OLED menu, web UI, and OTA check in sync |
| **BambuMan OLED browser** | Browse the bambuman.ee catalog by Material → Type → Color → UID on the OLED; sync the catalog directly from the OLED without a PC |
| **BambuMan web search** | Sync catalog, filter by material/color, fetch & write from web UI |
| **File upload** | Drag-and-drop or file-picker upload of `.bin` dumps to FAT |
| **Directory structure** | Both GitHub and BambuMan downloads use the same `/{MAT}/{TYPE}/{COLOR}/{UID}.bin` tree on FAT — no separate `/BM/` folder for bin files |
| **FAT browser (OLED)** | "Write Dump" navigates the real directory tree on-device — no flat list |
| **FAT browser (web)** | Web Files tab browses the full FAT directory tree with a live breadcrumb trail |
| **WiFi** | Auto-STA on boot; AP fallback `BambuTagger` / `bambu1234` |
| **Serial debug** | Timestamped debug output; disable with `#define DEBUG_SERIAL 0` |

---

## Hardware

### Bill of Materials

| Component | Notes | Buy |
|-----------|-------|-----|
| **ESP32** dev board (30-pin) | Any variant with ≥ 4 MB flash | https://de.aliexpress.com/item/1005007488059883.html |
| **RC522** RFID module | SPI interface | https://de.aliexpress.com/item/1005006907801802.html |
| **WS2812B LED** (1 - 3 pixel) | 5 V tolerant; powered from 3.3 V is fine for 1 LED | https://de.aliexpress.com/item/32560280169.html |
| **SH1106G 128×64 1.3\" OLED and rotary encoder** | I²C, 0x3C address (SH110X family) | https://de.aliexpress.com/item/1005007728845587.html |

### Pin Assignments

| Signal | ESP32 GPIO |
|--------|-----------|
| RC522 CS | 5 |
| RC522 RST | 27 |
| RC522 SCK | 18 |
| RC522 MOSI | 23 |
| RC522 MISO | 19 |
| RC522 3.3V | 3.3V |
| RC522 GND | GND |
| OLED SDA | 21 |
| OLED SCL | 22 |
| OLED 3.3V | 3.3V |
| OLED GND | GND |
| Encoder A/CLK | 34 |
| Encoder B/DT | 35 |
| Encoder BTN | 32 |
| WS2812B DIN | 26 |

Schematics are here [schematics](/schematics/schematics.png)   
> All encoder and button pins are INPUT_PULLUP; the encoder button is active-low.

---

## Software

### Required Libraries (Arduino Library Manager)

| Library | Version tested |
|---------|---------------|
| `MFRC522` | ≥ 1.4.10 |
| `Adafruit SH110X` | ≥ 2.1.8 |
| `Adafruit GFX Library` | ≥ 1.11.9 |
| `ArduinoJson` | ≥ 7.x |
| `Adafruit NeoPixel` | ≥ 1.12.0 |
| `mbedTLS` | Built into ESP32 Arduino core |

### Board Settings (Arduino IDE)

| Setting | Value |
|---------|-------|
| Board | **ESP32 Dev Module** |
| Partition Scheme | **Default 4MB with ffat** |
| Flash Size | 4 MB |
| Upload Speed | 921600 |
| Monitor Speed | **115200** |

> The sketch calls `FFat.begin(true)` — the `true` flag formats the FAT partition automatically on first boot if it is blank.

---

## OLED Menu

Navigate with the rotary encoder: rotate to scroll, click to select.  
Press and hold, or select **\<\< MENU** (always the first row in any browser) to return to the main menu.

```
┌─────────────────┐
│ ≡  BambuTagger  │
│   1 Read Tag    │
│   2 Clone Tag   │
│   3 Write Dump  │
│   4 GitHub Lib  │
│   5 BambuMan    │
│   6 WiFi / Web  │
│   7 OTA Update  │
└─────────────────┘
```

> The menu scrolls — up to 4 items are visible at a time; rotate the encoder past the last visible row to reveal more.

### 1 · Read Tag
Hold a spool near the RC522.  The sketch derives MIFARE keys from the tag UID using HKDF-SHA256, authenticates all 16 sectors, and displays filament type, colour, and weight.  The WS2812B LED lights up in the actual filament colour.

### 2 · Clone Tag
Reads the source tag sector-by-sector into RAM, then prompts for the destination tag.  Writes every block to the target tag.

**Key handling during clone/write:**

The write routine first attempts to unlock the card via the **Gen1A hardware backdoor** (send `0x40` / `0x43` — a special command accepted only by "magic" MIFARE clone chips).  If the card responds, all 64 blocks are written directly with **no sector authentication required**, including block 0 (normally read-only UID block).  This transparently handles Chinese Gen1A clone cards that were previously written with unknown Bambu keys.

If Gen1A is not detected, the routine probes for **Gen4 (GTU / GDM / USCUID)** cards by sending the version command `CF 00000000 CC` — a genuine Gen4 card responds `00 00 00 02 AA`. If confirmed, all 64 blocks (including block 0 / UID) are written verbatim via `CF <pw> CD` backdoor commands.

If Gen4 is not detected, the routine tries **Gen3 (APDU-based)** cards by sending `90 F0 CC CC 10 <block0>`. A Gen3 card responds `90 00` — this both detects the card type and writes block 0 (UID) atomically. Blocks 1–63 are then written via 3-key normal auth, with trailer keys derived from the dump's UID (which the card now presents).

If Gen3 also fails, the routine enters the **normal-auth path** — which transparently handles **Gen2 / CUID / FUID** cards. These look like standard MIFARE but allow writing block 0 after normal auth. The routine detects this implicitly by attempting `MIFARE_Write` to block 0; on Gen2 it succeeds (UID overwritten), on genuine MIFARE it fails silently. Once block 0 is confirmed written and sector 0's trailer is complete, the reader **cycles the RF antenna** (`PCD_AntennaOff` → 30 ms → `PCD_AntennaOn` → re-poll) so the card power-cycles from HALT state back to IDLE and responds to the next REQA. A plain `PICC_HaltA()` is insufficient here: ISO 14443A halted cards ignore REQA (0x26) and only wake on WUPA (0x52), but the MFRC522 library's `PICC_IsNewCardPresent()` sends REQA — so the card would silently ignore every re-poll attempt. The antenna cycle forces a hard capacitor drain, returning the card to IDLE state where it answers REQA normally. The MFRC522 then re-discovers the card's UID (new dump UID) and sectors 1–15 auth proceeds correctly.

If none of the above magic paths are detected (genuine MIFARE Classic), the routine falls back to a **3-key normal-auth strategy**:

| Path | Condition | How it works |
|------|-----------|-------------|
| **Gen1A backdoor** | Card responds to `0x40` / `0x43` | All 64 blocks written verbatim; block 0 (UID) overwritten |
| **Gen4 (GTU/GDM)** | Card responds to `CF 00000000 CC` with version `00 00 00 02 AA` | All 64 blocks written verbatim via `CF <pw> CD` commands; block 0 (UID) overwritten |
| **Gen3 (APDU)** | Card responds `90 00` to APDU `90 F0 CC CC 10 <block0>` | Block 0 written via APDU; blocks 1–63 via 3-key auth; trailers use dump-UID-derived keys |
| **Gen2 implicit** | Standard MIFARE protocol; block 0 write succeeds after normal auth | Detected during sector 0 write; block 0 overwritten; RF antenna cycled off/on so card exits HALT → IDLE state and responds to REQA; trailer keys re-derived from dump UID; sectors 1–15 auth then succeeds |
| **Normal auth** | Genuine MIFARE Classic (block 0 read-only) | 3-key trial per sector; trailers re-written with dest-UID-derived keys |

Normal-auth key trial order:

| Priority | Key tried | Succeeds when target is… |
|----------|-----------|--------------------------|\n| 1 | `FFFFFFFFFFFF` | blank / factory-fresh tag |
| 2 | Bambu key derived from **dest UID** | previously used Bambu spool |
| 3 | Bambu key derived from **source UID** | (fallback; rarely needed) |

On the normal-auth path, sector trailers are always written with dest-UID-derived keys so the Bambu printer can authenticate the result correctly.

The write function returns the number of sectors written successfully (0–16).  The OLED and LED show three outcomes:

| Result | LED | Message |
|--------|-----|---------|
| 16/16 sectors OK | 🟢 green flash | "Write complete!" |
| 1–15/16 sectors OK | 🟠 amber flash | "Partial! X/16 sec" |
| 0/16 sectors | 🔴 red flash | "Write failed!" |

### 3 · Write Dump
Browse the dump files stored on FAT using the on-device directory browser.  The browser reflects the real folder tree on the FAT partition (mirroring the GitHub repo structure).  Select a `.bin` file, present a target tag, and every block is written using the same Gen1A-backdoor / 3-key auth strategy described above.  Gen1A, Gen4 (GTU/GDM/USCUID), Gen3 (APDU), and Gen2 (CUID/FUID) magic cards are all detected automatically — no special configuration is needed.

#### FAT directory browser

Row 0 of the list is always a navigation shortcut:

| Depth | Row 0 label | Action |
|-------|-------------|--------|
| Root | `<< MENU` | Return to main menu |
| Inside a subfolder | `< BACK` | Go up one level |

| Encoder action | Result |
|----------------|--------|
| Rotate | Move cursor up / down (wraps: past last entry → entry 1; past entry 1 → last entry) |
| Click `<< MENU` (root) | Return to main menu |
| Click `< BACK` (sub-dir) | Go up one level |
| Click on `> DIRNAME` | Enter that subdirectory |
| Click on a `.bin` file | Load dump → show write-confirm screen |

- **Title bar** shows the current directory name (e.g. `PLA_BASIC`, `BLACK`); root shows `Select Dump`.
- Entries are sorted: subdirectories first (prefix `>`), then `.bin` files.
- Up to 4 rows are visible at a time; the list scrolls with the cursor.
- After a GitHub download completes, the OLED shows a write-confirm screen so you can write the tag immediately (see **§ 4 · GitHub Lib**). The FAT browser is also pre-navigated to the downloaded file's folder so you can write it later from **3 · Write Dump**.

### 4 · GitHub Lib
Browse the [Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library) repository **directly on the OLED** — no PC required.  Requires WiFi (STA) connectivity.

Every level has a direct `<< MENU` shortcut.  At the root, row 0 is `<< MENU`.  Inside any subdirectory, row 0 is `< BACK` (go up one level) and row 1 is `<< MENU` (return to main menu).

```
[root]
  Row 0: << MENU
  └─ PLA/
        Row 0: < BACK
        Row 1: << MENU
        └─ PLA Basic/
              Row 0: < BACK
              Row 1: << MENU
              └─ Black/
                    Row 0: < BACK
                    Row 1: << MENU
                    └─ 3AD82DAD/
                          Row 0: < BACK
                          Row 1: << MENU
                          └─ dump.json  ← click to download
```

| Encoder action | Result |
|----------------|--------|
| Rotate | Move cursor up / down |
| Click `<< MENU` (root) | Return to main menu |
| Click `< BACK` (sub-level row 0) | Go up one directory level |
| Click `<< MENU` (sub-level row 1) | Return to main menu directly |
| Click on folder | Navigate into it |
| Click on file | Download → save to FAT → write-confirm screen |

Files are saved to FAT mirroring the GitHub repository directory structure:

| Repo path | Saved as |
|-----------|----------|
| `PLA/PLA Basic/Black/3AD82DAD/dump.bin` | `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin` |
| `ABS/ABS Basic/Red/F1A2B3C4/dump.json` | `/ABS/ABS_BASIC/RED/F1A2B3C4.bin` |
| `TPU/TPU 95A HF/White/00112233/dump.bin` | `/TPU/TPU_95A_HF/WHITE/00112233.bin` |

Parent directories are created automatically if they don't exist.

#### Write-confirm after download

After a successful download the OLED immediately shows a write-confirm screen (identical to the BambuMan flow):

```
GitHub OK!
PLA Basic
PLA Matte

[click]=WRITE
[enc]=cancel
```

| Action | Result |
|--------|--------|
| **Click** (within 15 s) | Load dump → enter RFID write flow immediately |
| **Rotate encoder** | Cancel — return to GitHub browser at same directory |
| **15 s timeout** | Same as rotate — return to browser |

The dump is also pre-loaded into the write buffer, so if you cancel and later navigate to **3 · Write Dump** the cursor is already positioned on the file.

> **Tip:** Set a GitHub personal access token in the **WiFi tab** of the web UI to raise the API rate limit from 60 to 5 000 requests/hour.

### 5 · BambuMan Lib
Browse the [bambuman.ee](https://bambuman.ee/tags) community tag database **directly on the OLED** in a 4-level hierarchy.  Requires WiFi.  The catalog can be synced from the OLED itself — no PC or web browser needed.

#### Catalog hierarchy

Every level has a direct `<< MENU` shortcut at the top.

At the **Material level** (top), two fixed rows appear before any material entries:

```
<< MENU            ← return to main menu
> Sync Catalog     ← sync the catalog without leaving the OLED
PLA
PETG
ABS
TPU
…
```

At **Type / Color / UID levels**, two fixed rows appear before entries:

```
< BACK             ← go up one level
<< MENU            ← return to main menu directly
PLA_BASIC
PLA_MATTE
…
```

Drill down to reach individual UIDs:

```
Material (PLA, PETG, ABS, TPU …)
  └─ Type (PLA_BASIC, PLA_MATTE …)
        └─ Color (BLACK, RED, MARBLE …)
              └─ UID (3AD82DAD, 9510C2A3 …)  ← click to fetch & write
```

#### Encoder controls

| Encoder action | Result |
|----------------|--------|
| Rotate | Move cursor up / down |
| Click `<< MENU` (any level) | Return to main menu |
| Click `> Sync Catalog` (Material level, row 1) | Run catalog sync on-device (see below) |
| Click `< BACK` (Type / Color / UID level, row 0) | Go up one level |
| Click a Material / Type / Color | Navigate into it |
| Click a UID | Fetch `data.bin` from bambuman.ee → save to `/{MAT}/{TYPE}/{COLOR}/{UID}.bin` |
| Click after a successful fetch | Enter RFID write flow immediately |
| Rotate after a successful fetch | Cancel fetch result, stay in browser |

#### OLED Sync Catalog flow

Clicking **> Sync Catalog** runs a 4-step progress sequence directly on the OLED:

| Step | Display | What happens |
|------|---------|-------------|
| 1 | `1/4 Find ZIP…` | Probes `bambuman.ee/files/data_YYYY-MM-DD.zip` for the last 7 days |
| 2 | `2/4 Getting size…` | HTTP HEAD request to determine ZIP length |
| 3 | `3/4 Read EOCD…` | Fetches the last 512 bytes to locate the central directory |
| 4 | `4/4 Writing…` (counter) | Streams the central directory only (~400 KB Range request), writes `/BM/catalog.json` |

On success the OLED shows **"Done! N entries"** and the LED flashes green — click the encoder to return to the material browser.  On any error a red message is shown for 3 seconds before returning to the browser.

> **No decompression** — only the ZIP file listing is fetched, not the full archive.  The Range request is typically 400–500 KB regardless of how many dump files are in the ZIP.

#### Error states

| Message | Meaning |
|---------|---------|
| `No catalog — sync first` | `/BM/catalog.json` is missing — use `> Sync Catalog` (row 1) or the web UI BambuMan tab |
| `UID not found` | HTTP 404 from bambuman.ee |
| `Blocked (CF) — try Web UI` | HTTP 403 (Cloudflare) — use the web UI BambuMan tab instead |

### 7 · OTA Update
Checks the [BambuTagger GitHub releases](https://github.com/VID-PRO/BambuTagger/releases/latest) for a newer firmware version and flashes it over-the-air.  Requires WiFi (STA) connectivity.

#### OLED update flow

```
Step 1 — Checking…          Step 2a — Up to date          Step 2b — Update available
┌──────────────────────┐    ┌──────────────────────┐      ┌──────────────────────┐
│   OTA Firmware       │    │   OTA Firmware       │      │   OTA Firmware       │
│   1/3 Checking…      │    │   Up to date!        │      │   Update!            │
│                      │    │   v1.0.0             │      │   Now: v1.0.0        │
│   [cyan LED]         │    │   [green flash]      │      │   New: v1.0.1        │
│                      │    │                      │      │   [click]=FLASH      │
└──────────────────────┘    └──────────────────────┘      │   [enc]=cancel  15 s │
                                                           └──────────────────────┘

Step 3 — Flashing                      Step 4 — Done
┌──────────────────────┐               ┌──────────────────────┐
│   OTA Firmware       │               │   OTA Firmware       │
│   Flashing...        │               │   Done! Rebooting    │
│   [████████░░] 72%   │               │                      │
│   [yellow LED]       │               │   [green flash]      │
└──────────────────────┘               └──────────────────────┘
```

| Encoder action | Result |
|----------------|--------|
| Click (step 2b confirm screen, within 30 s) | Begin download and flash |
| Rotate (step 2b confirm screen) | Cancel — return to main menu |
| 30 s timeout | Same as rotate |

The firmware flashed is `BambuTagger.ino.bin` from the latest GitHub release (app partition only — no need to re-flash bootloader or partition table).  After a successful flash the device reboots automatically.

| Result | LED | Message |
|--------|-----|---------|
| Already up to date | 🟢 green flash | `Up to date! vX.Y.Z` |
| Flashing in progress | 🟡 Pulsing cyan → solid yellow | `Flashing... N%` |
| Flash success | 🟢 green flash | `Done! Rebooting` |
| Network error / GitHub error | 🔴 red flash | Error detail on OLED |
| User cancelled | — | Returns to main menu |

### 6 · WiFi / Web
Shows the current IP address (STA or AP).  Open a browser to the displayed address to access the web UI.

---

## Web Interface

Connect to the ESP32's IP (shown on the OLED) in any browser.

### Tab 1 — Local Library
Fully navigable browser for the FAT file system.

- **Breadcrumb trail** (`Root / PLA / PLA_BASIC / BLACK`) — click any segment to jump directly to that level.
- **Folder entries** (📁) — click to navigate into a subdirectory.
- **⬆ ..** row — click to go up one level (hidden at root).
- **Refresh** button reloads the current directory.
- **Upload** new files via drag-and-drop or file picker (`.bin` only) — files are placed in the currently browsed directory.
- **✍️ Write** any `.bin` file directly to an RFID tag: click the button, place a tag on the RC522 within 20 seconds.  A modal overlay shows progress and polls for completion.
- **Delete** any `.bin` file with the 🗑 button (full path passed to the server).

### Tab 2 — GitHub Library
- Browse the GitHub repository tree by folder path.
- Click any `.bin` or `.json` dump file to download it directly to FAT.
- Downloaded files are stored in a directory tree matching the GitHub repo structure (e.g. `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin`).

### Tab 3 — BambuMan Library
Browse and search the [bambuman.ee](https://bambuman.ee/tags) community tag database without leaving the web UI.

#### Sync Catalog
Click **🔄 Sync Catalog** to fetch the bambuman.ee file index.  The ESP32 sends an HTTP Range request for the ZIP **central directory only** (~400 KB, no decompression), parses all `Material/Type/Color/UID/data.bin` paths, and saves them to `/BM/catalog.json` on FAT.

- Takes ~30–60 seconds on first run.
- The entry count is shown after a successful sync (e.g. "2 622 entries — ready to search").
- Re-sync any time to pick up new community dumps.
- The same sync can also be triggered from the OLED (**5 BambuMan Lib → > Sync Catalog**) without opening a browser.

#### Search
- **Material dropdown** — auto-populated from the catalog (PLA, PETG, ABS, TPU …).
- **Color / name text input** — live-filters the results as you type.
- Scrollable results table (up to 100 rows): UID · Material · Type · Color.
- Per-row **⬇ Fetch** — downloads `data.bin` to `/{MAT}/{TYPE}/{COLOR}/{UID}.bin` (material/type/color are known from the catalog row).
- Per-row **✏️ Write** — downloads (if not already cached) and queues a tag-write (20 s window).

#### Fetch by UID
Enter a known UID directly and click **⬇ Fetch** to retrieve `data.bin` from `https://bambuman.ee/api/tags/{UID}/data.bin`.  Both the web API and the OLED follow the same 3-tier path resolution:

1. **Caller-supplied m/t/c** — if material, type, and color are passed explicitly (Search row results), used directly.
2. **Catalog lookup** — if any field is missing, `/BM/catalog.json` is stream-searched for the UID to fill in the gaps.  If the catalog has an entry the file lands in `/{MAT}/{TYPE}/{COLOR}/{UID}.bin`.
3. **Flat fallback** — `/BM/{UID}.bin` only if the catalog has no entry for this UID (e.g. catalog never synced).

### Tab 4 — Status
- Shows current WiFi mode, SSID, and IP address.
- Shows free heap and FAT usage (total / used bytes).
- Shows all data from the last read tag.

### Tab 5 — OTA Update
Check for and apply firmware updates without connecting a USB cable.

| UI element | Action |
|------------|--------|
| **Current firmware** | Displays the running `FIRMWARE_VERSION` |
| **🔍 Check for Updates** | Queries `GET /api/ota/check` → shows current and latest version |
| **⬆️ Flash Update** *(appears only when an update is available)* | `POST /api/ota/update` — streams the app binary, flashes, reboots |

- The **Check** step is read-only and safe to run at any time.
- The **Flash** step asks for confirmation before downloading.
- If the device reboots mid-response (successful flash), the JS detects the dropped connection and shows "Device rebooting… reconnect in a few seconds."
- While flashing, the OLED shows a live progress bar and the LED pulses cyan.

### Tab 6 — Config
- Shows current WiFi mode, SSID, and IP address.
- Scan for networks and connect to a new SSID + password.
- Credentials are saved to ESP32 NVS (via `Preferences`, namespace `wifi`) and restored on every boot — no file is written to FAT.
- **GitHub API Token** — enter a personal access token (read-only, no scopes required) and click **🔑 Save Token**.  The token is stored in NVS and injected as a `Bearer` header into every GitHub API request.  Leave blank to use unauthenticated access (60 req/hr limit).

---

## REST API

All endpoints return JSON unless noted.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | WiFi mode, SSID, IP, selected dump path, FAT usage, `app_state` |
| `POST` | `/api/wifi` | `{"ssid":"…","pass":"…"}` — connect & save |
| `GET` | `/api/scan` | Array of nearby SSIDs |
| `GET` | `/api/list?path=…` | GitHub directory listing for `path` |
| `POST` | `/api/download` | `{"url":"…","path":"…"}` — download raw file to FAT |
| `GET` | `/api/files?dir=<path>` | FAT directory listing; returns `{path, entries:[{name,isDir,size?}]}` |
| `POST` | `/api/delete` | `{"file":"…"}` — delete a FAT file |
| `POST` | `/api/writetag` | `{"path":"…"}` — load FAT dump and start tag-write (20 s window) |
| `POST` | `/api/upload` | `multipart/form-data` field `file` — upload a `.bin` |
| `GET` | `/api/token` | Returns `{"token":"ghp_…"}` (masked after first 4 chars) |
| `POST` | `/api/token` | `{"token":"…"}` — save GitHub API token to NVS |
| `POST` | `/api/bm/sync` | Fetch bambuman.ee ZIP central directory → save `/BM/catalog.json` |
| `GET` | `/api/bm/catalog` | Stream `/BM/catalog.json` from FAT |
| `GET` | `/api/bm/fetch?uid=XXXXXXXX[&mat=…&type=…&color=…]` | Fetch `data.bin` → caller m/t/c → catalog lookup → `/BM/` fallback |
| `GET` | `/api/bm/list` | Return `[{path, size}]` of all BambuMan-downloaded files (from `/BM/index.txt`, stale entries pruned) |
| `GET` | `/api/ota/check` | `{current, latest, update_available, download_url, size, ok}` — compare running firmware to latest GitHub release |
| `POST` | `/api/ota/update` | Download and flash the latest app binary; responds `{ok:true}` before rebooting (or `{ok:false, error:"…"}` on failure) |

---

## WS2812B LED Colour Reference

| State | LED |
|-------|-----|
| Main menu (idle) | Off |
| Waiting for tag | 🔵 Slow-breathing blue |
| Tag read OK | Actual filament colour |
| Write Dump — preview | Dump's filament colour (dim) |
| Gen1A backdoor detected | 🔵 Fast single flash blue |
| Gen2 (CUID/FUID) detected | 🔵 Fast single flash blue |
| Gen3 (APDU) detected | 🔵 Fast single flash blue |
| Gen4 (GTU/GDM) detected | 🔵 Fast single flash blue |
| Writing in progress | 🟡 Solid yellow |
| Write success (16/16 sectors) | 🟢 3 × green flash |
| Write partial (1–15/16 sectors) | 🟠 3 × amber flash |
| Write / clone failure (0/16) | 🔴 3 × red flash |
| Timeout / no tag | 🔴 2 × red flash |
| GitHub fetch in progress | 🔵 Fast-breathing blue |
| GitHub / BambuMan download | 🟡 Solid yellow |
| Download success | 🟢 3 × green flash |
| Download failure | 🔴 3 × red flash |
| WiFi STA connected | 💙 Dim cyan |
| WiFi AP mode | 🟠 Dim amber |
| OTA — querying GitHub | 🔵 Slow-breathing blue |
| OTA — downloading / flashing | 🔵 Pulsing cyan → 🟡 solid yellow during flash |
| OTA — flash success | 🟢 3 × green flash |
| OTA — flash failure | 🔴 3 × red flash |

---

## WiFi / Networking

1. On boot the sketch loads saved credentials from ESP32 NVS (`Preferences`, namespace `wifi`) and attempts to connect to the saved SSID.
2. If connection succeeds within 10 s, the OLED shows the IP and the LED turns dim cyan.
3. If it fails, the AP `BambuTagger` (password `bambu1234`) is started at `192.168.4.1` and the LED turns dim amber.
4. Use the **WiFi / Web** menu or the web UI to change credentials at any time.

---

## GitHub API Token

The OLED GitHub browser and the web Dumps tab both use the GitHub REST API to list repository contents.  Without authentication, GitHub enforces a limit of **60 requests per hour** per IP address.

To avoid hitting this limit:

1. Generate a free GitHub **Personal Access Token** (classic or fine-grained):  
   → [github.com/settings/tokens](https://github.com/settings/tokens)  
   No scopes are required — a token with zero permissions is sufficient for reading public repositories.
2. Open the BambuTagger web UI → **WiFi tab**.
3. Paste the token into the **GitHub API Token** field and click **🔑 Save Token**.
4. The token is saved to NVS and used automatically for all subsequent GitHub requests (rate limit raised to **5 000 req/hr**).

> The token is sent only to `api.github.com` and `raw.githubusercontent.com`.  It is never logged to serial output.

---

## FAT Directory Structure

Downloaded dump files are stored in a directory tree that mirrors the GitHub repository layout.  Parent directories are created automatically.

**Path mapping rules:**
- Each directory segment is **uppercased**
- Spaces are replaced with **underscores**
- The leaf filename is always `<UID>.bin` (extension forced to `.bin`)

| Source | FAT path |
|--------|----------|
| GitHub: `PLA/PLA Basic/Black/3AD82DAD/dump.bin` | `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin` |
| GitHub: `ABS/ABS Basic/Red/F1A2B3C4/dump.json` | `/ABS/ABS_BASIC/RED/F1A2B3C4.bin` |
| BambuMan: material PLA, type PLA_BASIC, color Black, UID `9510C2A3` | `/PLA/PLA_BASIC/BLACK/9510C2A3.bin` |
| BambuMan: UID only (catalog has no entry for this UID) | `/BM/9510C2A3.bin` |
| BambuMan catalog index | `/BM/catalog.json` |
| BambuMan download index | `/BM/index.txt` |
| Manual web upload | `/<filename>.bin` (flat at root) |

---

## Dump File Format

Dump files are raw binary images of a MIFARE Classic 1K card:

- **Size:** exactly **1 024 bytes** (64 blocks × 16 bytes)
- **Layout:** block 0 at offset 0, block 63 at offset 1008
- **Extension:** `.bin`

The GitHub repository also contains `.json` dumps.  The firmware parses several common JSON layouts automatically and converts them to binary before saving.

---

## Key Derivation

Keys are derived per-sector per-card using **HKDF-SHA256**:

```
IKM  = tag UID (4 bytes)
Salt = 9a759cf2c4f7ca0e... (Bambu Lab salt, hardcoded)
Info = sector index (1 byte)
OKM  = first 6 bytes → MIFARE Key A
       next  6 bytes → MIFARE Key B
```

This mirrors the algorithm used by Bambu Lab's own firmware.  All derivation is done on-device using the ESP32's built-in mbedTLS library — no network call is needed.

---

## Serial Debug Output

Open the Serial Monitor at **115200 baud** to see timestamped logs:

```
========================================
  BambuTagger  – debug build
========================================
[STATE] -> READ_TAG
[RFID]  processReadTag: waiting for tag (15 s)...
[RFID]  UID: 3A D8 2D AD
[RFID]  Key derivation complete.
[AUTH]  blk=03 keyA A1B2C3D4E5F6 -> OK
[READ]  sector 00 auth -> OK
[READ]    blk 00: 3A D8 2D AD 00 00 00 00  00 00 00 00 00 00 00 00
```

To disable all debug output and save flash/RAM:

```cpp
// BambuTagger.ino, line 70
#define DEBUG_SERIAL 0
```

---

## FAT Layout

```
/                                    — manually uploaded files
  my_custom_dump.bin
  BM/
    catalog.json                     — bambuman.ee catalog (sync once from web UI or OLED)
    index.txt                        — list of all BambuMan-downloaded file paths
    9510C2A3.bin                     — fallback only: no catalog entry for this UID
  PLA/
    PLA_BASIC/
      BLACK/
        3AD82DAD.bin                 — downloaded via OLED GitHub browser or web Dumps tab
        9510C2A3.bin                 — downloaded via OLED BambuMan browser or web BambuMan tab
      RED/
        F1A2B3C4.bin
  ABS/
    ABS_BASIC/
      WHITE/
        00112233.bin
```

> WiFi credentials and the GitHub API token are stored in ESP32 NVS (flash key-value store via `Preferences`), not on FAT.

---

## Building & Flashing

### Manual build (Arduino IDE)

1. Install the Arduino IDE (≥ 2.x) and the [ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html).
2. Install all libraries listed above via **Sketch → Include Library → Manage Libraries**.
3. Open `BambuTagger.ino`.
4. Select **Tools → Board → ESP32 Dev Module** and set the partition scheme to **Default 4MB with ffat**.
5. Click **Upload**.
6. Open **Tools → Serial Monitor** at 115200 baud to watch the boot log.

> On first boot the FAT partition will be formatted automatically (`FFat.begin(true)`).

### Automated releases (GitHub Actions)

A workflow file at `.github/workflows/release.yml` builds and publishes releases automatically.

#### Trigger conditions

| Event | Compile | Release created |
|-------|---------|----------------|
| Push / PR to `main` | ✅ (CI check) | ❌ |
| Push a `v*` tag | ✅ | ✅ |
| Manual workflow dispatch | ✅ | only if tagged commit |

#### Creating a release

Before tagging, update `#define FIRMWARE_VERSION` near the top of `BambuTagger.ino` to match the tag (e.g. `"1.0.1"`).  The OTA check compares the running value against the release tag, so they must match.

```bash
# 1. Update FIRMWARE_VERSION in BambuTagger.ino, commit
git add BambuTagger/BambuTagger.ino
git commit -m "Bump to v1.0.1"

# 2. Tag and push — workflow compiles + publishes release automatically
git tag v1.0.1
git push origin v1.0.1
```

The workflow compiles the sketch on **ESP32 Arduino core 2.0.17** (partition scheme `default_ffat`), merges all binary parts with `esptool.py merge_bin`, and attaches four files to the GitHub release:

| File | Flash address | Description |
|------|--------------|-------------|
| `BambuTagger_merged.bin` | `0x0` | **All-in-one** — flash this with `esptool write_flash 0x0 BambuTagger_merged.bin` |
| `BambuTagger.ino.bin` | `0x10000` | App binary only |
| `BambuTagger.ino.bootloader.bin` | `0x1000` | Bootloader |
| `BambuTagger.ino.partitions.bin` | `0x8000` | Partition table |

#### Flashing the merged binary (no Arduino IDE needed)

```bash
pip install esptool
esptool.py --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 BambuTagger_merged.bin
```

On Windows replace `/dev/ttyUSB0` with the correct COM port (e.g. `COM3`).

> **Tip:** Arduino library installations and the ESP32 core are cached by the workflow — warm builds finish in ~45 seconds.

---

## Credits & References

- Dump files: [queengooborg/Bambu-Lab-RFID-Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library)
- Community tag database: [bambuman.ee](https://bambuman.ee/tags)
- [RFID-Tag-Guide](https://github.com/Bambu-Research-Group/RFID-Tag-Guide)
- HKDF key derivation research: Bambu Lab community reverse-engineering
- MFRC522 library: by [miguelbalboa](https://github.com/makerspaceleiden/rfid)
- [Adafruit SH110X](https://github.com/adafruit/Adafruit_SH110X) / [GFX](https://github.com/adafruit/Adafruit-GFX-Library) / [NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) libraries: Adafruit Industries

---

## License

This project is provided as-is for personal and educational use.  
Bambu Lab trademarks and spool tag data formats are the property of Bambu Lab.
