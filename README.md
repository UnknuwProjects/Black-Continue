<div align="center">

# Black for Android

**A native Android port of the original Xbox version of _Black_.**

Touch controls · Gyroscope aiming · Controller support · Vulkan rendering

[Source Repository](https://github.com/RanggaCasper/Black-Android) · [Releases](https://github.com/RanggaCasper/Black-Android/releases) · [Gameplay Video](https://youtu.be/x5bczwTuyhw)

</div>

---

## Overview

Black for Android brings the original Xbox version of _Black_ to ARM64 Android devices. The game's x86 code is statically recompiled to C++ using [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp). Rendering uses Vulkan, while SDL3 handles windowing, input and audio.

This is a native port, not an emulator.

> **Game files are not included.** You must provide the data from your own legally acquired copy of the game.

<div align="center">

[![Gameplay video](https://img.youtube.com/vi/x5bczwTuyhw/maxresdefault.jpg)](https://youtu.be/x5bczwTuyhw)

<sub>Gameplay on POCO F6 · Maximum graphics settings · Gyroscope aiming</sub>

</div>

## Installation

1. Download and install the APK from the [Releases page](https://github.com/RanggaCasper/Black-Android/releases).
2. Launch **Black Android** and grant **All files access** when prompted. On Android 10, grant the storage permission instead.
3. Copy the game files to one of these locations:
   - `/storage/emulated/0/Black`
   - A folder named `Black` at the root of your SD card.
4. Use either the USA or Europe release. The game can load a disc image (`.iso`) or `default.xbe` together with its data folders.
5. Launch the app. It will start the game once the required files are detected.

If All files access is not granted, use this alternative location:

```text
Android/data/com.racas.black/files/assets
```

Settings (`black.toml`, `android.toml`, `layout.toml`), save data (`hdd`), logs and shader cache are stored in:

```text
Android/data/com.racas.black/files
```

## Features

Options are available from the game's settings menu. Each category has its own page, and the **Page** row in the pause menu switches between pages. The Android back button opens the pause menu.

### Graphics

| Option | Description |
| --- | --- |
| Render Resolution | 20%–200% of screen height; 67% by default. |
| Field of View | Gameplay horizontal FOV from 45° to 140°; default is 70°. |
| Viewmodel FOV | Adjusts the first-person weapon and arms' field of view. |
| FMV Scale | Choose between filling the screen or preserving the original 4:3 aspect ratio. |
| V-Sync | Can be enabled or disabled; off by default. |
| Maximum Frame Rate | Unlimited, display refresh rate, or a custom limit. Removes the original Xbox 30 FPS cap. |
| FPS Counter | Displays the current frame rate. |
| Anisotropic Filtering | Applies texture filtering across the game. |
| Draw Distance | Choose 2x, 4x or 8x the game's original level of detail and draw distance. |
| Reload Blur | Toggle the blur effect during reloads. |

### Touch Controls

- A movement stick is placed on the left side of the screen; dragging on the right side controls the camera.
- On-screen buttons provide the game's main actions. Dragging from the fire button also allows camera movement while firing.
- Touch controls are hidden during loading screens and cutscenes.
- Menu navigation uses a D-pad and back button. Tap an arrow to move one row, or hold it to continue.
- Touch sensitivity is adjustable independently of mouse sensitivity.
- Layout presets include **Default**, **3 Finger** and **4 Finger**. Button size and opacity can also be adjusted.
- Button styles: **Xbox**, **Clean** and **Classic**.
- Every control can be repositioned. Select a button and use **− / +**, or pinch with a second finger, to resize it.
- Gyroscope aiming works with the phone and, on Android 12 or newer, supported DualShock 4 and DualSense controllers. It can be set to work continuously or only while firing.

### Controller, Keyboard and Mouse

- SDL3-compatible controllers are supported, including rumble.
- Touch buttons are hidden while a controller is in use.
- Controller stick deadzone can be adjusted from 0% to 50%, with sensitivity from 0.1 to 3.0.
- Keyboard and mouse input is supported, with rebindable keys, mouse sensitivity and inverted look.
- The mouse is captured during gameplay, allowing the camera to continue moving beyond the screen edges.

### General

- Languages: English, French, German, Italian and Spanish where available on the disc, plus Indonesian for the port's own text.
- Skip intro logos.
- Audio output: automatic, mono, stereo or 5.1 surround.
- Check for updates automatically when the app starts, or manually from the settings.
- Pause the game and audio when the app moves to the background.

### Cheats

- **Weapons:** infinite ammo, infinite grenades, rapid fire (2x–4x) and no recoil.
- **Player:** god mode and one-hit kills.
- **Game:** adjustable game speed from 25% to 200%.

## System Requirements

| Component | Requirement |
| --- | --- |
| Android | Android 10 or newer |
| Architecture | ARM64 |
| Graphics | Vulkan 1.1-compatible GPU |
| Memory | 4 GB RAM |
| Storage | 2 GB for game files and saves |

Examples of supported GPU families include Adreno 6xx and newer, Mali-G7x and newer, and Xclipse. Drivers without dynamic rendering or timeline semaphore support are also supported.

## Building from Source

A Windows PC is required to build the project.

### 1. Clone the repository

```bash
git clone https://github.com/RanggaCasper/Black-Android.git
cd Black-Android
```

### 2. Add the game files

Place `default.xbe` from the USA release in `assets/`, or place the USA disc image (`.iso`) in `assets/` or `iso/`. The build process extracts `default.xbe` from the disc image.

### 3. Install dependencies

- [Visual Studio 2022](https://visualstudio.microsoft.com/downloads/) with **Desktop development with C++**.
- [Android Studio](https://developer.android.com/studio). Install Android SDK Platform 34, NDK `28.2.13676358` and CMake `3.31.6` through SDK Manager.
- [Python](https://www.python.org/), required only when extracting `default.xbe` from a disc image.

### 4. Build

Run:

```bat
build.bat
```

The script builds the recompiler, recompiles the XBE into `generated/game`, then builds the Android APK with Gradle.

The first build compiles the game for ARM64 and may take around 30–90 minutes.

The release APK is generated at:

```text
app/build/outputs/apk/release/app-release.apk
```

To install it using ADB:

```bash
adb install -r app/build/outputs/apk/release/app-release.apk
```

## Credits

- **[RanggaCasper](https://github.com/RanggaCasper)** — original Android port, project development and maintenance. Contributions include Indonesian language support, Mali graphics compatibility, controller input improvements, and additional gameplay and settings features.
- **[TGP482](https://github.com/TGP482/XboxXBE-Recomp)** — XboxXBE-Recomp, including the static recompiler and portable runtime used by this project.
- **[bagasromadon](https://github.com/bagasromadon)** — touch-control button skin.
- **[SDL](https://github.com/libsdl-org/SDL)**, **[glslang](https://github.com/KhronosGroup/glslang)**, **[toml++](https://github.com/marzer/tomlplusplus)** and **[Dear ImGui](https://github.com/ocornut/imgui)** — libraries used by the project.
- **[xemu](https://github.com/xemu-project/xemu)** and **[Cxbx-Reloaded](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded)** — references for Xbox hardware and API behaviour used by XboxXBE-Recomp.

## Disclaimer

_Black_ is a trademark of its respective owners. This project is an unofficial community project and is not affiliated with, endorsed by, or sponsored by the trademark owners.
