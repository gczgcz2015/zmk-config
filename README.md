# ZMK config starter

This repository is a ZMK config repo for a Pro Micro form factor nRF52840
split keyboard compatible with the upstream `nice_nano_v2` board definition.

It currently builds:

- a real `dactyl_manuform_6x6_left` firmware target for the left half
- a real `dactyl_manuform_6x6_right` firmware target for the right half
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
