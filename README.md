# Square SDR

A pocket controller for the **Square SDR 2** (a Hermes-Lite-2-style HF radio), running on the **Cardputer Zero**. Tune 0–30 MHz, watch the spectrum and waterfall, listen on USB, LSB, AM or CW, and transmit voice or CW, all from the little keyboard.

It talks to the radio over Ethernet using OpenHPSDR Protocol 1.

## Using it

| Key | What it does |
| --- | --- |
| `Z` / `C` (or ←/→) | Tune down / up |
| `4` `5` `6` `7` | Band / mode / filter bandwidth / tuning step |
| `N` | Auto notch on/off (kills steady tones and heterodynes) |
| `R` | RIT on/off (off clears it). While on, `F` / `X` shift it up / down in 50 Hz steps |
| **`Tab`** (or `Space`) | Push to talk, hold to transmit |
| `8` | Settings |
| `U` / `I` | Dark / light theme |
| `Fn+A` / `Fn+S` / `Fn+D` | Mute / volume down / volume up |
| `H` | Help popup |
| `Esc` | Back, or quit |

In Settings: `F` / `X` (or ↑/↓) pick a row, `Z` / `C` (or ←/→) change the value, `Esc` goes back.

The top bar shows an LED-style **S meter** while receiving and a **mic level** bar while transmitting (it goes red if you're clipping). The status area shows `CLIP` for ADC overload, `R+150` when RIT is active and `NOTCH` when the notch is on.

RIT shifts only what you hear. Transmit always stays on the dial frequency.

### Settings

TX Drive, Mic Gain (60% is a good start), Sidetone Pitch, TX Low Cut, PTT Key (Tab or Space) and Radio IP. Everything is saved between runs.

## Transmitting, safely

Transmit is on, so please be sensible:

- Only works inside the UK amateur allocations. Elsewhere you'll see `RX ONLY`.
- Drops automatically when you retune, after 3 minutes, or if SWR protection trips.
- Always released when the app exits.
- **Test on a dummy load first.** You need to hold the appropriate licence to put a signal on air.

Voice modes use a microphone (the built-in one or a USB headset) with automatic levelling. The Cardputer's speaker mutes while you're keyed to stop feedback. In CW mode, holding the key sends a carrier with a sidetone.

**Plug in a USB headset before launching the app.** Plugging it in while the app is running can upset the audio.

If you use a USB headset for both mic and earphones, copy `config/51-square-sdr-usb-duplex.conf` into WirePlumber's config so both directions work together.

## Install on the device

Build the package and push it over SSH:

```shell
REMOTE_USER=josh REMOTE_HOST=192.168.1.23 ./deploy.sh
```

Then install it on the device with `sudo dpkg -i "Square SDR_*_arm64.deb"` and relaunch.

## Building

You need CMake and a C++ toolchain.

- **Cardputer Zero (cross build from macOS/Linux):** install an aarch64 cross compiler (`crossbuild-essential-arm64` on Debian, `brew install aarch64-unknown-linux-gnu` on macOS), then `cmake --workflow --preset cp0-cross-package`. The `.deb` lands in `dist/`.
- **Desktop simulator:** `cmake --preset <name>` then `cmake --build --preset <name>`, using `linux-x86-64`, `darwin-arm64`, `darwin-x86-64`, `win32-msvc` or `win32-mingw64`. It can run offline or against a live radio.
- **Tests:** `ctest -C Debug` from the build directory.

Code layout: `src/app` (wiring and radio worker), `src/model` and `src/viewmodel` (state), `src/view` (LVGL screens), `src/platform` (input, audio, SDR protocol and DSP).

## Good to know

- The S meter is approximate and uncalibrated. PA temperature, current, power and SWR readings are estimates too, and SWR needs at least 2 W forward power.
- The waterfall colours are a display scale, not a measurement.
- Crossing a UK allocation edge plays a short tone.

## Licence

MIT. See [LICENSE](LICENSE).
