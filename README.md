# BambuTagger

An ESP32-based tool for reading, cloning, and writing Bambu Lab filament spool RFID tags.  
Designed around the MIFARE Classic 1K tags embedded in Bambu Lab spools, with full HKDF-SHA256 key derivation, a rotary-encoder OLED menu, a WS2812B status LED, and a built-in web interface.

---

## Features

| Category | Details |
|----------|---------|
| **RFID** | Read, clone, and write Bambu Lab MIFARE Classic 1K spool tags |
| **Key derivation** | HKDF-SHA256 with Bambu Lab salt — no hardcoded keys |
| **OLED menu** | 5-item navigable menu on a 128×64 SH110X / SH1106G display |
| **Rotary encoder** | ENC11/KY-040 encoder for scroll + click navigation |
| **WS2812B LED** | Single addressable LED showing filament colour and status |
| **Web UI** | Four-tab interface: Files / Dumps / Status / WiFi |
| **GitHub browser** | Browse & download dump files directly on the OLED (no PC needed) |
| **GitHub API token** | Optional personal access token for higher API rate limits (5 000 req/hr) |
| **File upload** | Drag-and-drop or file-picker upload of `.bin` dumps to FAT |
| **Directory structure** | Downloaded files mirror the GitHub repo tree on FAT, e.g. `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin` |
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

Schematics are here schematics/schematics.png    
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
│   5 WiFi / Web  │
└─────────────────┘
```

### 1 · Read Tag
Hold a spool near the RC522.  The sketch derives MIFARE keys from the tag UID using HKDF-SHA256, authenticates all 16 sectors, and displays filament type, colour, and weight.  The WS2812B LED lights up in the actual filament colour.

### 2 · Clone Tag
Reads the source tag sector-by-sector into RAM, then prompts for the destination tag.  Writes every block (including sector trailers) to the target tag.  Sector-trailer keys are re-derived for the target UID automatically.

### 3 · Write Dump
Browse the dump files stored on FAT using the on-device directory browser.  The browser reflects the real folder tree on the FAT partition (mirroring the GitHub repo structure).  Select a `.bin` file, present a blank/target tag, and every block is written.

#### FAT directory browser

Row 0 of the list is always a navigation shortcut:

| Depth | Row 0 label | Action |
|-------|-------------|--------|
| Root | `<< MENU` | Return to main menu |
| Inside a subfolder | `< BACK` | Go up one level |

| Encoder action | Result |
|----------------|--------|
| Rotate | Move cursor up / down |
| Click `<< MENU` (root) | Return to main menu |
| Click `< BACK` (sub-dir) | Go up one level |
| Click on `> DIRNAME` | Enter that subdirectory |
| Click on a `.bin` file | Load dump → show write-confirm screen |

- **Title bar** shows the current directory name (e.g. `PLA_BASIC`, `BLACK`); root shows `Select Dump`.
- Entries are sorted: subdirectories first (prefix `>`), then `.bin` files.
- Up to 4 rows are visible at a time; the list scrolls with the cursor.
- After a GitHub download completes, the browser pre-navigates to the folder the file was saved to.

### 4 · GitHub Lib
Browse the [Bambu Lab RFID Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library) repository **directly on the OLED** — no PC required.  Requires WiFi (STA) connectivity.

```
[root]
  └─ PLA/
        └─ PLA Basic/
              └─ Black/
                    └─ 3AD82DAD/
                          └─ dump.json  ← click to download
