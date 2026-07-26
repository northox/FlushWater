# ESP8266 Irrigation Manager

A small MQTT-driven irrigation controller built on an ESP-12F / NodeMCU. It
drives three valve zones, takes its orders from Home Assistant, and refuses to
leave a valve open if the network goes away.

## Key features

- **MQTT control** - topics `irrigation/control0` / `control1` / `control2`,
  payloads `on` / `off`.
- **Authenticated broker connection** - the firmware logs in with a username and
  password. An anonymous connect is rejected by most brokers.
- **One zone at a time** - Home Assistant enforces mutual exclusion, so opening
  one valve always closes the other two.
- **Weather-aware** - the scheduled runs are skipped when the forecast rain
  probability is 50% or higher.
- **30 min auto-off watchdog** - each zone closes itself 30 minutes after it was
  opened, even if Wi-Fi or MQTT dropped in the meantime.
- **Wi-Fi auto-reconnect** with exponential backoff, capped at 40 s.
- **Hardware watchdog** - 60 s window; the ESP reboots if it locks up.
- **Status LED** - solid when a zone is running, fast blink when Wi-Fi is down,
  medium blink when MQTT is down, slow heartbeat when healthy.

## Hardware

1. **ESP8266** - NodeMCU 1.0 or a bare ESP-12F.
2. **3-channel relay module** wired to:

   | Zone | Sketch macro | Pin | GPIO |
   |------|--------------|-----|------|
   | 0    | `RELAY0_PIN` | D1  | 5    |
   | 1    | `RELAY1_PIN` | D2  | 4    |
   | 2    | `RELAY2_PIN` | D5  | 14   |

   The sketch drives the relays **active HIGH** (`digitalWrite(pin, HIGH)` opens
   a valve). If your relay board is active LOW, invert the writes in
   `mqttCallback()` and `enforceAutoOff()`.

3. **An MQTT broker** - e.g. the Mosquitto add-on on Home Assistant.
4. **An OpenWeatherMap API key** - only needed for the rain check.

## Firmware setup

Credentials are kept out of git in `src/config.h`:

```sh
cp src/config.example.h src/config.h
$EDITOR src/config.h
```

Fill in your Wi-Fi SSID/password, the broker address, and the MQTT username and
password. `src/config.h` is in `.gitignore`.

### Building

**PlatformIO** - `pio run -t upload` from the repo root.

**Arduino IDE** - the IDE requires the sketch folder and the `.ino` to share a
name, so copy the two files into a folder called `irrigator/`:

```sh
mkdir -p ~/Arduino/irrigator
cp src/irrigator.ino src/config.h ~/Arduino/irrigator/
```

Then select board *NodeMCU 1.0 (ESP-12E Module)* and upload. Requires the
`PubSubClient` library.

### Putting a bare ESP-12F into flash mode

A NodeMCU-style board with onboard USB handles this automatically - just hit
upload. A bare module wired to a USB-serial adapter does not:

1. Wire adapter TX -> ESP RXD0, RX -> TXD0, GND -> GND, 3.3 V -> VCC.
   **The ESP-12F is 3.3 V only - 5 V will destroy it.** CH_PD/EN must be pulled high.
2. Pull **GPIO0 to GND** (jumper, or hold the Flash/Boot button).
3. While GPIO0 is held low, briefly pulse **RST** to GND and release.
4. Keep GPIO0 grounded for the whole upload.
5. When the upload finishes, release GPIO0 and tap RST again to boot normally.

## Broker configuration

The firmware authenticates, so the broker needs a matching login. For the Home
Assistant Mosquitto add-on, under *Settings > Add-ons > Mosquitto broker >
Configuration*:

```yaml
logins:
  - username: irrigator
    password: <a long random password>
```

Use the same values in `src/config.h`.

> If the firmware connects **without** credentials, the broker log fills with
> `received null username or password for unpwd check` followed by
> `Client ESP8266Client-irrigator disconnected, not authorised`, and the zones
> never respond. That log line is the fastest way to diagnose it.

## Home Assistant integration

Merge [`homeassistant/configuration.yaml`](homeassistant/configuration.yaml)
into your own config. It provides:

- three MQTT switches, `switch.irrigation_zone_0` / `_1` / `_2`
- `input_select.irrigation_zone` - the single control surface
- `sensor.rain_probability` - a template sensor built from the OpenWeatherMap
  daily forecast
- the exclusivity automation and the watering schedule

Add the OpenWeatherMap integration through the UI *before* reloading, so the API
key stays out of your config file.

For the dashboard card, see
[`homeassistant/lovelace-irrigation.yaml`](homeassistant/lovelace-irrigation.yaml).

### How control flows

```
dashboard / schedule --> input_select.irrigation_zone
                              |
                              v
                "Irrigation select exclusive"
                              |
                closes all 3, opens the chosen one
                              |
                              v
                   switch.irrigation_zone_N
                              |
                       MQTT command_topic
                              v
                            ESP8266
```

Because everything routes through the selector, the dashboard always shows which
zone is actually running.

### Schedule

| Zone | Name          | Days          | Window        |
|------|---------------|---------------|---------------|
| 1    | House front   | Tue, Thu, Sat | 02:11 - 02:48 |
| 0    | Car side      | Tue, Sat      | 02:48 - 03:35 |
| 2    | Street corner | Tue, Sat      | 03:35 - 05:20 |

Each step hands over to the next. A zone only *starts* if rain probability is
under 50%; the handover and the final 05:20 all-off run unconditionally, so a
skipped condition can never leave a valve open. The 05:20 step also issues a
direct `switch.turn_off` on all three zones as a fail-safe, independent of the
selector.

Rename the zones by editing the `options:` list of `input_select.irrigation_zone`
and the matching `value_template` strings in the exclusivity automation.

### Why OpenWeatherMap and not Met.no

`sensor.rain_probability` is derived from `weather.openweathermap` because
**Met.no does not expose `precipitation_probability`** - its forecasts only carry
`precipitation` in mm. If you point the template sensor at a Met.no entity it
will silently return 0 and you will water through every storm.

## Known gaps

- **No state feedback.** The firmware subscribes to `irrigation/controlN` but
  never publishes to `irrigation/statusN`, so Home Assistant shows commanded
  state, not confirmed state. Publishing a retained message on each relay change
  would close this loop.
- **No soil moisture input** - watering is purely time and forecast driven.
- **`retain: true`** on the command topics means the ESP replays the last command
  on reconnect. Convenient, but a stale retained `on` will reopen a valve at boot.

## Future plans

- **Plant Mood Detection(TM)**: photograph the ferns, run them through a neural
  net, trigger emergency sprinkling if they give you the stink eye.
- **Quantum Soil Moisture Sensor**: the soil is both wet and dry until observed.
  More research needed.
- **Interplanetary Irrigation**: red-dust-resistant nozzles and dust-storm mode
  for the Mars colony. Earth mode still works.
- **Astrological Alignment**: Mercury in retrograde? Best hold off. Full moon in
  Leo? Water twice.
- **Plant-GPT**: "Hey Basil, you look thirsty. Drizzle or deluge?" Basil: "Yes."
