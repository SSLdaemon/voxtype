# KDE Listening Overlay

This directory contains the current known-good version of the KDE/Plasma Wayland listening overlay built around `voxtype`.

The overlay is a separate helper executable. It watches Voxtype's runtime state file and shows a slim equalizer-style HUD only while recording.

## Current Behavior

- visible only while recording
- hidden while idle
- hidden while transcribing
- anchored above the Plasma bottom-right clock/date area
- stacked above normal windows
- non-interactive, so it does not steal focus from the active text field

## Important Dependency

The final reliable implementation depends on:

- Qt6 Widgets
- LayerShellQt

The `LayerShellQt` dependency is what made the final Wayland placement and focus behavior stable on KDE Plasma.

## Files

- [`CMakeLists.txt`](CMakeLists.txt): build definition
- [`src/main.cpp`](src/main.cpp): overlay implementation
- [`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md): detailed engineering history
- [`examples/`](examples): sanitized launcher, config, and user-service examples from the tested setup

Notable example files:

- `examples/systemd/*.template`: cleaner publishable user-unit templates
- `examples/systemd/*.reference`: lightly sanitized copies of the tested-machine service units

## Build

From the repo root:

```bash
cmake -S extras/kde-listening-overlay -B build-overlay
cmake --build build-overlay -j4
```

Or directly from this subdirectory:

```bash
cmake -S extras/kde-listening-overlay -B build-overlay
cmake --build build-overlay -j4
```

## Local Install Example

```bash
install -Dm0755 build-overlay/voxtype-listening-overlay ~/.local/bin/voxtype-listening-overlay
```

The engineering history, runtime assumptions, and remaining limitations are documented in [`IMPLEMENTATION_NOTES.md`](IMPLEMENTATION_NOTES.md).
