# FarQoiViewer

[Русская версия](readme-rus.md)

A Far Manager 3.x plugin for viewing QOI images in a native Windows window.

QOI images are decoded directly by the plugin. No PNG, WIC, or GDI+ image codecs are used.

## Features

- Press `F3` on a `.qoi` file to open it in the viewer.
- Supports standard QOI images with RGB and RGBA pixels.
- Supports QOIMT images with independently encoded strips and parallel decoding.
- Reads files and decodes images in a background thread, keeping the viewer responsive during loading.
- Preserves the image aspect ratio.
- Provides fit-to-window and 1:1 display modes.
- Supports zooming and panning.
- Composites transparent RGBA images over a checkerboard background.
- Displays diagnostic information when a file cannot be loaded.
- Uses a separate native Windows window with its own message loop.
- Leaves non-QOI files to Far Manager.

## Controls

| Key / Input | Action |
|-------------|--------|
| `0` | Fit image to window |
| `1` | Display at 100% (1:1) |
| `+` / `-` | Zoom in / out |
| Mouse wheel | Zoom in / out |
| Arrow keys | Pan the image |
| `Home` | Center the image |
| `Space` / Left mouse button | Toggle fit-to-window and zoom mode |
| `Esc` | Close the viewer |

## Open an image from the Far command line

Use the plugin command prefix `qoi:` followed by the image path:

```text
qoi: C:\path\to\image.qoi
qoi: "C:\path with spaces\image.qoi"
```

You can also select a `.qoi` file in a Far Manager panel and press `F3`.

## Build

### Requirements

- Windows
- [MSYS2 UCRT64](https://www.msys2.org/) with MinGW-w64 GCC
- CMake 3.20 or later
- Far Manager source code containing `far/plugin.hpp`

The project uses C++17 and the Far Manager SDK header from the source tree rather than a bundled copy.

Clone the Far Manager source:

```bash
git clone --depth 1 https://github.com/FarGroup/FarManager.git
```

For the build commands below, assume the Far Manager source is located at `C:/src/FarManager`. Change `FAR_SOURCE_DIR` to match your directory.

### Build with MSYS2 UCRT64

Run these commands from the UCRT64 terminal in the project directory:

```bash
cmake -S . -B build -G "MSYS Makefiles" \
  -DCMAKE_BUILD_TYPE=Release \
  -DFAR_SOURCE_DIR="C:/src/FarManager"

cmake --build build --clean-first --parallel
```

The resulting DLL is:

```text
build/FarQoiViewer.dll
```

## Install

Copy `FarQoiViewer.dll` into the plugin directory of your Far Manager installation, for example:

```text
%FARHOME%\Plugins\FarQoiViewer\FarQoiViewer.dll
```

If `%FARHOME%` is not defined on your system, use the corresponding path inside your Far Manager installation.

Restart Far Manager after installing or replacing the DLL.

## Notes

- This is a viewer plugin, not an archive or virtual-panel plugin.
- `F3` is intercepted only for `.qoi` files; other file types remain handled by Far Manager.
- QOIMT support is intended for files produced by a compatible QOIMT encoder using independently decodable strips.
- QOI format: [phoboslab/qoi](https://github.com/phoboslab/qoi).

## License

This project is licensed under the Apache License 2.0. See [LICENSE](LICENSE) for details.
