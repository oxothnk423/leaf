---
title: LCD Preview Renderer Backlog
description: Future host-side pixel-exact renderer for Leaf LCD page layout iteration.
---

# LCD Preview Renderer Backlog

Display-only UI tweaks currently require building firmware, uploading, booting Leaf, and navigating menus before the layout can be reviewed. A host-side renderer would make iteration much faster, but it needs to be pixel-for-pixel identical to the Leaf LCD output to be useful for final layout decisions.

An initial approximate Python layout renderer was tried and then removed because it was not accurate enough: host-rendered text and placeholder QR codes did not match Leaf's u8g2 fonts, QR output, title rows, footer rows, or exact drawing behavior.

Future work:

- Build a pixel-exact host renderer that runs the actual Leaf page drawing code against a host-side u8g2 framebuffer.
- Link the real Leaf font arrays from `src/vario/ui/display/fonts.h`.
- Link the real QR implementation from `src/vario/utils/qrcodex.c`.
- Link the u8g2 core from `src/libraries/U8g2/src`.
- Provide small host mocks for Arduino APIs, WiFi state, settings, page stack globals, and any hardware/global objects touched by the page under test.
- Export the rendered `96x192` native framebuffer as PNG, plus an integer-scaled preview PNG for easier review.
- Start with `PageMenuSystemWifiWebApp` in Leaf-AP mode, then generalize to other pages once the harness is proven.
- Ensure the tool uses the same display rotation/orientation as firmware (`96x192` portrait for current Leaf hardware).

Assessment / pickup notes:

- This should be treated as a host-build harness project, not another approximate renderer. The useful path is to compile a small executable that links the real Leaf page drawing code, u8g2, `fonts.h`, and `qrcodex.c`, then exports the actual u8g2 framebuffer.
- A native host C/C++ toolchain is the first prerequisite. The checked Windows environment did not expose `cl`, `gcc`, `clang`, or `zig` on `PATH`. LLVM/Clang plus CMake/Ninja, or MSVC Build Tools, should be enough.
- The MVP should render one deterministic page state: `PageMenuSystemWifiWebApp` in Leaf-AP mode. That page exercises the important fidelity risks: title/footer rows, cursor inversion, real Leaf fonts, real QR generation, Leaf AP SSID/password strings, and the short web-app URL.
- The host compatibility layer will need small mocks/stubs for `Arduino.h`, `Print`, `String`, `millis()`, `Serial`, pin APIs, `PROGMEM`, `WiFi.h`, `settings`, webserver URL/SSID/password helpers, page stack globals, and no-op audio/power/OTA/BLE hooks as required by linked page code.
- Use u8g2's existing full-buffer access (`getBufferPtr()`, `getBufferTileWidth()`, `getBufferTileHeight()`) to read rendered pixels instead of reimplementing drawing primitives.
- Add a tiny PNG writer, preferably vendored and offline-friendly, such as `lodepng` or `stb_image_write`.
- Suggested tool shape: `tools/lcd_preview/leaf_lcd_preview.exe --page wifi-web-app --state leaf-ap`, writing `lcd-preview.png` at native `96x192` and `lcd-preview-4x.png` for human review.
- The main implementation risk is dependency containment. Some display sources include unrelated firmware globals, so the harness should either link with dead-code stripping or split small shared drawing helpers away from hardware-heavy code when needed.
- Firmware already has a useful hardware-side reference path in `src/vario/comms/webserver.cpp`: `/api/debug/raw-xbm` dumps the current u8g2 buffer and `/app/debug/screenshot` renders it in browser. Use that once to compare the host-rendered MVP against real hardware pixels.
- Rough scope after a compiler is available: 1-2 focused days for the first deterministic page; 3-5 days for a reusable multi-page preview workflow with fixtures.

Notes from the first attempt:

- A non-exact renderer is not sufficient for design decisions.
- The Codex Windows environment at the time did not expose a native C/C++ compiler (`cl`, `gcc`, `clang`, `zig`) or a Python FFI/JIT compiler, so the exact u8g2 harness could not be built there.
- Revisit this when a host compiler is available, or consider a firmware-side debug endpoint that renders the real framebuffer and exports it for review.
