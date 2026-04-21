# Voxtype Listening Overlay: Implementation Notes

## Scope

This work does **not** change upstream `voxtype` core.

It adds a separate KDE/Plasma Wayland companion overlay that:

- watches Voxtype's state file
- shows a slim equalizer-style HUD only while recording
- stays above the Plasma panel in the bottom-right corner
- does not take keyboard focus
- does not intercept pointer input

The final working state described here was reached and verified on **April 21, 2026**.

## Final Result

The final overlay behavior is:

- visible only while Voxtype is recording
- hidden while idle
- hidden while transcribing
- pinned above the Plasma clock/date area in the bottom-right panel region
- stacked above normal windows
- non-interactive, so the active text field keeps focus

The user manually confirmed that this final behavior was working correctly.

## Tested Environment

This implementation was tested locally on:

- OS: Arch Linux
- Kernel: `6.18.22-1-cachyos-lts`
- Session: KDE Plasma on Wayland
- Plasma: `6.6.4`
- KWin: `6.6.4`
- Qt: `qt6-base 6.11.0-2.1`
- Layer shell library: `layer-shell-qt 6.6.4-1.1`

This matters because the final placement/focus fix depends on **Wayland layer-shell support**.

## Files Involved

### Repo files

- [`CMakeLists.txt`](CMakeLists.txt)
- [`src/main.cpp`](src/main.cpp)
- [`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md)

### Runtime files outside the repo

- `~/.config/voxtype-overlay/config.ini`
- `~/.config/systemd/user/voxtype-listening-overlay.service`
- `~/.config/systemd/user/voxtype.service`
- `~/.config/voxtype/config.toml`
- `~/.local/bin/voxtype-listening-overlay`
- `~/.local/bin/voxtype-toggle`
- `~/.local/bin/voxtype-cancel`
- `~/.local/share/applications/net.local.voxtype-toggle.desktop`
- `~/.config/kglobalshortcutsrc`

## Goal

The requested behavior was:

- slim, horizontal, equalizer-like visual
- visually light, not a bulky HUD
- shown above the Plasma clock/date area
- no taskbar entry
- no cursor or focus interference
- no need to click back into the text box after dictation starts

## Runtime Architecture

The current setup works like this:

1. `voxtype.service` runs `voxtype daemon` in the user session.
2. Voxtype writes its current state to the runtime state file.
3. `voxtype-listening-overlay.service` runs the overlay helper app.
4. The overlay polls the Voxtype state file and maps state to visibility.
5. KDE triggers recording through a global shortcut that launches `voxtype-toggle`.

Current state mapping:

- `recording` -> overlay visible
- `transcribing` -> overlay hidden
- `idle` -> overlay hidden
- `stopped` -> overlay hidden

Current state file path:

- `/run/user/1000/voxtype/state`

That path comes from Voxtype's `state_file = "auto"` behavior.

## Hotkey Integration

The final working setup does **not** rely on Voxtype's built-in evdev hotkey listener.

In `~/.config/voxtype/config.toml`:

- `hotkey.enabled = false`

Instead, KDE triggers recording via a desktop launcher and a global shortcut:

- desktop launcher: `~/.local/share/applications/net.local.voxtype-toggle.desktop`
- current KDE shortcut entry: `_launch=Meta+Control`

This means:

- Voxtype's daemon stays running
- KDE owns the shortcut
- the overlay only reacts to Voxtype state changes

## Implementation Timeline

### 1. Initial overlay app

The first working version added:

- a Qt6 Widgets overlay app
- a rounded translucent HUD
- animated bars
- polling of the Voxtype state file
- a separate user `systemd` service

This established the basic companion-app model instead of modifying Voxtype itself.

### 2. Initial placement logic

The first placement logic used screen geometry and available geometry to push the overlay toward the bottom-right corner.

This was enough for a first pass, but it was still a normal top-level window and therefore dependent on compositor behavior.

### 3. Focus-safe window hardening

To reduce interference, the overlay window was configured with:

- `Qt::WindowDoesNotAcceptFocus`
- `Qt::WindowTransparentForInput`
- `Qt::WA_ShowWithoutActivating`
- `Qt::WA_TransparentForMouseEvents`
- `Qt::NoDropShadowWindowHint`

This improved behavior, but it was still not the final fix under KDE Wayland.

### 4. Single-instance protection

A `QLockFile` guard was added so only one overlay instance can run at a time.

This prevented:

- duplicate overlays
- stale manually started processes from causing confusion
- overlapping bars after service restarts

### 5. Config cleanup and visual tuning

The overlay config was simplified and tuned for the requested look:

- `width=176`
- `height=24`
- `bar_count=14`
- `bar_spacing=4`
- `show_transcribing=false`
- `show_idle=false`

The config comments were also normalized to avoid parser warnings from KDE's INI handling.

### 6. Service-managed startup

The overlay was moved under a `systemd --user` service rather than being launched as a detached shell command.

This was important because the KDE user session already provides:

- `WAYLAND_DISPLAY`
- `DISPLAY`
- `XDG_RUNTIME_DIR`
- `DBUS_SESSION_BUS_ADDRESS`

That made startup after login more reliable.

### 7. Duplicate-process cleanup during debugging

During testing, a stray manually launched overlay process was found running outside the managed service.

That explained the "duplicate overlay" symptom.

After that was cleaned up, the single-instance model was kept as the permanent safeguard.

### 8. Regression after logout/login

After a logout/login cycle, the overlay service was running but the overlay itself was no longer visible.

The key symptom in the journal was:

`Failed to create popup. Ensure popup ... has a transientParent set.`

That failure came from a later local change that had switched the window type to `Qt::ToolTip`.

Under KDE/Plasma Wayland, `Qt::ToolTip` is treated like a popup surface and expects a transient parent.

This caused:

- the service to remain running
- the overlay process to exist
- the equalizer bar to never render

### 9. Popup regression fix

The tooltip/popup semantics were removed.

That resolved the "service is running but nothing appears" problem, but it still did not fully solve compositor-driven positioning and focus behavior.

### 10. Final fix: move to Wayland layer-shell

The decisive change was migrating the overlay to **LayerShellQt** on Wayland.

This is the change that made the overlay behave correctly as a real desktop HUD instead of a normal floating window.

The final layer-shell configuration does the following:

- uses `LayerShellQt::Window`
- places the surface in `LayerOverlay`
- anchors it to `AnchorRight | AnchorBottom`
- sets keyboard interactivity to `KeyboardInteractivityNone`
- disables activation on show
- sets desired size from the overlay widget size
- computes right/bottom margins from `QScreen::availableGeometry()`

That combination solved the two remaining user-facing problems:

- the overlay appearing in the center of the screen
- the overlay stealing focus from the active text field

### 11. Protocol-level verification

To confirm that the overlay was truly using layer-shell rather than only linking the library, it was run with `WAYLAND_DEBUG=1`.

That showed the overlay binding:

- `zwlr_layer_shell_v1`

This confirmed that the compositor was receiving a real layer-shell surface request.

### 12. Final visual verification

After the layer-shell change:

- the overlay appeared in the correct bottom-right position
- it stayed above the panel area as intended
- it no longer pulled focus away from the active input

The user then confirmed that it was "working perfectly."

## Current Configuration

Current overlay config:

```ini
[overlay]
screen=primary
width=176
height=24
right_margin=22
bottom_gap=8
corner_radius=12
bar_count=14
bar_spacing=4
show_transcribing=false
show_idle=false

[voxtype]
state_file=auto

[style]
background=#0e1318
border=#76f2de
recording=#76f2de
recording_peak=#f4fffd
transcribing=#f2b661
```

These values are calibrated for the current Plasma panel layout.

## Build And Install

Build:

```bash
cmake -S extras/kde-listening-overlay -B build-overlay
cmake --build build-overlay -j4
```

Install locally:

```bash
install -Dm0755 build-overlay/voxtype-listening-overlay ~/.local/bin/voxtype-listening-overlay
```

## Service Management

Restart overlay:

```bash
systemctl --user restart voxtype-listening-overlay.service
```

Check status:

```bash
systemctl --user --no-pager --full status voxtype-listening-overlay.service
```

Check recent logs:

```bash
journalctl --user -u voxtype-listening-overlay.service -n 50 --no-pager
```

## What Was Verified

The following were verified during development:

- the overlay builds cleanly with CMake and Qt6
- the installed binary starts from the user service
- the service survives login-session startup
- the overlay resolves the correct Voxtype state file
- duplicate overlay processes are prevented by `QLockFile`
- the old popup regression is gone
- the final binary links against `LayerShellQt` and `Qt6WaylandClient`
- protocol debug output confirmed binding to `zwlr_layer_shell_v1`
- the final user-session behavior was manually confirmed as correct

## Known Limitations

This implementation is calibrated for a KDE Plasma Wayland desktop and is not yet a general-purpose cross-desktop package.

Current limitations:

- placement is derived from screen available geometry and fixed margins, not the exact clock widget geometry
- the current docs and integration assume a user-level local install under `~/.local` and `~/.config`
- the final behavior depends on Wayland layer-shell support
- non-KDE desktops may need different shortcut integration and different panel calibration

## Recommendation For A Public Fork

If this is published, the safest structure is to keep it as an optional companion utility rather than mixing it directly into Voxtype core immediately.

Suggested repo layout:

- `extras/kde-listening-overlay/`
- `extras/kde-listening-overlay/src/main.cpp`
- `extras/kde-listening-overlay/CMakeLists.txt`
- `extras/kde-listening-overlay/README.md`
- `extras/kde-listening-overlay/config.ini.sample`
- `extras/kde-listening-overlay/voxtype-listening-overlay.service`

Recommended README points for a public fork:

- tested on Arch Linux
- tested on KDE Plasma 6 Wayland
- requires Qt6 and LayerShellQt
- designed as a Voxtype companion process, not a Voxtype core patch
- uses the Voxtype runtime state file
- expects user-level `systemd` startup

## Short Summary

What was built:

- a slim equalizer-style Voxtype listening overlay for KDE/Plasma Wayland
- a separate helper executable
- a user `systemd` service
- a config-driven placement and style system
- a single-instance guard

What had to be fixed along the way:

- loose initial placement
- duplicate overlay processes
- config parser warnings
- popup failure after logout/login
- centered overlay behavior
- focus stealing from the active text field

What made the final version work:

- non-activating and input-transparent window flags
- `QLockFile` single-instance protection
- service-managed startup
- migration to `LayerShellQt`
- bottom-right anchoring in the overlay layer
- keyboard interactivity disabled
- bottom/right margin calculation from available screen geometry
