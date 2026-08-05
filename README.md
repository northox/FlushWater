# FlushWaterNG

Sump pump controller for the **Seeed XIAO ESP32-C3**. Reads a resistive water
level sender, decides when the pump may run, and reports to MQTT / Home
Assistant.

The pump is not driven directly — the relay *inhibits* or *allows* it, so the
pump's own float switch remains the final authority.

## Behaviour

| | Condition to allow the pump |
|---|---|
| **Night** (22:00–04:59) | `level > waterLevelThreshold` (5 cm) |
| **Day** (05:00–21:59) | `level > criticalWaterLevel` (32 cm) **or** rising ≥ 1.0 cm/min |
| **Always required** | MQTT has not published `no` to the safety topic |
| **Refractory** | 5 min lockout after each window — bypassed when critical |
| **Window** | 5 min max, closes early once drained (min 30 s) |

Two failure directions are deliberate: an unknown clock falls back to **night**
rules so a network outage cannot disarm flood protection, and a critical level
**bypasses** the refractory so a real flood is not locked out half the time.

## Status LED

One LED, counted blink codes — N pulses, then a long dark gap.

| LED | Meaning |
|---|---|
| Solid on | Pump allowed right now |
| 1 blip | All good, idle |
| 2 blips | No WiFi |
| 3 blips | WiFi up, no MQTT broker |
| 4 blips | Connected, clock not NTP-synced |
| 5 blips | MQTT reported unsafe to operate |
| 6 blips | Sender table malformed — pump forced allow |
| Dark | Firmware not running |

## Hardware

Seeed XIAO ESP32-C3, a resistive level sender (240 Ω empty → 33 Ω full, the
standard automotive range), and a relay module on its own supply.

```
SUPPLY --[ R_TOP ]--+-- node --[10k]--+-- D1 (ADC)
                    |                 |
                [ sender ]         [100nF]
                    |                 |
                   GND               GND
```

| XIAO pin | Function |
|---|---|
| D1 | Divider node, via 10k series + 100nF to GND |
| D10 | Relay inhibit/allow, **10k pull-up to 3V3** |
| D5 | Status LED anode → 220 Ω → GND |

Non-negotiable details:

- **ADC1 only.** D0/D1/D2 work; D3 is ADC2 and returns garbage while WiFi runs.
- **BAT54 Schottky from D1 to 3V3.** An open sender pulls the node to the full
  supply; the 10k plus the clamp keeps that off the GPIO.
- **10k pull-up on D10.** `INHIBIT_ACTIVE_LEVEL` is HIGH, so a floating-low pin
  at boot means *pump allowed* — the wrong failure direction. Internal pull-ups
  are inactive during reset.
- **5V pin or USB, never both.** That pin is USB VBUS.

## Build

PlatformIO:

```sh
cp src/config.example.h src/config.h   # then fill in your credentials
pio run -t upload
pio device monitor
```

`src/config.h` is git-ignored and must never be committed.

Arduino IDE also works — copy `src/main.cpp` to `FlushWaterNG.ino` alongside
`config.h`, select board **XIAO_ESP32C3** and set **USB CDC On Boot: Enabled**.
Without that setting `Serial` is routed to the GPIO20/21 UART, which is not
wired to the USB-C connector, and the monitor stays silent. The source carries
explicit forward declarations, so it compiles as either `.ino` or `.cpp`.

## Calibration

Level is looked up by **sender resistance**, not ADC counts, so the table
survives a change of supply voltage, top resistor, or chip.

1. Measure `SUPPLY_MV` and `R_TOP_OHM` with a meter — on the supply you will
   actually run on. USB VBUS and an external brick do not read the same.
2. Set `CALIBRATION_VERBOSE 1` and log resistance against known water heights.
3. Replace `senderTable[]`. It must be strictly ascending in resistance and
   descending in level; `validateSenderTable()` checks this at boot and fails
   the pump to *allowed* if it does not hold.

The sender is not linear — roughly 3.5 Ω/cm through the main body but ~12 Ω/cm
below 7 cm. Keep the dense rows at the bottom; that is where the decisions are.

## MQTT

| Topic | Direction | Payload |
|---|---|---|
| `pool/sumppump/safe` | in | `no` inhibits the pump; anything else allows it |
| `pool/sumppump/status` | out, retained | `allow` / `inhibit` |
| `pool/sumppump/level` | out | level in cm |
| `pool/sumppump/alert` | out | sensor faults, ineffective pump, expired safety hold |
| `pool/sumppump/log` | out | boot and 5-minute heartbeat diagnostics |

A `no` on the safety topic expires after 30 minutes without a broker update and
fails **open**. A latch that can never be cleared is a flood waiting to happen.

## Home Assistant

Copy the entities from [`configuration.yaml`](configuration.yaml) into your HA
config and restart. The broker needs a matching login — see
[`src/config.example.h`](src/config.example.h).

You get:

| Entity | What it is |
|---|---|
| `sensor.sump_water_level` | Level in cm, graphable |
| `binary_sensor.sump_pump_allowed` | Whether the pump may run right now |
| `switch.sump_safety_hold` | Turn on to inhibit the pump |
| `sensor.sump_last_alert` | Sensor faults, ineffective pump, expired hold |
| `sensor.sump_diagnostics` | Boot line and 5-minute heartbeat |

[`lovelace-flushwater.yaml`](lovelace-flushwater.yaml) has a dashboard card with
the level history and pump state overlaid.

Add the automations too:

```yaml
automation: !include automations.yaml
```

**How the persistent hold works.** Turning the switch on publishes `no`, but the
controller drops that hold after 30 minutes without a further message — a latch
nothing can clear is a flood waiting to happen. So HA re-publishes it every 10
minutes, and a hold you set on purpose lasts indefinitely.

The two halves matter together: as long as HA is alive the hold sticks, and if
HA or the broker dies the hold lapses within 30 minutes and the controller goes
back to protecting the basement on its own. The 10-minute interval tolerates two
consecutive missed runs. Don't widen it much — at 25 minutes a single missed run
releases the pump.

`automations.yaml` also notifies on any controller alert, and warns if
`sensor.sump_diagnostics` goes quiet for 20 minutes. The heartbeat is every 5,
so silence means the controller is down and the pump is running unsupervised on
its own float switch.

## Resilience

- Task watchdog on the loop task, 60 s.
- Connectivity watchdog: reboot after 15 min offline, since the task WDT cannot
  catch a wedged network stack — `loop()` keeps running and feeding it. Defers
  while a flush is in progress.
- Reset reason reported at boot and in heartbeats. Watch for `BROWNOUT`: a pump
  motor starting sags a shared supply and otherwise looks like a random reboot.

## License

See [LICENSE](LICENSE).
