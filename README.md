# MeshCore NetLink

NetLink connects MeshCore repeaters over UDP. The repeaters can reach each other over a shared LAN or the internet.

When a repeater finishes transmitting a LoRa frame, NetLink sends a copy to its peers. Each receiving repeater passes the frame to the normal MeshCore receive path. MeshCore decides whether to retransmit it and applies the local airtime budget. If the IP link goes down, both repeaters continue serving their local radio networks.

**Release 0.9.3** · **Heltec V4.3 OLED** · Based on MeshCore `repeater-v1.18.0`

[Installation and updates](docs/netlink-heltec.md) · [Peer setup and CLI reference](docs/netlink.md) · [Upstream MeshCore](https://github.com/meshcore-dev/MeshCore)

## Typical uses

- **Back up a weak radio link.** Pair the repeaters on either side of an unreliable hop. When a frame fails to cross that hop over LoRa, its IP copy can still reach the far repeater.
- **Shorten routes across a large mesh.** Link two parts of a city where packets normally cross several radio hops. A frame can reach the far side sooner and continue over LoRa from there.
- **Stay on your home mesh while traveling.** Bring a NetLink repeater, connect it to Wi-Fi or a phone hotspot, and let it dial a reachable peer at home. Your companion can talk to the nearby repeater over LoRa as usual.

## Quick start

**Flashing an existing Heltec V4.3 OLED? Choose Firmware in the OTA updater.** The release `.bin` contains only the repeater application. The **Filesystem** option writes a different partition. A normal Firmware update on the supported partition layout preserves the repeater's identity and settings. For a blank board or a USB update, follow the [installation guide](docs/netlink-heltec.md).

**1. Check both repeaters.** Set up each one as a normal MeshCore repeater and verify that it works over LoRa. Then open each repeater's CLI over USB or an authenticated LoRa admin session. Replace the placeholders below with your own values.

**2. Connect both repeaters to Wi-Fi.** On each one, run:

```text
netlink.ssid <SSID>
netlink.pass <WIFI_PASSWORD>
netlink.wifi
```

Look for `up` and an IP address. To check the clock, run `netlink.status`; its first line shows `ntp=ok` after the first time sync.

**3. Exchange public keys.** Run `netlink.id` on each board. On A, enter B's full 64-character public key. On B, enter A's. This example uses slot 1; choose a free slot on each board.

```text
netlink.peer 1 <OTHER_REPEATER_PUBLIC_KEY>
```

**4. Set one reachable address.** For an internet link, one side needs a publicly reachable UDP endpoint. If A is behind a router with a public IPv4 address, forward **UDP 4242** to A's LAN address on port **4242**. On a shared LAN, B can reach A directly at its LAN address.

On **B** (the dialing side), run:

```text
netlink.addr 1 <A_IP_OR_DNS>
```

The port defaults to 4242. If the router exposes another port, append it after the address. As with WireGuard peers, either end can initiate when it knows a reachable endpoint. Both repeaters listen for incoming peers.

**5. Check the link.** Run `netlink.status` on each board and look for `up` beside the peer. Use `netlink.status 1` for details. The OLED status screen shows the connected peer count as `Peers: N`.

## Supported hardware

The Heltec V4.3 OLED is the only board supported by this release.

## Delivery and identity

Each board keeps its own MeshCore identity and supports up to four peers. NetLink authenticates datagrams using the configured peer keys. The payload retains the encryption properties of the original radio frame.

By default, each peer has an eight-frame queue with a 60-second lifetime. Missing acknowledgements trigger retries, and a full queue keeps the newest frames. Two compatible peers can also use unacknowledged delivery by setting the queue depth to zero on either end. Keepalives run while the link is up. An offline dialer gradually reduces its retry rate while continuing to accept incoming connections.

## Build from source

From the repository root, run:

```sh
pio run -e heltec_v43_netlink
```

The application image is `.pio/build/heltec_v43_netlink/firmware.bin`. The same image works for OTA and USB application updates. See the installation guide for the partition requirements.

NetLink extends the [MeshCore repeater](https://github.com/meshcore-dev/MeshCore/tree/repeater-v1.18.0/examples/simple_repeater). This fork retains the upstream [MIT license](license.txt).

**Remember, MeshCore is a radio network first.** Use NetLink sparingly and leave room on the air for your neighbors.