```

| Encoder action | Result |
|----------------|--------|
| Rotate | Move cursor |
| Click on folder | Navigate into it |
| Click **\< BACK** | Go up one level in the GitHub tree |
| Click on file | Download & save to FAT |

Files are saved to FAT mirroring the GitHub repository directory structure:

| Repo path | Saved as |
|-----------|----------|
| `PLA/PLA Basic/Black/3AD82DAD/dump.bin` | `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin` |
| `ABS/ABS Basic/Red/F1A2B3C4/dump.json` | `/ABS/ABS_BASIC/RED/F1A2B3C4.bin` |
| `TPU/TPU 95A HF/White/00112233/dump.bin` | `/TPU/TPU_95A_HF/WHITE/00112233.bin` |

Parent directories are created automatically if they don't exist.  
The OLED **Write Dump** browser shows the short path (`COLOR/UID`, e.g. `BLACK/3AD82DAD`) when highlighting a file.  
Downloaded files are immediately available in **3 · Write Dump**.

> **Tip:** Set a GitHub personal access token in the **WiFi tab** of the web UI to raise the API rate limit from 60 to 5 000 requests/hour.

### 5 · WiFi / Web
Shows the current IP address (STA or AP).  Open a browser to the displayed address to access the web UI.

---

## Web Interface

Connect to the ESP32's IP (shown on the OLED) in any browser.

### Tab 1 — Files
Fully navigable browser for the FAT file system.

- **Breadcrumb trail** (`Root / PLA / PLA_BASIC / BLACK`) — click any segment to jump directly to that level.
- **Folder entries** (📁) — click to navigate into a subdirectory.
- **⬆ ..** row — click to go up one level (hidden at root).
- **Refresh** button reloads the current directory.
- **Upload** new files via drag-and-drop or file picker (`.bin` only) — files are placed in the currently browsed directory.
- **Delete** any `.bin` file with the ✕ button (full path passed to the server).

### Tab 2 — Dumps
- Browse the GitHub repository tree by folder path.
- Click any `.bin` or `.json` dump file to download it directly to FAT.
- Downloaded files are stored in a directory tree matching the GitHub repo structure (e.g. `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin`).

### Tab 3 — Status
- Shows current WiFi mode, SSID, and IP address.
- Shows free heap and FAT usage (total / used bytes).
- Shows all data from the last read tag.

### Tab 4 — WiFi
- Shows current WiFi mode, SSID, and IP address.
- Scan for networks and connect to a new SSID + password.
- Credentials are saved to ESP32 NVS (via `Preferences`, namespace `wifi`) and restored on every boot — no file is written to FAT.
- **GitHub API Token** — enter a personal access token (read-only, no scopes required) and click **🔑 Save Token**.  The token is stored in NVS and injected as a `Bearer` header into every GitHub API request.  Leave blank to use unauthenticated access (60 req/hr limit).

---

## REST API

All endpoints return JSON unless noted.

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/api/status` | WiFi mode, SSID, IP, selected dump path, FAT usage |
| `POST` | `/api/wifi` | `{"ssid":"…","pass":"…"}` — connect & save |
| `GET` | `/api/scan` | Array of nearby SSIDs |
| `GET` | `/api/list?path=…` | GitHub directory listing for `path` |
| `POST` | `/api/download` | `{"url":"…","path":"…"}` — download raw file to FAT, filename auto-derived from `path` |
| `GET` | `/api/files?dir=<path>` | Directory listing for `path` (default `/`); returns `{path, entries:[{name,isDir,size?}]}` |
| `POST` | `/api/delete` | `{"file":"…"}` — delete a FAT file |
| `POST` | `/api/upload` | `multipart/form-data` field `file` — upload a `.bin` |
| `GET` | `/api/token` | Returns `{"token":"ghp_…"}` (masked after first 4 chars) |
| `POST` | `/api/token` | `{"token":"…"}` — save GitHub API token to NVS |

---

## WS2812B LED Colour Reference

| State | LED |
|-------|-----|
| Main menu (idle) | Off |
| Waiting for tag | 🔵 Slow-breathing blue |
| Tag read OK | Actual filament colour |
| Write Dump — preview | Dump's filament colour (dim) |
| Writing in progress | 🟡 Solid yellow |
| Write / clone success | 🟢 3 × green flash |
| Write / clone failure | 🔴 3 × red flash |
| Timeout / no tag | 🔴 2 × red flash |
| GitHub fetch in progress | 🔵 Fast-breathing blue |
| GitHub download | 🟡 Solid yellow |
| Download success | 🟢 3 × green flash |
| Download failure | 🔴 3 × red flash |
| WiFi STA connected | 💙 Dim cyan |
| WiFi AP mode | 🟠 Dim amber |

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

| Repo path | FAT path |
|-----------|----------|
| `PLA/PLA Basic/Black/3AD82DAD/dump.bin` | `/PLA/PLA_BASIC/BLACK/3AD82DAD.bin` |
| `ABS/ABS Basic/Red/F1A2B3C4/dump.json` | `/ABS/ABS_BASIC/RED/F1A2B3C4.bin` |
| `TPU/TPU 95A HF/White/00112233/dump.bin` | `/TPU/TPU_95A_HF/WHITE/00112233.bin` |
| `PETG/PETG HF/Bambu Green/AABBCCDD/dump.bin` | `/PETG/PETG_HF/BAMBU_GREEN/AABBCCDD.bin` |

Manually uploaded files (via the web UI) are placed at the FAT root (`/`) as-is.

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
/                                    — manually uploaded files live here
  my_custom_dump.bin
  PLA/
    PLA_BASIC/
      BLACK/
        3AD82DAD.bin                 — downloaded via OLED or web Dumps tab
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

1. Install the Arduino IDE (≥ 2.x) and the [ESP32 board package](https://docs.espressif.com/projects/arduino-esp32/en/latest/installing.html).
2. Install all libraries listed above via **Sketch → Include Library → Manage Libraries**.
3. Open `BambuTagger.ino`.
4. Select **Tools → Board → ESP32 Dev Module** and set the partition scheme to **Default 4MB with ffat**.
5. Click **Upload**.
6. Open **Tools → Serial Monitor** at 115200 baud to watch the boot log.

> On first boot the FAT partition will be formatted automatically (`FFat.begin(true)`).

---

## Credits & References

- Dump files: [queengooborg/Bambu-Lab-RFID-Library](https://github.com/queengooborg/Bambu-Lab-RFID-Library)
- HKDF key derivation research: Bambu Lab community reverse-engineering
- MFRC522 library: miguelbalboa
- Adafruit SH110X / GFX / NeoPixel libraries: Adafruit Industries

---

## License

This project is provided as-is for personal and educational use.  
Bambu Lab trademarks and spool tag data formats are the property of Bambu Lab.
