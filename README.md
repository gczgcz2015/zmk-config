# ZMK config starter

This repository is a minimal ZMK config repo for an Adafruit `nice!nano`.

It currently builds a safe `settings_reset` firmware image so the GitHub Actions
pipeline, artifact download, and UF2 flashing path can be verified before a real
keyboard shield is added.

## What is included

- A standard ZMK GitHub Actions workflow
- A `build.yaml` target for `nice_nano + settings_reset`
- A placeholder `config/` directory for future keyboard-specific files

## Next step for a real keyboard firmware

When you know the keyboard shield name or the custom matrix wiring, replace the
`settings_reset` target in `build.yaml` and add the corresponding files under
`config/`.
