# Table of contents
- [Building Retro-Go](#prerequisites)
- [Porting Retro-Go](#porting-retro-go)
- [IDE Support](#ide-support)


# Building Retro-Go

## Prerequisites
You will need a working installation of [esp-idf](https://docs.espressif.com/projects/esp-idf/en/release-v4.4/esp32/get-started/index.html#get-started-get-prerequisites). The automated ESP32-S3 builds use ESP-IDF **5.5.5**. Older ports support versions 4.4 onward, subject to their target configuration.

_Note: As of retro-go 1.44, I use 4.4.8. I used 4.3 for 1.35 to 1.43. ESP-IDF 4.1 was used for 1.20 to 1.34 versions._

### ESP-IDF Patches
Patching esp-idf may be required for full functionality. Patches are located in `tools/patches` and can be applied to your global esp-idf installation, they will not break your other projects/devices.
- `sdcard-fix`: This patch is mandatory for the ODROID-GO (and clones).
- `panic-hook`: This is to help users report bugs, see `Capturing crash logs` below for more details. The patch is optional but recommended.


## Obtaining the code

Using git is the preferred method but you can also download a zip from the project's front page and extract it if you want, Retro-Go has no external dependencies.

There are generally two active git branches on retro-go:
- `master` contains the code form the most recent release and is usually tested and known to be working
- `dev` contains code in development that will be merged to master upon the next release and is often untested

`git clone -b <branch> https://github.com/ashkan89/retro-go/`


## rg_tool.py
ESP-IDF's `idf.py` isn't capable of managing multiple applications in one project by itself (building a multi-app image, flashing to the correct partition, single build command, etc). `rg_tool.py` must be used instead, which passes correct arguments to `idf.py` and other tools.

For brevity, all the commands listed in this document omit additional flags that you will likely need, such as `--target` and `--port`. Those two flags can also be specified in the environment as `RG_TOOL_TARGET` and `RG_TOOL_PORT`, respectively.

Run `python rg_tool.py --help` to see all available flags and commands.


## Build everything and generate a firmware image:
- Generate a .fw file to be installed with odroid-go-firmware or odroid-go-multi-firmware from SD Card:\
    `python rg_tool.py build-fw` or `python rg_tool.py release` (clean build)
- Generate a .img to be flashed with esptool.py (Serial):\
    `python rg_tool.py build-img` or `python rg_tool.py release` (clean build)

For the complete image required by the on-device updater, include the factory app explicitly:

```powershell
$env:PROJECT_VER = "4.5"
python rg_tool.py --target esp32-s3-n16r8-ili9341 release factory launcher retro-core prboom-go gwenesis fmsx retro-legacy
```

`build-all.ps1` builds complete images for every target in `tools/release-targets.json`.
The version is embedded in each app's ESP-IDF descriptor and Retro-Go version string,
the image footer, and the output filename.

## Automated releases and device updates

After committing and pushing the desired release changes, push a stable version tag:

```sh
git tag -a v4.5 -m "Retro-Go v4.5"
git push origin v4.5
```

Tags accept `vMAJOR.MINOR` or `vMAJOR.MINOR.PATCH`, without leading zeroes or prerelease suffixes.
The Release workflow removes the `v` for the build version, runs tests, and builds the eight
ESP32-S3 configurations in `tools/release-targets.json` using ESP-IDF 5.5.5.
All images contain the factory updater, launcher, and all emulator apps.
The workflow checks targets, app versions, partition bounds, flash capacity, and image CRCs.
Publication waits for every build to succeed. It creates a draft titled **Retro-Go v4.5**,
uploads the `.img` assets, `SHA256SUMS`, and `release-manifest.json`, and publishes it as latest.
A failed upload leaves the draft unpublished; rerunning retries the upload. Already published
releases are protected from replacement. Use a new tag for corrections.

Release notes include GitHub's generated changelog and installation instructions. If
`CHANGELOG.md` has a matching `# Retro-Go 4.5` section, its contents are included too.
For tag `v4.5`, asset names are exactly `retro-go_4.5_<target>.img`; keep this convention because
devices use it to select their matching image. Regular branch/PR CI builds two representative
8 MB and 16 MB targets and runs the release tests without publishing.
GitHub's built-in workflow token supplies release permissions; no personal token is needed.

At launcher startup, once station Wi-Fi connects, a background task checks GitHub's latest
stable release. It retries transient failures up to three times, one minute apart, and draws
an upward-arrow update icon in the top-right status area when a newer matching image exists.
AP mode does not trigger an Internet check. A manual **Check for updates** always refreshes
the result, automatically selects the device's exact target image, and asks only to download
and reboot for installation. Older/same versions, prereleases, and incompatible assets are
excluded. Unknown development version strings cannot be compared; use `PROJECT_VER` for those builds.

Downloads require HTTPS certificate verification, reject unencrypted redirects and HTTP errors,
check the expected byte count and embedded release version, and verify image target and CRC before
preparing the factory updater. Failed or cancelled transfers are removed. A valid device clock
is needed for TLS; station Wi-Fi starts SNTP automatically. Devices with older firmware lacking
a factory partition need one complete image installed over USB first. Keep power connected
through installation; this multi-app updater does not provide automatic rollback after a power loss.

Local release checks:

```sh
python -m unittest discover -s tools/tests -v
cc -std=c99 -Wall -Wextra -Werror tools/tests/test_update_version.c -o test-update-version
./test-update-version
python tools/release.py package path/to/all/images v4.5
```

For a smaller build you can also specify which apps you want, for example the launcher + DOOM only:
1. `python rg_tool.py build-fw launcher prboom-go`

Note that the app named `retro-core` contains the following emulators: NES, PCE, G&W, Lynx, and SMS/GG/COL. As such, these emulators cannot be selected individually. The reason for the bundling is simply size, together they account for a mere 700KB instead of almost 3MB when they were built separately.

The `retro-legacy` app bundles ZX Spectrum 48K, Atari 2600, and Atari 7800.
It is included in default builds. See [its integration notes](retro-legacy/README.md)
for ROM folders, controls, source revisions, and implementation details.


## Flashing an image for the first time
Once we have successfully built an image file (`.img` or `.fw`), it must be flashed to the device.

To flash a `.img` file with `rg_tool.py`, run:
```
python rg_tool.py --target (target) --port (usbport) install (apps)
```

To flash a `.img` file with `esptool.py`, run:
```
esptool.py write_flash --flash_size detect 0x0 retro-go_*.img
```

To flash a `.fw` file:

Instructions depend on your device, refer to [README.md](README.md#installation).


## Build, flash, and monitor individual apps for faster development:
A full Retro-Go image must be flashed at least once (refer to previous section), but, after that, it is possible to flash and monitor individual apps for faster development time.

1. Flash: `python rg_tool.py --port=COM3 flash prboom-go`
2. Monitor: `python rg_tool.py --port=COM3 monitor prboom-go`
3. Flash then monitor: `python rg_tool.py --port=COM3 run prboom-go`


## Windows
Running `./rg_tool.py ...` on Windows might invoke the wrong Python interpreter (causing the build to fail)
or even do nothing at all. In such cases you should use `python rg_tool.py ...` instead.


## Changing the launcher's images
All images used by the launcher (headers, logos) are located in `themes/default`. If you edit them you must run the `tools/gen_images.py` script to regenerate `launcher/main/images.c`. Refer to [THEMING.md](THEMING.md) for details about the image format.


## Updating translations
When adding new menu items or on-screen messages, they should be translated. Retro-Go uses a system similar to gettext where, as a developer, your only task is to wrap your strings in `_(...)`. Refer to [LOCALIZATION.md](LOCALIZATION.md) to learn how to add the actual translations.


## Capturing crash logs
When a panic occurs, Retro-Go has the ability to save debugging information to `/sd/crash.log`. This provides users with a simple way of recovering a backtrace (and often more) without having to install drivers and serial console software. A weak hook is installed into esp-idf panic's putchar, allowing us to save each chars in RTC RAM. Then, after the system resets, we can move that data to the sd card. You will find a small esp-idf patch to enable this feature in tools/patches.

To resolve the backtrace you will need the application's elf file. If lost, you can recreate it by building the app again **using the same esp-idf and retro-go versions**. Then you can run `xtensa-esp32-elf-addr2line -ifCe app-name/build/app-name.elf`.



# Porting Retro-Go

## ESP32
Instructions to port to new ESP32 devices can be found in [PORTING.md](PORTING.md).

## Other architectures
I don't want to maintain non-ESP32 ports in this repository, but let me know if I can make small changes to make your own port easier! The absolute minimum requirements for Retro-Go are roughly:
- Processor: 200Mhz 32bit little-endian
- Memory: 2MB
- Compiler: C99 (and C++03 for handy-go)

Whilst all applications were heavily modified or even redesigned for our constrained needs, special care is taken to keep
Retro-Go and ESP32-specific code exclusively in their port file (main.c). This makes reusing them in your own codebase very easy!



# IDE Support

## VS Code
Retro-Go comes with a VS Code workspace file that contains configuration for the C/C++ extension.

To get intellisense working properly you have to define some paths in your *global* configuration file:

````json
    "retro-go.sdk-path": "C:/espressif/frameworks/esp-idf-v5.0.4",
    "retro-go.tools-path": "C:/espressif/tools/xtensa-esp32-elf/esp-2021r2-patch3-8.4.0/xtensa-esp32-elf",
````

Clangd is not supported at this time because I have not found a way to make it work well in our multi-folder workspace.
