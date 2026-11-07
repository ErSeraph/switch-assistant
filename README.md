# Switch Assistant

<p align="center">
  <img src="img/logo.png" alt="Switch Assistant logo" width="220">
</p>
<p align="center">
  <a href="https://www.home-assistant.io/integrations/mqtt/"><img alt="Home Assistant" src="https://img.shields.io/badge/Home%20Assistant-MQTT-41bdf5?style=for-the-badge"></a>
  <a href="https://switch.hacks.guide/"><img alt="Nintendo Switch" src="https://img.shields.io/badge/Nintendo%20Switch-homebrew-e60012?style=for-the-badge"></a>
</p>
<p align="center">
  <a href="https://github.com/FaserF/switch-assistant/releases/latest"><img src="https://img.shields.io/github/v/release/FaserF/switch-assistant?style=flat-square" /></a>
  <a href="https://github.com/FaserF/switch-assistant/blob/main/LICENSE"><img src="https://img.shields.io/github/license/FaserF/switch-assistant?style=flat-square" /></a>
  <a href="https://github.com/FaserF/switch-assistant/releases"><img src="https://img.shields.io/github/downloads/FaserF/switch-assistant/total?style=flat-square&color=1f883d"></a>
</p>

<p align="center">
  Available for Nintendo Switch via
</p>

<p align="center">
  <img src="img/hbappstore.png" alt="Switch Assistant logo" width="30">
  <a href="https://hb-app.store/switch/SwitchAssistant">
    <img alt="HB App Store"
      src="https://img.shields.io/badge/HB%20App%20Store-Download%20Now-009400?style=for-the-badge">
  </a>
</p>

Switch Assistant connects your Nintendo Switch to Home Assistant through MQTT.

Once configured, it publishes console sensors to Home Assistant and shows Home Assistant popup notifications directly over your game, without opening Tesla Menu.

