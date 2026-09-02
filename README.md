# BlueZ Joy-Con 2 transport

Bluetooth LE transport support for Nintendo Switch 2 Joy-Con controllers on Linux.

This repository adds a Joy-Con 2 transport plugin directly to BlueZ and builds it as a **built-in `bluetoothd` plugin**.

The plugin uses BlueZ internal GATT APIs, so it is compiled together with the same BlueZ source tree that provides `bluetoothd`, rather than being loaded as an external `.so`.

## Architecture

BlueZ owns Bluetooth LE discovery and ATT/GATT communication.

The accompanying [`linux-switch2`](https://github.com/novakpetya/linux-switch2) kernel driver project owns controller initialization, protocol handling, calibration, input decoding, motion, rumble, and controller frontend creation.

Communication between the BlueZ plugin and kernel driver uses:

```text
/dev/switch2-gatt
```

The plugin:

- recognizes Joy-Con 2 Left (`057e:2067`) and Right (`057e:2066`) controllers;
- provides the fixed GATT layout required for reliable connection;
- configures the known-good BLE connection parameters;
- enables automatic reconnection after discovery;
- forwards controller commands and replies between BlueZ and the kernel;
- forwards native side input reports (`0x07` Left / `0x08` Right);
- switches to the common `0x05` input path after initialization;
- forwards rumble output to the appropriate BLE characteristic.

The plugin deliberately contains no controller input decoding or compatibility logic. Those responsibilities remain in the kernel driver.

## Why a BlueZ plugin is required

Joy-Con 2 uses a Nintendo-specific BLE GATT protocol rather than the normal Bluetooth HID profile.

Primary-service discovery works, but the controller does not reliably complete BlueZ's subsequent secondary-service discovery. The plugin therefore supplies the measured fixed GATT layout before the normal BlueZ GATT client is created.

## BlueZ version

The default build currently uses the BlueZ `5.87` release tag:

```sh
make
```

A different BlueZ Git ref can be selected explicitly:

```sh
make BLUEZ_REF=5.88
```

or for development:

```sh
make BLUEZ_REF=master
```

Using a release tag is recommended for normal installations.

## Requirements

You need the normal dependencies required to build BlueZ from source, including:

- a C compiler;
- GNU make;
- Git;
- Autotools;
- pkg-config;
- GLib and D-Bus development files;
- any optional development libraries required by the enabled BlueZ features.

Exact package names depend on your Linux distribution.

The accompanying [`linux-switch2`](https://github.com/novakpetya/linux-switch2) kernel driver is required for Joy-Con 2 operation over both BLE and USB.

## Build

```sh
make info
make
make check
```

BlueZ is downloaded into:

```text
.build/bluez
```

The build copies:

```text
switch2.c
```

to:

```text
plugins/switch2.c
```

and integrates it into BlueZ as a built-in plugin.

## Install

Build as a normal user:

```sh
make
```

then install:

```sh
sudo make install
```

Restart Bluetooth afterwards. On a systemd-based distribution:

```sh
sudo systemctl daemon-reload
sudo systemctl restart bluetooth
```

The default installation layout is:

```text
prefix        /usr
sysconfdir    /etc
localstatedir /var
libdir        /usr/lib
libexecdir    /usr/lib
```

Paths can be overridden when necessary.

For example:

```sh
make PREFIX=/usr/local LIBDIR=/usr/local/lib LIBEXECDIR=/usr/local/lib
sudo make PREFIX=/usr/local LIBDIR=/usr/local/lib LIBEXECDIR=/usr/local/lib install
```

### Distribution package warning

Installing BlueZ directly with `sudo make install` may replace files owned by your distribution's BlueZ packages.

Users who want package-manager integration should adapt the build for their distribution rather than installing directly over packaged files.

If you later use:

```sh
sudo make uninstall
```

and the installation overlapped distribution-owned files, reinstall your distribution's BlueZ packages afterwards.

## Connecting a Joy-Con 2

### First connection

Bluetooth discovery is only required the **first time** a Joy-Con 2 is used with the system.

Make sure the `linux-switch2` kernel driver is installed and the `hid_switch2` module is loaded.

Start normal Bluetooth discovery using your desktop environment's **Add New Bluetooth Device** interface, or use:

```sh
bluetoothctl
```

then:

```text
scan on
```

Press the **SYNC** button on the Joy-Con 2.

**Do not pair the controller.** Do not run `pair` and do not complete a pairing dialog.

Discovery is sufficient. The plugin recognizes the Joy-Con 2 advertisement, stores it for automatic reconnection, and establishes the BLE transport.

Once connected, discovery can be stopped:

```text
scan off
```

Repeat this first-time discovery procedure for the other Joy-Con if required.

### Subsequent connections

After a Joy-Con 2 has been discovered once, Bluetooth discovery is no longer necessary.

To reconnect it later, simply press its **SYNC** button.

The plugin will recognize the known controller and BlueZ will reconnect it automatically.

## Diagnostics

BlueZ/plugin messages:

```sh
sudo journalctl -fu bluetooth | grep --line-buffered 'switch2:'
```

A normal connection progresses through:

```text
Joy-Con 2 discovered
→ fixed GATT cache prepared
→ auto-connect enabled
→ kernel transport attached
→ controller initialized
→ common input subscribed
```

Kernel messages:

```sh
sudo journalctl -k | grep -i switch2
```

Input devices can also be inspected with:

```sh
grep -A8 -B2 -i 'Joy-Con' /proc/bus/input/devices
```

## Uninstall

To remove a direct source installation:

```sh
sudo make uninstall
```

If this installation replaced files belonging to your Linux distribution, reinstall its BlueZ packages afterwards.

## License

GPL-2.0-or-later.