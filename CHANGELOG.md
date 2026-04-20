# Changelog

## v1.3

- Fix saved video recordings showing a green screen by excluding the notification overlay from the recording layer stack.
- Resolve the current game name from a bundled title database.
- Install the bundled title database alongside the Switch Assistant config files.

## v1.2

- Fix Switch Assistant title IDs to avoid collisions with existing homebrew sysmodules.
- Safely clean up old Switch Assistant installs only when identified by project markers.
- Improve notification text handling for Unicode payloads.
- Prevent old notifications from being shown again on boot.
- Add a configurable Boot Delay option for custom Atmosphere packs that crash when boot2 sysmodules start system services too early.

## v1.1

- Publish active game title ID and app running state sensors.
- Add Home Assistant popup notifications.

## v1.0

- Initial release.
