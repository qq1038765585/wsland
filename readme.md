# wsland

A custom Wayland GUI environment for WSL based on **wlroots** and **FreeRDP Server**.

## Overview

`wsland` is an experimental replacement solution for the original WSLg environment.

WSLg provides an excellent Linux GUI experience on Windows, but it has not received significant updates for a long time and still has some issues that affect daily usage.

The goal of `wsland` is to provide a more controllable and customizable GUI environment for Linux applications running inside WSL.

The project is based on:

- **wlroots** - Wayland compositor framework
- **FreeRDP Server** - RDP backend and RemoteApp support

Currently, `wsland` has reached a state that can satisfy my personal daily usage requirements.

## Project Status

`wsland` is currently distributed as a pre-built **WSL VHD image**.

This project does not provide source code.

The main purpose of this project is to provide a ready-to-use WSL GUI environment instead of a general-purpose compositor framework.

The provided VHD image contains:

- Wayland compositor environment
- XWayland support
- FreeRDP Server integration
- RemoteApp (RAIL) support
- Required runtime dependencies

## Future Direction

The current implementation is built directly on top of wlroots.

However, maintaining a complete Wayland compositor requires a significant amount of time and effort.

The future direction is to investigate whether existing mature Wayland compositors can be used as the window management layer.

Possible candidates include:

- Hyprland
- niri

The goal is to reuse existing compositor projects and focus on WSL integration and RDP/RemoteApp functionality.

Development progress may be slow because this project is maintained during personal spare time.

## Installation

### Requirements

- Windows with WSL support
- WSL version supporting custom distributions

### Install

1. Download the latest VHD package.

Example:

```
system.zip
```

2. Extract the downloaded archive.

After extraction, you should get:

```
system.vhd
```

3. Open **WSL Settings**.

4. Go to:

```
Developer
    -> Custom system distro
```

5. Select the extracted `system.vhd` file, example:
<img width="1284" height="791" alt="image" src="https://github.com/user-attachments/assets/b5912fb7-8602-40a0-a947-037b5c1040ff" />

6. run wsl --shutdown once, then open you user distro exec wayland / xwayland application.

Example:

```bash
google-chrome
```

The Linux GUI applications will be displayed through the windows desktop session.
<img width="1920" height="1080" alt="image" src="https://github.com/user-attachments/assets/0d85e430-75e8-4e93-aef9-b464b20e9ded" />


## Configuration

Optional configuration file:

```
C:\ProgramData\Microsoft\WSL\wsland.ini or C:\Users\[your user name]\wsland.ini
```

# Configuration

Wsland provides a configuration file for Synchronize display scaling, keyboard layout, window border appearance, and keyboard shortcuts.

The configuration file uses the INI format.

## Output Configuration

```ini
[output]
scale=false
```

### `scale`

Synchronize the display scale factor with Windows.

- `false` (default)
  - Use the default Linux display scale.
  - Provides better rendering quality.

- `true`
  - Synchronizes the Linux display scale with the Windows scaling setting.
  - Example:
    - Windows display scale: `125%` (`1.25x`)
    - Linux display scale: `1.25x`
  - Note:
    - The display quality may not be optimal when this option is enabled.

Example:

```ini
[output]
scale=true
```

---

## Keyboard Configuration

```ini
[keyboard]
layout=us
variant=
options=
```

Configures the keyboard layout using XKB settings.

### `layout`

Keyboard layout name.

- By default, Wsland automatically converts the Windows keyboard layout to the corresponding XKB layout.
- If a special keyboard layout is required, it can be manually specified here.
- Refer to XKB documentation for available layouts.

Example:

```ini
[keyboard]
layout=us
```

### `variant`

Keyboard layout variant.

Leave empty if no variant is required.

Example:

```ini
[keyboard]
layout=us
variant=intl
```

### `options`

Additional XKB keyboard options.

Example:

```ini
[keyboard]
options=caps:escape
```

---

## Border Configuration

```ini
[border]
width=2
radius=12
column-gaps=8
active-color=#0000FFFF
inactive-color=#00000000
```

Controls the appearance of window borders.

### `width`

The width of the focused window border.

Example:

```ini
width=2
```

---

### `radius`

The corner radius of the focused window border.

Example:

```ini
radius=12
```

---

### `column-gaps`

The gap between application with monitor.

This option controls the spacing between windows when applications are arranged in column-maximize mode.

Example:

```ini
column-gaps=8
```

---

### `active-color`

The border color of the active/focused window.

Color format:

```
#RRGGBBAA
```

Where:

- `RR` - Red channel
- `GG` - Green channel
- `BB` - Blue channel
- `AA` - Alpha channel

Example:

```ini
active-color=#0000FFFF
```

---

### `inactive-color`

The border color of inactive windows.

Color format:

```
#RRGGBBAA
```

Example:

```ini
inactive-color=#00000000
```

---

## Key Binding default Configuration

```ini
[binding]
Alt+Shift+Q=kill
Alt+Shift+C=center
Alt+Shift+F=column-maximize
```

Defines keyboard shortcuts for window management.

### Supported Modifier Keys

- `Ctrl`
- `Alt`
- `Shift`

### Supported Keys

- `A-Z`

### Supported Actions

| Action | Description |
| --- | --- |
| `kill` | Close the focus application |
| `center` | Display the focus application on screen center |
| `maximize` | Maximize the focus application |
| `fullscreen` | Fullscreen the focus application |
| `column-maximize` | Maximize the focus application column mode |

Example:

```ini
[binding]
Alt+Shift+Q=kill
Alt+Shift+C=center
Alt+Shift+F=column-maximize
```

The above configuration provides:

| Shortcut | Action |
| --- | --- |
| `Alt + Shift + Q` | Close the focus application |
| `Alt + Shift + C` | Display focus application to screen center |
| `Alt + Shift + F` | Maximize the focus application column mode |

---

## Window Mouse Operations

In addition to keyboard shortcuts, Wsland provides mouse-based window operations:

| Shortcut | Action |
| --- | --- |
| `Alt + Left Mouse Button` | Move the window |
| `Alt + Right Mouse Button` | Resize the window |

---

## Complete Configuration Example

```ini
[output]
scale=false

[keyboard]
layout=us
variant=
options=

[border]
width=2
radius=12
column-gaps=8
active-color=#0000FFFF
inactive-color=#00000000

[binding]
Alt+Shift+Q=kill
Alt+Shift+C=center
Alt+Shift+F=column-maximize
```

## Troubleshooting

### VHD Permission Issue

If there's this error: WsL/Service/CreateInstance/CreateVm/HCS/E_ACCESSDENIED

1. Locate the VHD file.

2. Open file properties.

3. Add the `Users` group.

4. Grant read and execute permissions.

5. Restart WSL and try again.

Example:
<img width="404" height="488" alt="image" src="https://github.com/user-attachments/assets/45c33d92-556c-4a3a-a154-834eb354938e" />

## Known Limitations

Because `wsland` is an experimental project, some limitations may exist:

- Hardware compatibility depends on the WSL environment.
- Some Wayland or XWayland applications may not work perfectly.
- Advanced desktop features are incomplete.
- Updates are not guaranteed.

## References

The project is based on or inspired by:

- wlroots  
  https://gitlab.freedesktop.org/wlroots/wlroots

- freerdp  
  https://github.com/FreeRDP/FreeRDP

- wslg  
  https://github.com/microsoft/wslg

- weston  
  https://gitlab.freedesktop.org/wayland/weston
