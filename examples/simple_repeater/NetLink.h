#pragma once
#ifdef MESHCORE_NETLINK
#include <Mesh.h>

namespace netlink {
void begin(mesh::LocalIdentity &identity);
void poll(mesh::RTCClock &clock);
bool wifiOnline();
bool needsAwake();
uint16_t listenPort();
unsigned connectedPeers();
void transmit(const mesh::Packet &packet);
int receive(uint8_t *bytes, float &rssi, float &snr);
void accepted();
bool command(const char *line, char *reply);
bool firstInstall();
void finishInstall();
} // namespace netlink
#endif