> [!TIP]
> Having issues, freezes or crashes? See [Troubleshooting](#troubleshooting).

## What It Does

- **Remote Control Entity**: Control your Nintendo Switch like a smart TV / remote in Home Assistant (power, sleep/wake, volume up/down/mute, brightness, reboot, shutdown, activity / channel switcher).
- **Media Player Entity**: Track playing state, view current game title, control volume, and launch installed games directly from the source / channel list.
- **Game Launcher & Player Filters**: Dedicated `select` entities for all installed games as well as filtered by player count (2 Player, 4 Player, 8/8+ Player games). Selecting a game instantly launches it on the console.
- **Player Count Sensors**: Reports the number and list of installed multiplayer games (2p, 4p, 8p+) with full JSON attributes for automations and dashboard cards.
- **Firmware Update Entity**: Shows installed firmware version with direct links to [THZoria/NX_Firmware Releases](https://github.com/THZoria/NX_Firmware/releases).
- **Console & Battery Telemetry**: Shows battery level, charging state, voltage, temperature, health, screen brightness, volume, audio output target, and connected controllers.
- **Game Detection**: Reports whether a game is currently running and publishes the current game Title ID and localized title name.
- **Power Buttons**: Exposes reboot and shutdown buttons in Home Assistant.
- **In-Game Notifications**: Displays Home Assistant popup notifications over your game without needing Tesla Menu.
- **Live Video Streaming**: Real-time RTSP screen stream directly into Home Assistant Generic Camera.
- **Zero Config MQTT Discovery**: All entities appear automatically in Home Assistant.

## Supported Home Assistant Entities

| Domain | Entity | Description | Default Status | Type / Values |
| --- | --- | --- | --- | --- |
| `remote` | `Remote` | TV-style remote control | **Enabled** | Power on/off, sleep/wake, volume, brightness, reboot, shutdown, activity (game launch) |
| `media_player` | `MediaPlayer` | Media player controller | **Enabled** | State (`playing`, `idle`, `off`), volume slider, current game title, source selector (game launch) |
| `select` | `Game Launcher` | Switch installed games picker | **Enabled** | Channel/game picker (select title to launch) |
| `select` | `2 Player Games` | Installed 2+ player games picker | **Enabled** | Select title to launch |
| `select` | `4 Player Games` | Installed 4+ player games picker | **Enabled** | Select title to launch |
| `select` | `8 Player Games` | Installed 8+ player games picker | **Enabled** | Select title to launch |
| `sensor` | `2 Player Games Count` | Count & list of 2+ player games | **Enabled** | Integer count, `games` attribute list |
| `sensor` | `4 Player Games Count` | Count & list of 4+ player games | **Enabled** | Integer count, `games` attribute list |
| `sensor` | `8 Player Games Count` | Count & list of 8+ player games | **Enabled** | Integer count, `games` attribute list |
| `update` | `Firmware` | Firmware update tracker | **Enabled** | Current installed FW version, release URL ([THZoria/NX_Firmware](https://github.com/THZoria/NX_Firmware/releases)) |
| `update` | `Atmosphère` | Atmosphère CFW update tracker | **Enabled** | Current installed CFW version, GitHub release URL & summary ([Atmosphere-NX](https://github.com/Atmosphere-NX/Atmosphere)) |
| `sensor` | `Battery` | Device battery percentage (HA Battery integration) | **Enabled** | `%` (0–100, `device_class: battery`) |
| `binary_sensor` | `Battery Charging` | Charger connected / charging status | **Enabled** | `ON` / `OFF` (`device_class: battery_charging`) |
| `binary_sensor` | `Docked` | Console dock status | **Enabled** | `ON` / `OFF` |
| `binary_sensor` | `Game Running` | Status if a game is active | **Enabled** | `ON` / `OFF` |
| `sensor` | `Current Game` | Name of the running game | **Enabled** | Game Title String |
| `sensor` | `Player Count` | Number of active player controllers | **Enabled** | `0` – `8` |
| `button` | `Reboot` | Restarts the Switch console | **Enabled** | Button press |
| `button` | `Shutdown` | Powers down the Switch console | **Enabled** | Button press |
| `button` | `Sleep` | Puts the Switch console to sleep | **Enabled** | Button press |
| `button` | `Close Game` | Returns to the Home Menu / closes active game | **Enabled** | Button press |
| `button` | `Launch Daybreak` | Launches the Daybreak firmware updater | **Enabled** | Button press |
| `event` | `Overlay Action` | Events triggered from overlay / shortcuts | **Enabled** | `toggle`, `turn_on`, `turn_off`, `call_service`, etc. |
| `notify` | `Popup Notification` | In-game notification toast | **Enabled** | Text message / JSON payload |
| `sensor` | `Current Game Title ID` | Hex Title ID of running game | *Disabled by default* | 16-character hex (e.g. `0100000000010000`) |
| `sensor` | `Charger Type` | Detected charger power rate (Diagnostic) | *Disabled by default* | `unconnected`, `enough_power`, `low_power`, `not_supported` |
| `sensor` | `Battery Voltage` | Real-time battery voltage (Diagnostic) | *Disabled by default* | `mV` |
| `sensor` | `Battery Temperature` | Battery temperature (Diagnostic) | *Disabled by default* | `°C` |
| `sensor` | `Battery Health` | Battery capacity health percentage (Diagnostic) | *Disabled by default* | `%` |
| `sensor` | `SoC Temperature` | Internal processor temperature | *Disabled by default* | `°C` |
| `sensor` | `Uptime` | Console system uptime | *Disabled by default* | `s` |
| `sensor` | `WiFi Signal Strength` | WiFi RSSI connection strength | *Disabled by default* | `dBm` |
| `sensor` | `SD Card Free Space` | Available storage space on SD card | *Disabled by default* | `MB` |
| `sensor` | `SD Card Total Space` | Total storage capacity of SD card | *Disabled by default* | `MB` |
| `sensor` | `Internal Storage Free Space` | Available storage space on NAND (User Partition) | *Disabled by default* | `MB` |
| `sensor` | `Internal Storage Total Space` | Total capacity of internal NAND (User Partition) | *Disabled by default* | `MB` |
| `sensor` | `RAM Used` | Current system memory usage | *Disabled by default* | `MB` |
| `number` | `Screen Brightness` | Controls display brightness level | **Enabled** | `0` – `100` (`%`, step `5`) |
| `switch` | `Screen` | Controls display backlight state (on/off) | **Enabled** | `ON` / `OFF` |
| `sensor` | `Screen Brightness Sensor` | Diagnostic display brightness reading | *Disabled by default* | `%` (0–100) |
| `binary_sensor` | `Screen Backlight Sensor` | Diagnostic screen backlight status | *Disabled by default* | `ON` / `OFF` |
| `sensor` | `Volume` | System audio volume | *Disabled by default* | `%` (0–100) |
| `sensor` | `Audio Output Target` | Active audio endpoint | *Disabled by default* | `speaker`, `headphone`, `tv`, `usb`, `bluetooth` |
| `sensor` | `Player 1–8 Controller` | Controller model per player slot | *Disabled by default* | `handheld`, `joydual`, `joyleft`, `joyright`, `pro`, `gamecube`, `snes`, `n64`, `genesis` |
| `binary_sensor` | `Player 1–8 Controller Wired` | Wired/docked or wireless status per player slot | *Disabled by default* | `ON` (wired) / `OFF` (wireless/none) |
| `sensor` | `Screen Stream URL` | RTSP live video stream URL | *Disabled by default* | `rtsp://<switch_ip>:8554/` |

## Remote Control, Media Player & Game Launching

### Game Launcher & Channel Selection
All installed games are automatically enumerated by the sysmodule and exposed to Home Assistant:
- **`media_player` Source List**: Selecting a game in the media player's `source` dropdown immediately requests application launch on the Switch.
- **`remote` Activity List**: Selecting an activity launches that game.
- **`select` Entities**:
  - `select.nintendo_switch_game`: Dropdown of all installed games.
  - `select.nintendo_switch_2_player_games`: Filtered list for 2+ player games.
  - `select.nintendo_switch_4_player_games`: Filtered list for 4+ player games.
  - `select.nintendo_switch_8_player_games`: Filtered list for 8+ player games.

### Customizing Player Counts & Titles
Switch Assistant includes built-in keyword and title detection for player counts (2p, 4p, 8p+), but you can customize or override them at any time:
1. **`players.ini` / `players.txt`**: Create `sdmc:/switch/switch-ha/players.ini` or `players.txt`:
   ```ini
   0100000000010000=2
   Super Mario Odyssey=2
   Mario Kart 8 Deluxe=4
   Super Smash Bros Ultimate=8
   ```
2. **`titles.txt` (3rd column)**: In `sdmc:/switch/switch-ha/titles.txt`:
   ```text
   0100000000010000;Super Mario Odyssey;2
   0100152000022000;Mario Kart 8 Deluxe;4
   ```

### Remote Entity Commands
You can send commands using Home Assistant's `remote.send_command` service:

```yaml
action: remote.send_command
target:
  entity_id: remote.nintendo_switch_remote
data:
  command:
    - volume_up       # Increases volume
    - volume_down     # Decreases volume
    - volume_mute     # Mutes/unmutes audio
    - brightness_up   # Increases display brightness
    - brightness_down # Decreases display brightness
    - screen_off      # Turns off screen / sleep
    - screen_on       # Turns on screen / wake
    - reboot          # Reboots console
    - shutdown        # Powers off console
```

### Diagnostics & Remote Log Retrieval
Switch Assistant includes a full remote diagnostics and structured logging subsystem with real-time log export over MQTT:
- **Button Entity (`button.nintendo_switch_fetch_diagnostics`)**: Pressing this button in Home Assistant triggers immediate collection and publishing of current sysmodule diagnostics and recent logs.
- **Sensor Entity (`sensor.nintendo_switch_diagnostics`)**: Reflects console health state (`healthy`, `warning`, `error`) and exposes complete telemetry as JSON attributes (firmware version, Atmosphère version, MAC address, WiFi RSSI, battery stats, SoC temperature, RAM usage, storage space, and active service statuses).
- **MQTT Query Support**: You can publish to `switch_ha/<client_id>/command` or `switch_ha/<client_id>/command/get_logs` with optional parameters:
  ```json
  {"command": "get_logs", "level": "warn", "lines": 50}
  ```
  Supported `level` filters: `all`, `debug`, `info`, `warn`, `error`.
  The console immediately publishes structured diagnostic data and filtered log entries to `switch_ha/<client_id>/logs/response`.

### Media Player Entity
The media player entity automatically reflects whether the Switch is actively playing a game (`playing`), on the home menu (`idle`), or asleep (`off`). It displays the current game as the media title, renders game box cover art (`entity_picture`), exposes Title ID & player capabilities via attributes, and allows direct volume slider control as well as source/game switching from standard media control cards.

## Controlling Home Assistant Devices from an Overlay (UltraHand / Tesla / Shortcuts)

You can trigger actions and control devices (e.g. lights, switches, scenes) in Home Assistant directly from an UltraHand overlay, Tesla menu, or homebrew script while in-game.

When a command is triggered, Switch Assistant:
1. Calls the corresponding service / publishes the event to Home Assistant via MQTT.
2. Displays an in-game overlay toast confirming the action (e.g. `HA Action: Living Room Light (toggle)`).

### How to Trigger from UltraHand / Tesla / Scripts

Simply write a command file to `sdmc:/switch/switch-ha/overlay-command.ini`:

```ini
action=toggle
entity_id=light.living_room
name=Living Room Light
```

Or call any specific service:

```ini
action=call_service
domain=scene
service=turn_on
entity_id=scene.gaming_night
name=Gaming Mode
```

### Automations via MQTT Event Entity
Switch Assistant automatically exposes an `event.nintendo_switch_overlay_action` entity in Home Assistant. You can create Home Assistant automations triggered whenever an overlay action is performed:

```yaml
alias: "Switch Overlay: Toggle Lights"
trigger:
  - platform: state
    entity_id: event.nintendo_switch_overlay_action
action:
  - service: light.toggle
    target:
      entity_id: light.living_room
```

## How It Works

The project installs three components:

| Component | Purpose |
| --- | --- |
| `switch-ha.nro` | Homebrew app used for configuration, connection tests, and automatic installation. |
| Atmosphere sysmodule | Background process that publishes sensors and receives MQTT commands. |
| Notification overlay | Shows Home Assistant popups over the currently running game. |

The `.nro` app is only used for setup and configuration. After rebooting, the sysmodule handles the background MQTT work.

## Requirements

- Nintendo Switch running Atmosphere.
- Working Homebrew Menu.
- Home Assistant already installed.
- An MQTT broker reachable from the Switch, such as Mosquitto.
- A network where the Switch and MQTT broker can communicate with each other.

> Important: the sysmodule supports plain TCP MQTT, usually on port `1883`. It does not support TLS on `8883`, MQTT over WebSocket, or mDNS hostnames such as `homeassistant.local` for the MQTT broker.

## Before You Start

If you want to add a more polished and visual experience to your Home Assistant dashboard, check out this custom card built on top of Switch Assistant by *hudsonbrendon*:

https://github.com/hudsonbrendon/nintendo-switch-card

It provides a clean visual representation of your Nintendo Switch state, including controllers, battery, current game, and more, along with handy controls and notifications.

<p align="center"> <img src="https://github.com/user-attachments/assets/d30aed7e-9722-4464-852c-28144422cfd8" alt="Nintendo Switch Card preview" width="500"> </p>

## Quick Installation

1. Download `switch-ha.nro` from the latest [release](https://github.com/FaserF/switch-assistant/releases).
2. Copy the file to your SD card:

   ```text
   sdmc:/switch/switch-ha/switch-ha.nro
   ```

3. Launch `Switch Assistant` from the Homebrew Menu.
4. On first launch, the app creates the required folders and installs:

   ```text
   sdmc:/atmosphere/contents/00FF000053484101
   sdmc:/atmosphere/contents/00FF000053484102
   sdmc:/switch/switch-ha/switch-ha-overlay.ovl
   sdmc:/switch/switch-ha/config.ini
   ```

5. Enter your Home Assistant and MQTT settings.
6. Press `Y` to test the connection.
7. Press `-` to reboot the console.

After the reboot, Home Assistant should automatically discover the new MQTT entities.

## Configuration On The Switch

On the main screen:

| Button | Action |
| --- | --- |
| `D-Pad` | Select a field. |
| `A` | Edit the selected field. |
| `Y` | Test Home Assistant and MQTT. |
| `-` | Reboot the console to apply changes. |
| `+` | Exit the app. |

Fields to fill in:

| Field | Example | Notes |
| --- | --- | --- |
| `HA URL` | `http://192.168.1.10:8123` | Your Home Assistant URL. |
| `HA Token` | long-lived token | Your Home Assistant long-lived access token. |
| `MQTT Host IP` | `192.168.1.10` | Use the broker IP address, not `homeassistant.local`. |
| `MQTT Port` | `1883` | Do not use `8123`: that is the Home Assistant web port. |
| `MQTT Username` | `mqtt_user` | MQTT broker username. |
| `MQTT Password` | `password` | MQTT broker password. |
| `Discovery` | `homeassistant` | Standard MQTT Discovery prefix. |
| `Name` | `Nintendo Switch` | Device name shown in Home Assistant. |
| `Client ID` | `switch-ha-xxxxxxxx` | Generated automatically, editable if needed. |
| `Boot Delay` | `0` | Seconds to wait before the sysmodule starts system services. Use only if your setup crashes during boot. |
| `Notifications` | `YES` | Enabled by default. Disable and reboot if the notification overlay crashes your console. |

The Home Assistant token can be long. If entering it on the console is inconvenient, open the app once, then edit this file from your PC:

```text
sdmc:/switch/switch-ha/config.ini
```

After changing any configuration value, reboot the console so the sysmodule reloads the file.

## Create A Home Assistant Token

1. Open Home Assistant.
2. Open your user profile.
3. Scroll down to **Long-lived access tokens**.
4. Create a new token.
5. Copy it into the `HA Token` field or into `config.ini`.

## MQTT In Home Assistant

With MQTT Discovery enabled, Switch Assistant publishes discovery payloads under the configured prefix, usually:

```text
homeassistant
```

Console states are published under:

```text
switch_ha/<client_id>/...
```

Main entities:

| Type | Entities |
| --- | --- |
| Battery sensors | level, charging state, charger type, voltage, temperature, battery health |
| Console sensors | brightness, screen, volume, audio output target |
| Game | game running, current Title ID |
| Controllers | player count, Player 1-8 controller type |
| Commands | reboot, shutdown |
| Notifications | Home Assistant popup on the Switch |

## Live Streaming

The console supports live streaming while you are in-game.

To enable it, simply create a **Generic Camera** using the stream URL, which can be found in the entity **Screen Stream URL**. Make sure to disable SSL verification in the camera configuration for it to work correctly.
<p align="center">
  <img src="img/live_example.gif" alt="Home Assistant notification example on Nintendo Switch" width="900">
</p>

## Send A Notification To The Switch

<p align="center">
  <img src="img/notification_example.jpg" alt="Home Assistant notification example on Nintendo Switch" width="900">
</p>

Home Assistant creates a notify entity with a name similar to:

```text
notify.nintendo_switch_popup_notification
```

Example Home Assistant automation / script action:

```yaml
action: notify.send_message
target:
  entity_id: notify.nintendo_switch_popup_notification
data:
  title: "Laundry Done"
  message: "The washing machine has finished"
```

The notification entity automatically receives both the `title` and `message` fields via Home Assistant's standard notification platform.

You can also send custom notifications via `mqtt.publish`:

```yaml
action: mqtt.publish
data:
  topic: switch_ha/<client_id>/notify/popup
  payload: '{"title":"Doorbell","message":"Someone is at the front door"}'
```

The sysmodule writes the latest notification here:

```text
sdmc:/switch/switch-ha/notification-current.ini
```

and keeps a small log here:

```text
sdmc:/switch/switch-ha/notifications.log
```

The overlay reads these files and shows the popup automatically.

## Troubleshooting

### Home Assistant does not show any entities

- Make sure the MQTT integration is enabled in Home Assistant.
- Check that `Discovery` is set to `homeassistant`.
- Press `Y` in the app and confirm that the MQTT test succeeds.
- Reboot the console after saving the configuration.

### Wrong MQTT port

If you see a message like:

```text
Port 8123 is HA HTTP; MQTT is usually 1883
```

you are using the Home Assistant web port. MQTT usually runs on `1883`.

### TCP failed or broker/port closed

The Switch reached the broker IP, but the MQTT port is closed or not exposed.

Check that:

- Mosquitto or your MQTT broker is running.
- Port `1883` is exposed on your LAN.
- Firewall, VLAN, or client isolation rules are not blocking the Switch.
- The configured IP address is the MQTT broker address.

### The test works, but sensors do not appear

The test runs from the `.nro` app. Sensors are published by the sysmodule only after rebooting.

Press `-` in the app or reboot the console manually.

### Console crashes during boot

Some custom Atmosphere packs may crash if boot2 sysmodules start system services too early or if the notification overlay is not compatible with that custom CFW.

If the console fails to boot after installing Switch Assistant:

1. Power off the console.
2. Put the SD card in a PC.
3. Remove these folders if they exist:

   ```text
   sdmc:/atmosphere/contents/00FF000053484101
   sdmc:/atmosphere/contents/00FF000053484102
   ```

4. Put the SD card back in the console and boot normally.
5. Launch `Switch Assistant` from the Homebrew Menu.
6. Set `Notifications` to [NO].
7. Press `-` in the app.

- If it works, then the issue was caused by the notification overlay. You will not be able to use notifications anymore, but you can open a new issue with your console details and I will work on making them compatible.

- If you still have issues, sometimes it helps to delay the startup of Switch Assistant so the console has enough time to finish its boot routine. Re-do above steps from 1 to 5 and:

6. Set `Boot Delay` to `30` seconds.
7. Press `-` in the app.

- If the console still crashes, repeat the recovery steps and try a higher value such as `60` seconds.

`Boot Delay` is saved in:

```ini
startup_delay_seconds=30
```

The default is `0`, which keeps startup immediate on standard Atmosphere setups.

## Build From Source

Requirement:

- Docker Desktop installed and running.

From Windows:

```bat
scripts\build-docker.bat
```

The script:

- pulls `devkitpro/devkita64` if needed;
- builds the sysmodule;
- builds the notification overlay;
- builds the notification overlay loader;
- embeds the sysmodule and overlay into the app romfs;
- generates `switch-ha.nro`.

## Useful Paths

| Path | Contents |
| --- | --- |
| `sdmc:/switch/switch-ha/switch-ha.nro` | Homebrew app. |
| `sdmc:/switch/switch-ha/config.ini` | Configuration. |
| `sdmc:/switch/switch-ha/switch-ha-overlay.ovl` | Notification overlay. |
| `sdmc:/switch/switch-ha/sysmodule.log` | Sysmodule log. |
| `sdmc:/switch/switch-ha/sysmodule-heartbeat.txt` | Sysmodule diagnostic state. |
| `sdmc:/switch/switch-ha/notifications.log` | Short notification history. |
| `sdmc:/atmosphere/contents/00FF000053484101` | Switch Assistant sysmodule. |
| `sdmc:/atmosphere/contents/00FF000053484102` | Switch Assistant overlay loader. |

## Credits

- This project is a fork of the original [Switch Assistant](https://github.com/ErSeraph/switch-assistant) by [ErSeraph](https://github.com/ErSeraph). A huge thank you to ErSeraph for creating and maintaining the original project!
- The overlay loader is derived from the ISC-licensed `nx-ovlloader` project and chainloads the Switch Assistant overlay directly instead of Tesla Menu.

## License

Switch Assistant is licensed under the [MIT License](LICENSE).

The overlay loader includes code derived from `nx-ovlloader`, which is licensed under the ISC License.
