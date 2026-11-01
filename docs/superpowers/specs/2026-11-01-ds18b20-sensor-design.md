# DS18B20 sensor migration

Replace the production NTC input with one externally powered DS18B20 probe on
the existing classic ESP32 Dev Kit. The owner selected the waterproof probe
and terminal adapter and supplied its specifications on 2026-11-01.

## Wiring and limits

- Adapter DATA / yellow probe lead: GPIO27.
- Adapter VCC / red probe lead: 3.3 V.
- Adapter BLK / black probe lead: GND, shared with the ESP32.
- DATA requires a 4.7 kΩ pull-up to 3.3 V. Verify the adapter supplies it;
  install an external resistor only if absent. The description suggests an
  included pull-up but does not confirm its value.
- Remove the old NTC divider and its 100 nF capacitor from the sensor circuit.
  GPIO34 is input-only and cannot drive the 1-Wire bus.
- The supplied cable is rated to 85 °C, even though the sensor IC supports
  −55 to 125 °C. Keep the cable away from the heater and hot surfaces.
- Heater GPIO25, fan GPIO26, 0–80 °C targets and the 82 °C shutdown remain.

## Approaches

Use Espressif's maintained DS18B20 and RMT 1-Wire components, replacing the
production ADC path. A custom bit-banged protocol would duplicate existing
drivers and compete with Wi-Fi timing. Selectable NTC support adds wiring and
configuration choices that the requested replacement does not need. The
obsolete NTC sources and their driver tests are removed; the firmware and test
applications use the DS18B20 component.

## Implementation choices

- Configure 11-bit (0.125 °C) resolution before every conversion, including
  after hot-plug. The vendor driver waits 400 ms, fitting a 500 ms acquisition
  cycle. Keep the independent 100 ms control task and 2 s stale timeout.
- Initialize bus resources without requiring the probe to be connected, so
  the dashboard remains available and a missing probe is a recoverable fault.
- Only publish a temperature after conversion and a CRC-checked scratchpad
  read. Missing presence, stuck-low DATA, transport/CRC errors, nonfinite
  values and values outside −55 to 125 °C invalidate the sample.
- Check DATA both before acquisition and after reading. Enforce sample age in
  the independent controller even if acquisition stalls; after a gap over 2 s,
  clear the filter and require three new valid samples for fault recovery.
- Preserve open/short/range/invalid fault names in the existing dashboard API,
  median filtering, the three-sample enable gate and manual fault recovery.
- Introduce sensor-neutral reading/fault types instead of exposing ADC fields
  to the temperature monitor or runtime.
- Migrate valid schema-1 settings to schema 2, preserving the target and
  resetting the NTC calibration offset to 0 °C for the new probe. Wi-Fi lives
  in a separate partition and is unaffected. New DS18B20 calibration persists.

## Verification and scope

Host tests cover driver sequencing, faults, cleanup, reconnect, range checks,
settings migration and the controller's existing safety/maintenance behavior.
Build the production, unit-test and relay-off sensor-smoke images with
ESP-IDF 6.1. Physical wiring, thermometer comparison and fault injection are
commissioning steps for the owner. Do not flash or publish as part of this
source change.

References: [DS18B20 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS18B20.pdf),
[Espressif DS18B20 driver](https://components.espressif.com/components/espressif/ds18b20/versions/0.4.0/readme),
[ESP32 pin table](https://documentation.espressif.com/esp-dev-kits/en/latest/esp32/esp32-devkitc/user_guide.html).
