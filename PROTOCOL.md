# Gree IR protocol notes (YAW1F / YX1F family)

Everything below was verified against a Gree 0.5 PK inverter unit with a YAW1F9
remote, using a TL1838 receiver and ESPHome's `remote_receiver`/`remote_transmitter`.

## Frame structure

One Gree transmission = 67 bits:

```
[ 32 bits data block A ][ 3-bit footer 010 ][ 32 bits data block B ]
```

Bytes are sent LSB-first. Timing (canonical, from IRremoteESP8266):

| Symbol | Value |
|---|---|
| Header mark | 9000 us |
| Header space | 4500 us |
| Bit mark | 620 us |
| Zero space | 540 us |
| One space | 1600 us |
| Mid/footer gap | 620 us mark + 19980 us space |

## Checksum

4-bit checksum in the high nibble of byte 7 (block B's last byte):

```
sum = 10                                  # start value
sum += low_nibble(byte0..byte3)           # four low nibbles
sum += high_nibble(byte4..byte6)          # three high nibbles
byte7 = (sum & 0xF) << 4 | low_nibble(byte7)
```

Verified on every captured frame from both the remote and the ESPHome climate.

## Byte map (empirical, this unit)

| Byte | Meaning (observed) |
|---|---|
| 0 | mode + power (0x09 = cool, on) |
| 1 | temperature (0x08 = 24 C) + timer bits |
| 2 | flags — remote-family frames use 0x60, climate-family frames use 0x20 |
| 3 | swing / packet-role bits; frame-C (DRED sync) uses 0xA0 / 0xA4 |
| 4-6 | family-dependent flags (remote: 06 80 00, climate: 00 20 20) |
| 7 | high nibble = checksum; bit 2 of low nibble = **Econo (SE)** |

## The two frame families

- **Climate family** (`20 50 00 20 20` tail) — accepted by the AC from any
  transmitter. The stock ESPHome `gree` climate sends these.
- **Remote family** (`60 50 06 80 00` tail) — what the physical YAW1F9 remote
  sends. A single frame of this family from a third-party transmitter is
  **ignored**; only a complete burst is accepted.

## Eco / "Save Energy" (SE)

Take a climate-family state frame and set the Econo bit:

```
09 08 20 50 00 20 20 F0   ->   09 08 20 50 00 20 20 F4
                                     (byte 7 low nibble |= 0x04)
```

The indoor display shows `SE`. Cool mode only. Works as a single frame.

## DRED (power limitation, d1/d2/d3)

The remote's hidden **P** button cycles d3 -> d2 -> off. The level itself is
carried by a separate near-zero frame ("frame C"), NOT by the state frame:

| Level | Frame C bytes | Checksum byte |
|---|---|---|
| off | `00 00 00 A0 00 00 00` | `A0` |
| d3 (~75%) | `01 00 00 A0 00 00 00` | `B0` |
| d2 (~50%) | `00 00 00 A4 00 00 00` | `E0` |

### The recipe that works from a third-party transmitter

A command = **two transmissions**, ~40 ms apart, both with canonical timings:

```
1. state frame, remote family:   09 08 60 50 06 80 00 <ck>   (temp is baked in)
2. frame C for the desired DRED level
```

ESPHome YAML:

```yaml
- remote_transmitter.transmit_pronto:
    data: "<state frame>"
- delay: 40ms
- remote_transmitter.transmit_pronto:
    data: "<DRED frame C>"
```

The physical remote actually sends three frames per keypress ([A, B, C]);
the shorter [A, C] burst is accepted too. ~35 ms and ~88/162 ms gaps did NOT
work for us — 40 ms did.

### Mutual exclusion with Eco (SE)

While Eco (SE) is active, DRED frames are ignored. Turn SE off first, wait
(~600 ms), then send the DRED burst. The bundled buttons already do this.

## ESPHome Pronto pitfalls

- Word count must be exactly `4 + 2 x word[2]` (pure mark/space pairs, no
  trailing mark). One word off -> `Inconsistent data, not sending` and the
  button silently does nothing.
- Receiver dumps are also `4 + 2N`; do not append a trailing mark when rebuilding.
- Receiver skew: TL1838 marks read ~50 us short, spaces ~90 us long. Use captured
  values to *read bits* (threshold: space > ~1200 us = 1) but *send* canonical
  values.
- Receiver `idle:` max is 32767 us. `idle: 30ms` merges the two Gree blocks into
  one capture; the 10 ms default splits them.

## Capturing your own remote

Set `idle: 30ms` on `remote_receiver`, press the remote button near the TL1838,
and read the `remote.pronto` dump from the log. Decode block A (pairs 1-32 after
the header), the footer, and block B; recompute the checksum with the formula
above. `tools/gree_ir_builder.py` regenerates Pronto strings from byte arrays.
