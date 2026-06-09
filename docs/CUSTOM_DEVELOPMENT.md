# Custom Development Notes

This document describes the verified engineering extensions in this repository relative to the MimiClaw upstream baseline.

## Development Focus

The work turns the original headless embedded agent into a more observable and hardware-oriented device. The main goals were:

1. Give the device a visible, expressive runtime state.
2. Adapt model access for MiniMax and Chinese-oriented usage.
3. Make the Agent execute tools before claiming an action succeeded.
4. Add low-level commands for practical board bring-up and wiring diagnosis.
5. Improve Feishu stability under ESP32-S3 memory constraints.

## ST7789 Display Subsystem

`main/display/lcd_display.c` implements the active display backend. Although the compatibility API retains the `oled_display_*` naming, the configured target is a 240x240 ST7789 SPI LCD.

Implemented capabilities:

- ST7789 command/data initialization sequence
- SPI panel communication and chunked framebuffer flush
- PSRAM/DMA-capable framebuffer allocation
- Primitive drawing and custom 5x7 ASCII font rendering
- Wrapped status text and message previews
- Visual states for boot, idle, happy, busy, listening, error, and Wi-Fi

`main/mimi.c` and `main/agent/agent_loop.c` connect those states to real firmware events, including startup, Wi-Fi status, channel traffic, Agent processing, and LLM errors.

The compact renderer currently sanitizes non-ASCII text. MiniMax improves Chinese-language model interaction, but the LCD module does not yet provide a Chinese glyph library.

## Model and Agent Behavior

### MiniMax adaptation

The Anthropic-compatible request path is routed to:

```text
https://api.minimaxi.com/anthropic/v1/messages
```

The default model is:

```text
MiniMax-M2.7-highspeed
```

The OpenAI-compatible provider path remains available through runtime configuration.

### Tool-first execution constraints

The Agent context explicitly requires real tool calls for actions such as time lookup, scheduling, and hardware control. It also prevents success claims before the corresponding tool returns successfully.

This is an important reliability change for an embedded Agent because a conversational acknowledgement is not equivalent to a physical or scheduled action.

## Hardware Diagnostic CLI

The serial CLI adds:

| Command | Purpose |
| --- | --- |
| `i2c_scan` | Scan selected SDA/SCL pins for I2C devices |
| `i2c_diag` | Inspect bus levels and attempt clock-pulse recovery |
| `pin_set` | Drive a GPIO output for physical measurement |
| `pin_read` | Read a GPIO input with pull-up |
| `pin_blink` | Toggle a GPIO to verify wiring |
| `oled_force` | Reinitialize the configured display through a compatibility command |

These commands shorten the feedback loop when debugging a new board, display, sensor, or pin assignment.

## Feishu PSRAM Optimization

The Feishu channel now prefers PSRAM for large HTTP and WebSocket buffers and for the WebSocket task stack. Each allocation path has a standard-memory fallback so the firmware can still run when PSRAM allocation fails.

## Main Changed Areas

| Path | Role |
| --- | --- |
| `main/display/` | New display implementation and compatibility interface |
| `main/mimi.c` | Display initialization and channel/system status integration |
| `main/agent/agent_loop.c` | Agent processing and error display integration |
| `main/agent/context_builder.c` | Tool-first execution rules |
| `main/cli/serial_cli.c` | I2C, GPIO, and display diagnostic commands |
| `main/channels/feishu/feishu_bot.c` | PSRAM-oriented allocation and task creation |
| `main/llm/llm_proxy.c` | MiniMax-compatible host and endpoint |
| `main/mimi_config.h` | Model defaults and ST7789 hardware configuration |
| `main/CMakeLists.txt` | Display source and driver dependencies |

## Attribution

This implementation is based on the MIT-licensed [MimiClaw](https://github.com/memovai/mimiclaw) project by Ziboyan Wang. The original license and copyright notice are preserved in `LICENSE`.
