# Install NetLink on a Heltec V4.3 OLED

NetLink 0.9.3 targets the Heltec V4.3 OLED with the upstream 16 MB flash layout. The release image is named `Heltec-V43-OLED-NetLink-0.9.3.bin`. A local PlatformIO build produces the same type of application image under the name `firmware.bin`.

## Update an existing repeater over Wi-Fi

1. Run `start ota` in the repeater CLI. The board returns an update URL.
2. Connect to the board's `MeshCore-OTA` Wi-Fi network and open the returned URL, including `/update`. The root page displays the repeater name.
3. Select **Firmware** for the release `.bin`. It contains only the repeater application. The **Filesystem** upload target writes over the filesystem partition, which holds the repeater identity and settings. Check the selected target before uploading, then let the board restart.
4. Reconnect to the CLI. Run `ver` to check for `v1.18.0-netlink.0.9.3`. Use `get name`, `get radio`, and `netlink.status` to check the configuration.

## Update an existing repeater over USB

Use the same application image. Read the device's partition table and OTA boot selection to identify the active application slot before writing. On the upstream layout, slot 0 starts at `0x10000`. A board previously updated over OTA may boot slot 1.

Write only the application image to the selected slot. Keeping the partition table, OTA selection, NVS, and SPIFFS preserves the board identity, access list, radio settings, and saved peer configuration. New NetLink options receive their default values when loading older settings.

## First installation

A blank board also needs the bootloader, partition table, and boot selection. With PlatformIO installed, connect the board over USB and run the complete environment upload from the repository root:

```sh
pio run -e heltec_v43_netlink -t upload
```

On a clean installation, the radio profile starts at 869.525 MHz, 62.5 kHz bandwidth, SF7, coding rate 4/7, TX setting 0, two-byte path hashes, and a 10% airtime budget. Configure the radio for the local network and region before deployment.

Continue with [peer setup and the CLI reference](netlink.md).
