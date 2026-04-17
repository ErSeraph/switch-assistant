# Switch Assistant

Switch Assistant is a Nintendo Switch homebrew app that connects your console to Home Assistant through MQTT.

The project has two parts:

- `switch-ha.nro`: the foreground configuration app.
- Atmosphere sysmodule: the background process that publishes sensors to MQTT.

The NRO is only used to configure the app, test the connection, and install or update the bundled sysmodule. The NRO does not publish sensors while closed. The sysmodule reads the saved configuration and runs in the background after a console reboot.

## Features

- Local configuration UI on the Switch.
- Automatic sysmodule installation from the NRO.
- Home Assistant MQTT discovery.
- Retained MQTT state publishing.
- Sensor updates when values change.
- Home Assistant and MQTT connection test from the NRO.
- Reboot and shutdown buttons exposed to Home Assistant.

Current sensors include:

- `Battery Level`
- `Is Charging`
- `Charger Type`
- `Battery Voltage`
- `Battery Temperature`
- `Battery Health`
- `Screen Brightness`
- `Screen`
- `Volume`
- `Audio Output Target`
- `Player Count`
- `Player 1 Controller` through `Player 8 Controller`

## First Setup

Copy the built NRO to your SD card:

```text
sdmc:/switch/switch-ha/switch-ha.nro
```

Launch `Switch Assistant` from the Homebrew Menu. On startup, the NRO automatically installs or updates the bundled sysmodule here:

```text
sdmc:/atmosphere/contents/0100000000000F12
```

On first launch, if no configuration file exists, the app creates:

```text
sdmc:/switch/switch-ha/config.ini
```

The default config uses:

- MQTT port `1883`
- MQTT discovery prefix `homeassistant`
- device name `Nintendo Switch`
- an auto-generated client ID like `switch-ha-xxxxxxxx`

After the first launch, you can either edit settings from the NRO UI or edit `config.ini` directly from a PC. Manual editing is only available after the NRO has been opened at least once, because that first run creates the config file and folders.

## Configuration

Use the D-Pad to select a field, press `A` to edit it, and save happens automatically after each edit.

Required fields:

- `HA URL`: your Home Assistant URL, for example `http://192.168.1.10:8123`.
- `HA Token`: a Home Assistant long-lived access token.
- `MQTT Host`: broker IP address only, for example `192.168.1.10`.
- `MQTT Port`: usually `1883`.
- `MQTT Username`: your broker username.
- `MQTT Password`: your broker password.
- `MQTT Discovery Prefix`: usually `homeassistant`.
- `Device Name`: default is `Nintendo Switch`.
- `Client ID`: auto-generated, but can be edited if needed.

The MQTT host should be a numeric IP address. Plain TCP MQTT 3.1.1 is supported. TLS on `8883`, mDNS hostnames such as `homeassistant.local`, and MQTT over WebSocket are not supported by the sysmodule.

Press `Y` in the NRO to run a one-time Home Assistant and MQTT connection test. The NRO only tests the connection; the background sensor publishing is done by the sysmodule after reboot.

When you change any configuration value, reboot the console so the sysmodule reloads the updated config. You can press `-` in the NRO to reboot.

## Home Assistant MQTT

State topics are published under:

```text
switch_ha/<client_id>/...
```

Home Assistant discovery topics use the configured discovery prefix, normally:

```text
homeassistant
```

Recommended MQTT settings are similar to HASS.Agent:

- Broker host: IP address of your MQTT broker.
- Port: `1883`.
- Username/password: your MQTT broker credentials.
- Discovery prefix: `homeassistant`.
- Client ID: unique for this Switch.

If the log says `TCP failed ... broker/port closed errno=111`, the Switch reached the broker IP but the TCP port was refused before MQTT authentication. In that case, the issue is not the username or password. Check that your broker is listening on LAN port `1883`, that the Home Assistant Mosquitto add-on exposes the port outside the container, and that firewall/VLAN/client isolation rules allow the Switch to reach the broker.

## UI Controls

- `D-Pad`: select field.
- `A`: edit selected field and save automatically.
- `Y`: test Home Assistant and MQTT connection.
- `-`: reboot the console to apply sysmodule/config changes.
- `+`: exit.

## Build

Requirements:

- Docker Desktop installed and running.

Build from Windows:

```bat
scripts\build-docker.bat
```

The script:

- checks that Docker is available;
- pulls `devkitpro/devkita64` if needed;
- builds the sysmodule;
- embeds the sysmodule into the NRO;
- builds the final `switch-ha.nro`.