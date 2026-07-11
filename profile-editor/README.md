# Instrument profiles

Copy the `*.json` files you need onto the SD card into `/sdcard/profiles/`
(create the `profiles` directory in the card root).

After enumeration the kit reads `*IDN?` and picks the first profile whose
`match` fits (substring of the IDN and/or the USB VID). **SD profiles take
precedence over the firmware built-ins**, so you can override or add
instruments without reflashing.

## Schema (kap. 6)

| Field | Meaning |
|-------|---------|
| `id`, `name` | identification of the profile |
| `match.idn_contains` | substring that must appear in `*IDN?` |
| `match.vid` | required USB vendor id (hex string or number, `0`/omitted = any) |
| `select_single` | `true` = DMM-style, one measurement logged at a time (menu switches); `false` = all measurements logged as columns (M4b, multi-channel) |
| `autostart` | `true` (default) = logging starts right when the instrument connects (headless campaigns, power-loss recovery); `false` = wait for OK / web Start (clean data for hand-operated instruments) |
| `default_interval_ms` | sampling interval (menu can override) |
| `init[]` | SCPI commands sent once on connect |
| `measurements[]` | `{ scpi, label, unit, column }` — one per logged value |

`commands[]` from the zadani is reserved for the future control branch and is
ignored by the logger today.
