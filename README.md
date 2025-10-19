# ESP8266 Water Level Management System
This repository hosts the code for a water level management system tailored for the ESP8266 microcontroller. It's designed to monitor and control water levels in environments such as sump pits, water tanks, or irrigation systems, ensuring efficient and automated water management.

## Key Features
- **Automated Pump Control**: Engages the water pump based on predefined water level thresholds, time of day, etc.
- **Safety Measures**: Prevents the pump from running dry by deactivating it before the water level drops too low.
- **MQTT Integration**: Allows for remote monitoring and control, sending water level data and pump status to a specified MQTT broker.

## Design Decisions
- **Focused Flushing Logic**: Incorporates an active monitoring loop within `activatePump()`, ensuring water level safety during pump operation.
- **Simplicity**: Straightforward design for ease of maintenance and stability.


----

# ESP8266 Water Level Management System

Monitors a sump pit and **permits or inhibits** pump operation. Uses a hardware-fail-safe chain so the pump still runs when water is present even if the ESP8266 is offline.

## Hardware

* **MCU:** ESP8266 (NodeMCU/Wemos D1 mini class)
* **Sensor input:** Analog water-level probe → `A0` (0–1 V scaled to board spec)
* **Inhibit output to NC contactor:** `GPIO5` (D1) → drives a relay that **inhibits** the pump when active
* **External status LED:** `GPIO4` (D2) with series resistor
* **On-board LED:** `LED_BUILTIN` heartbeat
* **Contactor (fail-safe):** Finder **6K.04.9.024.4709** NC industrial contactor, ~**55 CAD**, rated to **−40 °C**
* **Level sensing rod:** Amazon depth/level rod, link: [https://www.amazon.ca/dp/B0B886FY16](https://www.amazon.ca/dp/B0B886FY16)
  * **Installed length:** `ROD_LENGTH = 42` cm in firmware. If you use a different length, update `ROD_LENGTH` and the lookup table.
  * **Mounting:** Fix vertically. Zero is the pit bottom. Ensure the lowest mark is above sediment. Keep cable strain-relieved and drip-looped.
  * **Calibration:** The firmware maps ADC readings to centimeters via a lookup table. Populate points at known depths (0, 5, 10… 42 cm), then refine midpoints to linearize your sensor response.
    * Edit `lookupTable[][2]` pairs `{adc, cm_at_tip}`.
    * After changes, verify with a meter stick and log output.
  * **Electrical:** Use a proper divider to 0–1 V for `A0`. Add RC filter if noisy (e.g., 1–4.7 kΩ series, 0.1–1 µF to GND at ADC). Keep probe wiring away from pump and mains.
  
### Wiring model (summary)

1. **Float/mechanical relay** closes when water is present.
2. **Finder 6K NC contactor** is in series with the pump. With no ESP control, the NC path lets the pump run when the float closes.
3. **ESP “inhibit” relay** breaks the contactor control path.

   * Output logic: **HIGH = inhibit**, **LOW = allow** (configurable).
4. If the ESP dies or reboots, hardware defaults to **pump runs when water present**.

> This repo’s firmware treats the ESP output as an **inhibit gate**, not a primary motor driver.

## Pin map (defaults)

| Function               | Pin           | Notes                              |
| ---------------------- | ------------- | ---------------------------------- |
| Water level ADC        | `A0`          | Use proper divider/filtering       |
| Inhibit output         | `GPIO5` D1    | `INHIBIT_ACTIVE_LEVEL` is **HIGH** |
| External status LED    | `GPIO4` D2    | Active HIGH                        |
| On-board heartbeat LED | `LED_BUILTIN` | Board-specific                     |

Adjust pins in `#define` if your board differs.

## Control logic (concise)

* **Inhibit gate:**

  * On boot: **inhibit = ON** (safe).
  * MQTT safety override topic can force inhibit ON.
* **Night window:** treated as **22:00–03:59** by default. If **time unsynced**, **assume night**.

  * At night, **allow** if level > `waterLevelThreshold`, for up to `pumpOperationTimeout`.
* **Daytime:** keep **inhibit ON** unless emergency:

  * `level > criticalWaterLevel`, or
  * **fast rise** ≥ threshold over a **5-minute** window.
    Emergency allows open a short **allow window** (default 5 min, capped by `pumpOperationTimeout`).
* **Refractory:** after an allow window ends, wait 5 minutes before re-allowing, unless level is **critical**.
* **Hardware stops the pump** when the pit empties. Software does not force-stop the motor.

## LED states

* **External LED (D2):**

  * Solid ON: allow window active.
  * 1 Hz blink: normal.
  * 2 Hz: MQTT down.
  * 4 Hz: Wi-Fi down.
  * Short flash every 3 s: time unsynced.
  * 1 s ON / 1 s OFF: safety “unsafe” received.
* **On-board LED:** 1 Hz heartbeat.

## MQTT

* **Publishes**

  * `pool/sumppump/status` → `"allow"` or `"inhibit"`
  * `pool/sumppump/level` → integer centimeters (periodic and on ≥1 cm change)
  * `pool/sumppump/alert` → text alert (e.g., insufficient drop)
* **Subscribes**

  * `pool/sumppump/safe` → `"no"` forces inhibit; any other value clears the override

## Configuration highlights

```cpp
#define NIGHT 0
#define MORNING 4                      // 00:00–03:59 is “night”
#define INHIBIT_ACTIVE_LEVEL HIGH      // set LOW if your relay inverts
const int  waterLevelThreshold = 20;   // cm; night “needed” threshold
const int  criticalWaterLevel = 40;    // cm; day emergency
const unsigned long pumpOperationTimeout = 5UL * 60000UL; // max allow window
// Fast-rise detector: computes cm/min over 5 minutes; default trip ~1 cm/min
```

## Reliability notes

* **Wi-Fi boot:** firmware waits up to ~20 s on first connect; runtime uses exponential backoff with `yield()` to keep the stack responsive.
* **MQTT:** only attempted when Wi-Fi is up; unique client ID avoids broker session collisions on rapid reboots.
* **Watchdog:** 60 s; main loop is non-blocking.
* **ADC stability:** first few reads after boot can be noisy; code buffers readings and publishes level periodically or on ≥1 cm change.

## Safety

* Mains work can kill. Use a licensed electrician.
* Keep low-voltage ESP wiring isolated from contactor mains.
* Add snubbers or MOVs per your contactor/coil spec.
* Validate fail-safe: with ESP powered off, confirm pump runs when float closes.

## Future Plans
- **Predictive Analysis of Weather Forecasts**: Before the clouds even decide to open up, our system will already be two steps ahead. By analyzing weather forecasts with the same intensity a trader analyzes the stock market, we'll predict when to activate the pump, ensuring preemptive action against any potential waterlogging. This feature is in development, pending our ability to negotiate data sharing terms with the weather itself.
- **Historical Pump Activation Data Mining**: They say history repeats itself, and who are we to argue? By meticulously studying the patterns of past pump activations, we aim to uncover the mystic rhythms of subterranean hydrodynamics. This deep dive into historical data will not only refine our activation protocols but might also accidentally advance the field of archaeology.
- **Astrological Water Level Management**: In the grand scheme of the cosmos, even sump pumps play a pivotal role. By integrating astrological data, our system will provide tailored water management solutions based on the alignment of stars and planets. Mercury in retrograde? Maybe it's a sign to double-check those pump connections. Full moon in Pisces? Waters might rise, but your pump will be ready and waiting.

## License
MIT.
