<p align="center">
  <img src="https://raw.githubusercontent.com/UnknuwProjects/Black-Android/main/app/src/main/res/mipmap-xxxhdpi/ic_launcher.png" width="96" alt="Black Android Logo">
</p>

<p align="center">
  <code><b>[ B L A C K : A N D R O I D ]</b></code><br>
  <code>REL-{{VERSION}} // STABLE BUILD</code>
</p>

<p align="center">
  An unofficial native Android port of the classic Xbox title <b>Black</b>,<br>
  recompiled via <a href="https://github.com/TGP482/XboxXBE-Recomp">XboxXBE-Recomp</a> and rendered with Vulkan.
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Android-10%2B-3DDC84?style=flat-square&logo=android&logoColor=white" alt="Android 10+">
  <img src="https://img.shields.io/badge/Architecture-arm64--v8a-blue?style=flat-square" alt="arm64-v8a">
  <img src="https://img.shields.io/badge/Graphics-Vulkan_1.1-AC162C?style=flat-square&logo=vulkan&logoColor=white" alt="Vulkan 1.1">
  <img src="https://img.shields.io/badge/Core-SDL_3-1D4F91?style=flat-square" alt="SDL3">
</p>

---

> [!IMPORTANT]
> **No Game Assets Included**  
> This release contains **no copyrighted game assets**. You must provide your own legally obtained copy of *Black* (Xbox Original USA or Europe release).

---

## What's New in {{VERSION}}

* **Suppressor menu rework:** The weapon list is now split into two groups.
  * **Supported Suppressor:** the ten weapons with a real in-game suppressor (AK-47, G36C, Glock, M16, M16A2 + M203, MAC-10, MP5, P90, Pistol, Uzi). One entry per weapon, spawns suppressed, and Y attaches or removes it as in the original game.
  * **Unsupported Suppressor (Experimental):** Grenade Launcher, M249, Magnum, RPG, Remington 870, SPAS 12 and Walther 2000, each with a plain and a permanently suppressed variant.
* **Silenced audio on all variants:** Suppressed variants play the silenced fire sound. M249, Magnum, Remington 870, SPAS 12 and Walther 2000 also change their muzzle flash. RPG and Grenade Launcher are audio only (marked with *).
* **Infinite Medkits:** New cheat under Player. The medkit count stays at maximum, and the setting is saved with the other cheats.
* **Panel polish:** Clearer group headings, shorter labels, and no more clipped text in the weapon list.
* **Indonesian translation:** The new panel text is translated.

---

## Download

| File | Size | SHA-256 |
| :--- | :--- | :--- |
| [`{{APK}}`](https://github.com/UnknuwProjects/Black-Android/releases/download/{{TAG}}/{{APK}}) | {{SIZE}} | `{{SHA256}}` |

---

## Installation

1. Install `{{APK}}` on your device.
2. Launch the app and grant **All Files Access** when prompted.
3. Place your game files (either a disc image `.iso` or `default.xbe` with its data folders) in one of:
   * `/storage/emulated/0/Black/`
   * A `Black` folder at the root of your SD card
4. The game detects the files and boots automatically.

> [!TIP]
> If All Files Access is denied, data is stored in `Android/data/com.black.unofficial/files/assets/`.

---

## Features

| Category | Options |
| :--- | :--- |
| Graphics | Render resolution 20–200%, FOV 45–140°, viewmodel FOV, FMV scale, unlocked framerate, V-Sync, FPS counter, anisotropic filtering, draw distance scaling, toggleable reload blur |
| Touch Controls | Layout editor, touch sensitivity, gyro aiming (device sensors, DualShock 4, DualSense) |
| Input | SDL3 controller support with rumble, physical keyboard, captured mouse mapping |
| General | Multiple languages (including Indonesian), intro skip, audio output routing, update check (manual and at startup), background auto-pause |
| Cheats | Infinite ammo, grenades and medkits, rapid fire, no recoil, god mode, one-hit kill, ESP lines, aimbot, game speed, mission and Black Ops unlock, weapon menu with suppressed variants |

*In-game settings are in the game's native options menu. Use the Android back button to open the pause menu.*

---

## System Requirements

- **OS:** Android 10 or newer (64-bit arm64-v8a)
- **GPU:** Vulkan 1.1 compatible (Adreno 6xx+, Mali-G7x+, Xclipse)
- **Memory:** 4 GB RAM minimum
- **Storage:** 2 GB free internal storage

---

## Credits

Port maintained by **UnknuwProjects**, based on the original Android adaptation by [RanggaCasper](https://github.com/RanggaCasper/Black-Android). Built on [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) by **TGP482**, with [SDL](https://github.com/libsdl-org/SDL), [glslang](https://github.com/KhronosGroup/glslang), [toml++](https://github.com/marzer/tomlplusplus), and [Dear ImGui](https://github.com/ocornut/imgui).

---
