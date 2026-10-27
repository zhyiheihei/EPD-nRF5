## Development

> **Note:**
> - Recommended to use [Keil 5.36](https://img.anfulai.cn/bbs/96992/MDK536.EXE) or lower versions (if pack downloads fail, you can download from the group files)
> - The `sdk10` branch contains the legacy SDK code with a smaller Bluetooth protocol stack footprint, used to support 128K Flash chips (no longer updated)

Using the nRF51 version project as an example (`Keil/EPD-nRF51.uvprojx`), the project configuration has several `Targets`:

- `nRF51822_xxAA`: Used to compile 256K Flash firmware
- `flash_softdevice`: Used to flash the Bluetooth protocol stack (only needs to be flashed once)

For programmers, you can use J-Link or DAPLink (you can use [RTTView](https://github.com/XIVN1987/RTTView) to view RTT logs).

**Flashing:**

1. Erase all (if you can't flash after erasing in Keil, try using the programmer's host software to erase)
2. Switch to `flash_softdevice`, download the Bluetooth protocol stack, **download directly without compiling** (only needs to be flashed once)
3. Switch to `nRF51822_xxAA`, compile first then download

### Crystal Oscillator Configuration

By default, this project does not use an external low-speed crystal oscillator (frequency: `32.768kHz`), because not all boards have this oscillator. Boards without a low-speed oscillator cannot run firmware that enables the low-speed oscillator.
If your board has an external low-speed crystal oscillator, it's recommended to modify it to use the external oscillator, as this will make the clock timing more accurate. Here's how to modify it:

**nRF51**

Modify `main.c`:

```c
#define NRF_CLOCK_LFCLKSRC      {.source        = NRF_CLOCK_LF_SRC_XTAL,             \
                                 .rc_ctiv       = 0,                                 \
                                 .rc_temp_ctiv  = 0,                                 \
                                 .xtal_accuracy = NRF_CLOCK_LF_XTAL_ACCURACY_20_PPM}
```
**nRF52**

Modify `sdk_config.h`:

```c
#define NRF_SDH_CLOCK_LF_SRC 1
#define NRF_SDH_CLOCK_LF_RC_CTIV 0
#define NRF_SDH_CLOCK_LF_RC_TEMP_CTIV 0
#define NRF_SDH_CLOCK_LF_ACCURACY 7
```

### Building Firmware with GCC (Linux / macOS)

Firmware can also be built without Keil using the GCC cross toolchain, which additionally produces signed Bluetooth OTA packages:

1. Install `arm-none-eabi-gcc` (GCC 13 recommended), `nrfutil` 6.x, and `python3 -m pip install intelhex`. On NixOS/Nix, simply run `nix develop` in the project root to get everything ready.
2. `make -f Makefile.firmware` builds the firmware to `build/nrf52/EPD-nRF52.elf.hex`.
3. `make -f Makefile.firmware ota` additionally generates:

- `build/nrf52/EPD-nRF52-uc8179-vXX-ota.zip`: Bluetooth DFU package, can be sent to the device via the web interface or nRF Connect
- `build/nrf52/EPD-nRF52-full.hex`: merged softdevice + bootloader + firmware + DFU settings image, flash it once with a programmer (then further updates can go through OTA)
- `build/nrf52/EPD-nRF52-settings.hex`: bootloader DFU settings page

The OTA package version is read from `APP_VERSION` in `EPD/EPD_service.h` automatically.

> **Note:** OTA packages must be signed with the matching private key (`tools/priv.pem`). nrfutil must be version 6.x, the 8.x rewrite removed the legacy `pkg generate` / `settings generate` commands.

### Emulator

This project provides an SDL2-based emulator that runs the GUI code on Windows, Linux and macOS, allowing you to view the effects after modifying the GUI code without downloading to the microcontroller.

Emulation screenshot:

![](images/4.jpg)


**Compiling:**

- Windows: install [MSYS2](https://www.msys2.org), open the `MSYS2 MINGW64` command window and run `pacman -Syu mingw-w64-x86_64-gcc mingw-w64-x86_64-SDL2 mingw-w64-x86_64-pkg-config`
- Linux/macOS: install `gcc`, `make`, `pkg-config` and SDL2 via the system package manager (NixOS/Nix users: `nix develop`)

Then `cd` to the project directory and execute `make` to build `build/emu/emulator`.

**Usage:** run `./build/emu/emulator` (space switches clock/calendar mode, `R` toggles red/black/white, `W` changes the week start, arrow keys adjust date/month). Running `./build/emu/emulator out.ppm` renders a frame to a PPM image headlessly and exits, which is also used as a CI smoke test.

**Host-side GUI tests:** `make -f Makefile.test test` compiles the pure formatting helpers
(e.g. `GUI_FormatScheduleLine`) with the host `gcc` and asserts their output — no SDL, no device.
`tests/render_schedule_panel.sh` renders the dashboard headlessly (SDL dummy driver), crops the
schedule panel and leaves a PNG for layout review.

**Modifying the GUI:**

After modifying the code in the GUI directory, simply re-execute the make command above to compile.

> **Note:** Code in the GUI directory must not depend on platform-specific things, such as microcontroller-specific API interfaces, otherwise compilation on Windows will fail. The correct approach is: calculate the data and put it in `gui_data_t` before calling the `DrawGUI` function, then pass it in through the `data` parameter.
