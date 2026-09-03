# SoftDevice (not included)

The Nordic **s140 v6.1.1** SoftDevice is proprietary and is **not redistributed**
in this repository (see Nordic's Software Development Kit license). You must
supply it yourself before the one-time flash step.

## What you need

A combined **MBR + s140 6.1.1** Intel-HEX image named:

```
softdevice/s140_6.1.1_mbr.hex
```

`flash_softdevice.cfg` expects exactly this path/name.

## Where to get it

The matching SoftDevice ships with the Adafruit nRF52 Arduino core (the same
core this project builds against). After PlatformIO has fetched the framework,
it is under:

```
~/.platformio/packages/framework-arduinoadafruitnrf52/cores/nRF5/nordic/softdevice/s140_nrf52_6.1.1_API/
```

and the raw hex is distributed with the Nordic **nRF5 SDK** / the S140 6.1.1
release on Nordic Semiconductor's website. Combine the MBR + SoftDevice into a
single hex (or use the pre-combined image from the Adafruit bootloader release
assets) and save it to the path above.

Because it is proprietary, the file is listed in `.gitignore` — keep it out of
any public fork.
