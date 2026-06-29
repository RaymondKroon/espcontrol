# EspControl Desktop SDL

This target runs EspControl locally on a desktop computer using ESPHome's `host` platform and a 720×720 SDL window.

## Requirements

- ESPHome 2026.5.3 or newer
- SDL2 development headers (`libsdl2-dev` on Debian/Ubuntu, `sdl2` from Homebrew on macOS)
- A graphical desktop session

Check SDL with:

```bash
sdl2-config --libs --cflags
```

## Run

From this directory:

```bash
esphome run dev.yaml
```

Or from the repository root:

```bash
esphome run devices/desktop-sdl/dev.yaml
```

The SDL window is the touch display. Use the mouse as touch input.

## Configure EspControl

This target includes a small host-only companion web server because ESPHome's normal `web_server` component is not available on the `host` platform.

Open the EspControl setup page at:

```text
http://localhost:8080/
```

From another machine on the same network, use the desktop computer's IP address instead, for example:

```text
http://10.208.22.218:8080/
```

## Home Assistant

Home Assistant will not auto-discover ESPHome host-platform devices by mDNS. Add it manually with the desktop computer's IP address and ESPHome API port `6053`.
