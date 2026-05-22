# ZMK config starter

This repository is a ZMK config repo for a Pro Micro form factor nRF52840
split keyboard compatible with the upstream `nice_nano_v2` board definition.

It currently builds:

- a normal `dactyl_manuform_6x6_left` central firmware target for the left half
- a dongle-mode `dactyl_manuform_6x6_left_peripheral` firmware target for the left half
- a real `dactyl_manuform_6x6_right` firmware target for the right half
- a Prospector-compatible `dactyl_manuform_6x6_dongle` target for a Seeed Studio XIAO nRF52840
- a safe `settings_reset` firmware image for clearing persistent settings

## Current Dactyl assumptions

The custom shield uses the upstream `nice_nano_v2` board target plus a local
split shield named `dactyl_manuform_6x6`. The matrix is wired like the
reference QMK `handwired/dactyl_manuform/5x7` layout, with each half using
six rows and seven columns.

- diode direction: `col2row`
- left/right column pins: `D2 D3 D4 D5 D6 D7 D9`
- shared row pins: `D14 D15 D18 D19 D20 D21`
- left side is the split central
- only one host Bluetooth profile is configured on the central side
- idle timeout is 30 seconds and deep sleep timeout is 1 hour

If your actual matrix wiring differs, update the `row-gpios` and `col-gpios`
definitions in the shield overlay files.

## Dongle mode

The dongle build follows the Prospector ZMK module setup. The XIAO dongle is
the split central and uses the `prospector_adapter` shield for the 1.69-inch
ST7789 display. Ambient light sensing is disabled by default, so the screen can
be wired without the optional APDS9960 sensor. The default Prospector layer
roller is reduced and moved to the top-left corner, with a small LVGL cat face
drawn in the center of the display.

To use dongle mode, flash:

- `dactyl-manuform-6x6-prospector-dongle.uf2` to the XIAO nRF52840 dongle
- `dactyl-manuform-6x6-left-peripheral.uf2` to the left half
- `dactyl-manuform-6x6-right.uf2` to the right half

Pair the left half to the dongle first, then the right half. To return to the
original no-dongle setup, flash `dactyl-manuform-6x6-left.uf2` back to the left
half and pair the host with the left half again.

## Wiring

For `col2row`, put the diode anodes on columns and cathodes on rows.

| Matrix net | nice!nano label |
| --- | --- |
| Column 0 | `D2` |
| Column 1 | `D3` |
| Column 2 | `D4` |
| Column 3 | `D5` |
| Column 4 | `D6` |
| Column 5 | `D7` |
| Column 6 | `D9` |
| Row 0 | `D14` |
| Row 1 | `D15` |
| Row 2 | `D18` |
| Row 3 | `D19` |
| Row 4 | `D20` |
| Row 5 | `D21` |
