# ZMK config starter

This repository is a ZMK config repo for an Adafruit `nice!nano` compatible
split keyboard.

It currently builds:

- a real `dactyl_manuform_6x6_left` firmware target for the left half
- a safe `settings_reset` firmware image for clearing persistent settings

## Current Dactyl assumptions

The custom shield uses the upstream `nice_nano` board plus a local split shield
named `dactyl_manuform_6x6`.

- diode direction: `col2row`
- left/right column pins: `D2 D3 D4 D5 D6 D7`
- shared row pins: `D8 D9 D10 D14 D15 D16`
- left side is the split central
- only one host Bluetooth profile is configured on the central side
- idle timeout is 30 seconds and deep sleep timeout is 1 hour

If your actual matrix wiring differs, update the `row-gpios` and `col-gpios`
definitions in the shield overlay files.
