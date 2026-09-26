# FarQoiViewer

[Русская версия](readme-rus.md)

A Far Manager 3.x plugin that opens `.qoi` images in a native Windows window.

The QOI stream is decoded directly. No PNG, WIC, or GDI+ image codecs are used.

## Features

* `F3` on a `.qoi` file opens the viewer
* Non-QOI files are left to Far Manager
* RGB and RGBA QOI supported
* Aspect ratio preserved
* Fit-to-window and 1:1 modes
* Zoom and pan
* RGBA composited over a checkerboard background
* Independent window with its own message loop

## Controls

| Key | Action |
|-----|--------|
| `0` | Fit to window |
| `1` | 100% / 1:1 |
| `+` / `-` | Zoom in / out |
| Mouse wheel | Zoom |
| Arrow keys | Pan |
| `Home` | Center image |
| `Space` / LMB | Toggle fit ↔ zoom |
| `Esc` | Close |

Command line (Far prefix):

```
qoi C:\path\to\image.qoi
qoi "C:\path with spaces\image.qoi"
```

## Build

The plugin uses `far/plugin.hpp` from the Far Manager source tree (not a bundled copy).

```
git clone --depth 1 https://github.com/FarGroup/FarManager.git
```

### MSVC

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
  -DFAR_SOURCE_DIR=C:/src/FarManager

cmake --build build --config Release
```

DLL: `build/Release/FarQoiViewer.dll`

### MinGW (MSYS2 UCRT64)

```
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAR_SOURCE_DIR=/c/src/FarManager

cmake --build build -j
```

DLL: `build/FarQoiViewer.dll`

## Install

Copy the DLL to:

```
%FARHOME%\Plugins\FarQoiViewer\FarQoiViewer.dll
```

Restart Far Manager.

## Notes

* Uses `ProcessConsoleInputW` to intercept `F3` only for `.qoi` files
* Not an archive / virtual-panel plugin — it is a viewer for the current selection
* QOI format: [phoboslab/qoi](https://github.com/phoboslab/qoi)
