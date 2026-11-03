# esphome-gree

Full **local** control of Gree (YAW1F / YX1F family) air conditioners from ESPHome —
including features that the stock ESPHome `gree` climate component does not expose:

| Feature | Entity | Notes |
|---|---|---|
| Mode / fan / swing / temperature | `climate` entity | stock ESPHome `gree` platform |
| **Eco / "Save Energy" (SE)** | template button | official Gree Econo bit (byte 7, bit 2) |
| **DRED d2 — 50% power cap** | template button | demand-response power limitation |
| **DRED d3 — 75% power cap** | template button | demand-response power limitation |
| DRED off | template button | clears power limitation |
| Raw IR capture (learning) | `remote_receiver` | dumps any remote frame as Pronto |

No cloud, no vendor app, no WiFi module inside the AC — one ESP32-C3 board with an
IR LED and a TL1838 receiver, mounted with line of sight to the indoor unit.

## Why this exists

The stock ESPHome `gree` climate covers mode/fan/swing/temp, but two useful features
of the Gree protocol family are missing:

1. **Eco (SE)** — the official energy-saving mode. The remote shows `SE` on the
   indoor display. Implemented by setting the Econo bit in byte 7.
2. **DRED power caps (d1/d2/d3)** — the demand-response power-limitation levels
   (AS/NZS 4755). On remotes with a hidden **P** button, pressing it cycles
   d3 (~75% cap) → d2 (~50% cap) → off. The display shows `d3`/`d2` while active.

Both work from Home Assistant with the template buttons in `ac-gree.yaml`.
The DRED recipe (two-packet burst + canonical timings + 40 ms gap) was reverse
engineered with a TL1838 receiver and cross-checked against community captures —
see [PROTOCOL.md](PROTOCOL.md) for the full frame map, checksum formula, and the
pitfalls that cost us a day (single frames are silently ignored; the AC only
accepts DRED as a 2-packet burst).

## Hardware

- ESP32-C3 Super Mini (any ESP32 works; keep `output_power: 10dB` on the C3 — see notes)
- IR LED 940 nm driven through 2N2222A + base resistor, GPIO4
- TL1838 / VS1838B demodulator on GPIO5 (input, inverted, pullup)
- IR LED aimed at the indoor unit's receiver

Wiring notes are commented inline in [`ac-gree.yaml`](ac-gree.yaml).

## Install

1. Copy `ac-gree.yaml` and `secrets.yaml.example` (rename to `secrets.yaml`) into
   your ESPHome config directory; fill in the five secrets.
2. Adjust `model:` for the `gree` climate (`yac1fb9` works for YAW1F-family units;
   `yaw1f` is the alternative).
3. Flash over USB once, then adopt in ESPHome/Home Assistant.
4. Optional: regenerate frames for a different base temperature with
   [`tools/gree_ir_builder.py`](tools/gree_ir_builder.py) and paste them into the
   buttons.

```bash
# build a Cool 26 DRED d2 pronto string:
python3 tools/gree_ir_builder.py --temp 26 --dred d2
```

## Troubleshooting quick hits

- **No beep on DRED buttons** → Eco (SE) is active. SE and DRED are mutually
  exclusive on these units; the bundled buttons already sequence around it
  (send SE-off, wait 600 ms, then the DRED burst).
- **Button "works" but nothing happens** → ESPHome silently refuses malformed
  Pronto data (`Inconsistent data, not sending`). The word count must be exactly
  `4 + 2 × header_count`. Check the log.
- **ESP32-C3 keeps failing WiFi auth** → set `output_power: 10dB`, keep the
  AP to WPA2, `fast_connect: true`.
- **Captured frames from a real remote do not replay** → receiver skew marks
  read short / spaces read long; re-synthesize with canonical timings instead of
  re-sending captured values (see PROTOCOL.md).

## Credits

- Protocol references and the multi-packet structure: the
  [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) project and
  issue [#1724](https://github.com/crankyoldgit/IRremoteESP8266/issues/1724)
  (YX1F remote analysis by @stonemull and @Limb).
- ESPHome's built-in `gree` climate platform does the standard state frames.

## License

MIT — see [LICENSE](LICENSE).
