# NetLink 0.9.2

NetLink adds a UDP peer transport to the MeshCore 1.18.0 repeater for Heltec V4.3 OLED. Up to four peers can be configured. Each board has its own MeshCore identity, name and radio settings. Internet access is optional for local forwarding.

## Pair two repeaters

1. On each board, enter `netlink.ssid <SSID>` and `netlink.pass <password>`. Check `netlink.wifi`.
2. Read `netlink.id` on each board. On each end, run `netlink.peer 1 <other board's full public key>`.
3. On the board behind NAT, run `netlink.addr 1 hub.example.org 4242`. Forward UDP port 4242 to the listening board. Leave its peer address empty for passive operation. Both sides can also have addresses.
4. Check `netlink.status 1` on each board. A matching pair reports `up`. For radio administration, log in with the normal MeshCore admin credentials.

The UDP link authenticates traffic using the MeshCore identities and a directional HMAC. It carries the original LoRa bytes without adding channel encryption. Each local LoRa TX completion yields one IP copy per eligible peer. The receiving repeater hands the frame to the ordinary MeshCore dispatcher. Radio forwarding, duplicate handling and the airtime budget remain under MeshCore control.

## Commands

Slots are numbered 1–4. Configuration changes persist across restarts. A public key is 64 hex characters.

| Command | Purpose |
|---|---|
| `netlink.id` | Read the local public key. |
| `netlink.wifi` | Show SSID, connection state, address and signal strength. |
| `netlink.ssid NAME` / `netlink.pass PASSWORD` | Set the Wi-Fi network or password and reconnect. |
| `netlink.wifi scan` / `netlink.wifi list [PAGE]` | Start a 2.4 GHz scan, then read the last successful result by descending RSSI. |
| `netlink.port PORT` | Set the listening UDP port (1–65535, default 4242). |
| `netlink.peer N KEY` | Add or replace the peer in slot N. Replacing the key resets that slot's options. |
| `netlink.peer N off` / `netlink.peer N on` | Disable or enable a configured peer, retaining its settings. |
| `netlink.del N` | Remove the slot and release its queue. |
| `netlink.addr N HOST [PORT]` / `netlink.addr N -` | Set a dial address (IPv4 or DNS), or listen passively. Default port 4242. |
| `netlink.snr N [dB]` | Read or set the incoming SNR, −32 to +31.75 dB, rounded to the nearest quarter dB. Default +8 dB. |
| `netlink.rssi N DBM` | Set the synthetic radio RSSI for received frames, −127 to 0, default −30. |
| `netlink.queue N` | Read the configured capacity and lifetime. |
| `netlink.queue N DEPTH SECONDS` / `netlink.queue N 0` | Set capacity (1–32) and lifetime (3–3600 s), or request unacknowledged delivery. Defaults: 8 frames, 60 s. |
| `netlink.ka SECONDS` | Set the common keepalive interval (1–3600 s, default 25). It must fit below every enabled peer timeout. |
| `netlink.timeout N [SECONDS]` | Read or set the receive timeout, greater than keepalive and at most 65535 s. Default 120 s; an existing longer keepalive uses keepalive + 1. |
| `netlink.status [N]` | Show the peer summary or detailed live state. |
| `netlink.diag [N]` | Show socket statistics, or the peer's queue eviction, expiry and offline discard counts. |
| `netlink.mem` | Show available heap, largest allocation, low watermark and low-memory reset indication. |

## Sessions and queues

A session is bound to the peer's key, boot identity and fresh handshake tokens. Capability bits travel in the existing authenticated control header. Two capable peers select `noack` for both directions if either end requests zero capacity. A legacy peer selects `legacy-ack`; a local zero setting then uses an eight-frame queue for that session. Changing between zero and nonzero requests a fresh handshake, including on a connected passive peer. Changing a positive capacity or lifetime takes effect locally.

Each reliable slot owns only the storage it needs. A full queue evicts the oldest frame. A reduction in capacity retains the newest queued frames. Administrative changes do not count as delivery failures. An unavailable passive peer has no queue, dial timer or outgoing probes. An active dialer can queue while disconnected. A disabled peer ignores incoming sessions and releases its queue. A capable neighbour can act on its authenticated close immediately; a lost close or a legacy neighbour falls back to the receive timeout.

A successful UDP send does not prove liveness. After the keepalive interval without a valid inbound session packet, the board sends a ping and repeats at that interval until the timeout. A lost local Wi-Fi link closes the socket and its sessions. NetLink never waits for a peer ACK in the LoRa loop.

`q` shows occupancy and actual allocated capacity. `tx` counts acknowledgements, including a delivery acknowledged after a retry. `rx` counts frames accepted into the local dispatcher. `drop` includes `full`, `expired` and `offline` (queued frames released when a passive link disappears). `rtt` is the latest unambiguous time in milliseconds from a first send to its ACK, including processing at the remote board. A retransmitted frame provides no RTT sample. `noack` has no queue, retry work or ACK timing. Counters live in RAM.

## Build and update

Build with `pio run -e heltec_v43_netlink`. The output is `.pio/build/heltec_v43_netlink/firmware.bin`. For this release, `ver` returns `v1.18.0-netlink.0.9.2 (Build: 25 Nov 2026)`.

For an existing Heltec V4.3 OLED with the upstream 16 MB partition map, run `start ota` and upload the application image as **Firmware**. For USB updates write only the application image to the confirmed bootable application slot. Slot 0 begins at `0x10000`; an OTA-updated device can boot slot 1. Preserve the boot selection, partition table, NVS and SPIFFS. Full erase, filesystem uploads and a changed partition map can remove the identity or settings. A blank board needs the correct bootloader and partition layout before an application-only image can boot.

The stock repeater preference loader imports older MeshCore settings. Existing identity, radio settings and ACL are retained; earlier NetLink NVS settings gain the defaults for new options. On a clean installation this profile starts at 869.525 MHz, BW 62.5 kHz, SF7, CR 4/7, TX 0, two-byte path hashes and a 10% airtime budget. Set suitable local radio parameters before deployment.

## Upstream integration

Track the upstream repeater tag and review the diff from that tag when rebasing. The `heltec_v43_netlink` environment extends the upstream Heltec repeater. The transport lives under `examples/simple_repeater/NetLink*`. Integration points are the TX completion and RX dispatcher, the authenticated repeater CLI, the main loop, and the OLED status page. CPU deep sleep is deferred while station Wi-Fi is configured or a scan is running; Wi-Fi modem sleep stays enabled. Upstream MeshCore licensing and notices apply.

The `trim` diagnostic counts frames discarded by a manual capacity reduction. A slot can be enabled when its timeout exceeds keepalive. The same application binary supports USB and OTA updates.
