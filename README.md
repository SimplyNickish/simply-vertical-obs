# Simply Vertical for OBS

An OBS Studio plugin that draws a vertical (9:16) stream layout inside OBS, built from your own game capture and camera. You design the layout on [vertical.simplynickish.com](https://vertical.simplynickish.com); the plugin reads your saved layout when OBS starts and follows every change you make there, so the page only needs to be open while you edit, never while you stream.

Made by [SimplyNickish](https://simplynickish.com). Simply Vertical needs a SimplyNickish membership; this repository is the plugin's full source, published under the GPL like OBS itself.

## Install

Download the installer from the Go live tab on [vertical.simplynickish.com](https://vertical.simplynickish.com), close OBS, run it, then open OBS again. Windows may show a blue "Windows protected your PC" box: that is normal for small apps like this one and the installer is safe, so press More info, then Run anyway. The installer finds your OBS folder, keeps your sign-in and settings when updating, and asks you to close OBS if it is still open. Remove it any time from Windows Installed apps ("Simply Vertical for OBS").

Then, in OBS, open the Simply Vertical dock, press Link my account, and press Approve on the page that opens in your browser.

## Requirements

* Windows 10 or 11, 64-bit
* OBS Studio 31.1 or newer (tested on 33.0)

## Building from source

You need Visual Studio 2022 (Desktop development with C++) and CMake 3.28 or newer.

```
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

The first configure downloads the OBS sources and the prebuilt OBS dependencies listed in `buildspec.json`. The plugin lands in `build_x64/RelWithDebInfo/simply-vertical.dll`, with its files in `data/`.

A normal build talks to the live site. Adding `-DSV_DEV_BUILD=ON` makes a testing build that talks to the development database and a site running on this PC instead, and shows the testing options.

### Installer

The Windows installer is made with [NSIS](https://nsis.sourceforge.io) 3 from `installer/simply-vertical.nsi`. It packs a normal build from the `build_release` folder:

```
cmake --preset windows-x64 -B build_release
cmake --build build_release --config RelWithDebInfo
cd installer
makensis simply-vertical.nsi
```

## Licence

GNU General Public License v2.0 or later. See [LICENSE](LICENSE). Built on the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate).
