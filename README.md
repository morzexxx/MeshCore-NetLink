# NetLink for MeshCore

NetLink connects MeshCore repeaters over UDP. It is designed for places where two local LoRa networks can reach the internet or a shared LAN but cannot reach each other by radio.

When a repeater finishes transmitting a LoRa frame, NetLink sends a copy to its peers. Each receiving repeater passes the frame to the normal MeshCore receive path. MeshCore decides whether to retransmit it and applies the local airtime budget. If the IP link goes down, both repeaters continue serving their local radio networks.

**Release 0.9.3** · **Heltec V4.3 OLED** · Based on MeshCore `repeater-v1.18.0`

[Installation and updates](docs/netlink-heltec.md) · [Peer setup and CLI reference](docs/netlink.md) · [Upstream MeshCore](https://github.com/meshcore-dev/MeshCore)

## Supported hardware

This release supports one board: the **Heltec V4.3 OLED**. Other boards in the upstream MeshCore source tree have no NetLink build profile in this release.

## Connect two repeaters

1. Install NetLink on both boards. Open the repeater CLI over USB or an authenticated LoRa admin session.
2. On each board, set the Wi-Fi network with `netlink.ssid <SSID>` and `netlink.pass <PASSWORD>`. Check the assigned IP with `netlink.wifi`.
3. Run `netlink.id` on both repeaters. On each one, add the other repeater's full public key with `netlink.peer 1 <PUBLIC_KEY>`. Use a free slot if slot 1 is already in use.
4. On the dialing repeater, set `netlink.addr 1 hub.example.org 4242`, replacing the example hostname with the reachable repeater's address. If that repeater is behind a router, forward the external UDP port to its listening port (4242 by default).
5. Run `netlink.status 1` on both boards. An established link shows `up`. In acknowledged mode, `rx` counts frames accepted locally and `tx` counts frames acknowledged by the peer.

Each repeater can listen and dial at the same time. One reachable endpoint is enough for a pair. Both ends can also have a configured address.

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
