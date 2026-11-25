#ifdef MESHCORE_NETLINK
#include "NetLink.h"

#include "NetLinkQueue.h"

#include <Arduino.h>
#include <Crypto.h>
#include <Preferences.h>
#include <SHA256.h>
#include <Utils.h>
#include <WiFi.h>
#include <algorithm>
#include <atomic>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <mbedtls/platform_util.h>
#include <memory>

namespace netlink {
constexpr unsigned peersMax = 4, defaultDepth = 8, maxDepth = 32;
constexpr uint32_t capabilities = 0x4e4c0100, noAckFlag = 1;
enum Kind : uint8_t { Hello = 1, Welcome, Ping, Pong, Data, Ack, Close };
struct PeerConfig {
  uint8_t key[32];
  char host[72];
  uint16_t port;
  int8_t rssi;
  bool enabled;
};
struct ConfigV1 {
  uint32_t version;
  uint16_t port, keep;
  PeerConfig peer[peersMax];
  char ssid[33], pass[65];
};
struct PeerOptions {
  uint16_t lifetime, timeout;
  uint8_t depth;
  bool disabled;
};
// Keep the entire V1 blob (including tail padding) at offset zero.
struct Config {
  ConfigV1 base;
  PeerOptions options[peersMax];
};
static Config settings;
static ConfigV1 &cfg = settings.base;
static bool writableConfig = true;
static PeerOptions defaults(uint16_t keep) {
  return { 60, static_cast<uint16_t>(std::max<unsigned>(120, keep + 1)), defaultDepth, false };
}
static mesh::LocalIdentity *self;
static Preferences prefs;
// V1 integers are little endian. MAC covers header and payload, including direction.
struct __attribute__((packed)) Header {
  uint16_t magic;
  uint8_t version, kind;
  uint8_t sender[8];
  uint64_t boot, token, echo;
  uint32_t serial, frame, ackHigh, ackBits;
};
constexpr unsigned tagSize = 16;
struct Peer {
  uint8_t txKey[32], rxKey[32];
  sockaddr_in endpoint, dial;
  uint64_t local, remote, remoteBoot, challenge, candidate;
  uint32_t serial, highest, seen, seenBits;
  uint32_t lastRx, nextHello, dnsAt, ackDue, tx, rx, full, expired, offline;
  uint32_t queueDue, pingDue, retryUntil, rtt;
  uint16_t backoff;
  bool up, ackPending, noAck, modern, retryEndpoint, hasRtt, keyValid;
  FrameQueue queue;
};
static Peer peers[peersMax];
static uint64_t boot;
static uint32_t frameNumber;
static int sock = -1, socketError;
static uint32_t wireTx, wireRx, badTag;
static uint32_t wifiDue, socketDue;
static std::atomic<bool> wifiChanged{ true };
struct ScannedAP {
  char ssid[33];
  int8_t rssi;
};
static std::unique_ptr<ScannedAP[]> scannedAPs;
static int scannedCount = -1;
enum class ScanState { Idle, Running, Failed };
static ScanState scanState = ScanState::Idle;
static esp_err_t scanError = ESP_OK;
static uint32_t scanStarted, scanRetryAt;
static bool scanPausedConnect;

static void startScan(uint32_t now) {
  scanStarted = now;
  scanRetryAt = now;
  scanState = ScanState::Running;
  WiFi.scanDelete();
  scanError = esp_wifi_scan_start(nullptr, false);
  if (scanError == ESP_ERR_WIFI_STATE && WiFi.status() != WL_CONNECTED) {
    // The driver rejects scans during association. Pause only the offline attempt.
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    scanPausedConnect = true;
    scanRetryAt = now + 250;
  } else if (scanError != ESP_OK) {
    scanState = ScanState::Failed;
  }
}

static void pollScan(uint32_t now) {
  if (scanState != ScanState::Running) return;
  if (scanError == ESP_ERR_WIFI_STATE) {
    if (int32_t(now - scanRetryAt) < 0) return;
    scanError = esp_wifi_scan_start(nullptr, false);
    scanRetryAt = now + 250;
    if (scanError == ESP_ERR_WIFI_STATE && now - scanStarted < 3000) return;
    if (scanError != ESP_OK) {
      scanState = ScanState::Failed;
      return;
    }
  }
  // Arduino collects the records on WIFI_EVENT_SCAN_DONE, including native scans.
  const int count = WiFi.scanComplete();
  if (count < 0 && now - scanStarted < 15000) return;
  scanState = ScanState::Failed;
  if (count < 0) {
    esp_wifi_scan_stop();
    scanError = ESP_ERR_TIMEOUT;
    return;
  }
  // The Arduino API accepts an eight-bit AP index.
  const int n = std::min(count, 256);
  std::unique_ptr<ScannedAP[]> result(n ? new (std::nothrow) ScannedAP[n] : nullptr);
  if (n && !result) {
    WiFi.scanDelete();
    scanError = ESP_ERR_NO_MEM;
    return;
  }
  for (int i = 0; i < n; ++i) {
    auto &ap = result[i];
    WiFi.SSID(i).toCharArray(ap.ssid, sizeof ap.ssid);
    for (char *p = ap.ssid; *p; ++p)
      if ((uint8_t)*p < 32 || *p == 127) *p = '?';
    ap.rssi = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  if (n > 1)
    std::sort(result.get(), result.get() + n,
              [](const ScannedAP &a, const ScannedAP &b) { return a.rssi > b.rssi; });
  scannedAPs = std::move(result);
  scannedCount = n;
  scanState = ScanState::Idle;
}
static volatile uint32_t syncTime;
static bool synced;
static struct {
  int slot = -1;
  uint32_t number, at;
  uint8_t len, bytes[MAX_TRANS_UNIT];
} incoming;
static uint64_t nonce() {
  uint64_t value;
  do {
    esp_fill_random(&value, sizeof value);
  } while (!value);
  return value;
}
static bool due(uint32_t now, uint32_t when) {
  return int32_t(now - when) >= 0;
}
static void resetLink(Peer &p) {
  p.up = false;
  p.remote = 0;
  p.challenge = 0;
  p.local = nonce();
  p.highest = 0;
  p.backoff = 1;
  p.nextHello = millis();
  p.ackPending = false;
  p.noAck = false;
  p.modern = false;
  p.hasRtt = false;
  p.retryEndpoint = false;
}
static bool enabled(unsigned i) {
  return cfg.peer[i].enabled && !settings.options[i].disabled;
}
static void disconnected(unsigned i) {
  auto &p = peers[i];
  if (incoming.slot == int(i)) incoming.slot = -1;
  resetLink(p);
  p.endpoint = {};
  if (!cfg.peer[i].host[0] || !settings.options[i].depth) {
    p.offline += p.queue.count();
    p.queue.allocate(0);
  }
}
static void initPeer(unsigned i) {
  auto &p = peers[i];
  p = Peer{};
  if (!enabled(i)) return;
  uint8_t secret[32], context[40] = "NetLink1";
  self->calcSharedSecret(secret, cfg.peer[i].key);
  if (mesh::Utils::isZeroes(secret, sizeof secret)) return;
  p.keyValid = true;
  memcpy(context + 8, self->pub_key, 32);
  hmac<SHA256>(p.txKey, sizeof p.txKey, secret, sizeof secret, context, sizeof context);
  memcpy(context + 8, cfg.peer[i].key, 32);
  hmac<SHA256>(p.rxKey, sizeof p.rxKey, secret, sizeof secret, context, sizeof context);
  mbedtls_platform_zeroize(secret, sizeof secret);
  resetLink(p);
  p.dnsAt = millis();
}
// Blocking resolver lives outside the mesh loop. Discard outdated answers after edits.
struct Lookup {
  unsigned slot;
  char host[72];
  uint32_t ip;
};
static QueueHandle_t requests, answers;
static void resolver(void *) {
  Lookup q;
  for (;;)
    if (xQueueReceive(requests, &q, portMAX_DELAY)) {
      IPAddress ip;
      q.ip = WiFi.hostByName(q.host, ip) == 1 ? uint32_t(ip) : 0;
      xQueueSend(answers, &q, 0);
    }
}
static bool ensureResolver() {
  if (requests) return true;
  requests = xQueueCreate(peersMax, sizeof(Lookup));
  answers = xQueueCreate(peersMax, sizeof(Lookup));
  if (requests && answers && xTaskCreate(resolver, "netlink-dns", 4096, nullptr, 1, nullptr) == pdPASS)
    return true;
  if (requests) vQueueDelete(requests);
  if (answers) vQueueDelete(answers);
  requests = answers = nullptr;
  return false;
}
static sockaddr_in address(uint32_t ip, uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = ip;
  a.sin_port = htons(port);
  return a;
}
static void resolve(unsigned i, uint32_t now) {
  auto &c = cfg.peer[i];
  auto &p = peers[i];
  if (!c.host[0] || !due(now, p.dnsAt)) return;
  IPAddress ip;
  if (ip.fromString(c.host))
    p.dial = address(uint32_t(ip), c.port);
  else {
    Lookup q{};
    q.slot = i;
    strlcpy(q.host, c.host, sizeof q.host);
    if (!ensureResolver() || !xQueueSend(requests, &q, 0)) return;
  }
  p.dnsAt = now + 60000;
}
static bool sendMessage(unsigned i, Kind kind, const sockaddr_in &dest, uint64_t echo,
                        const Frame *f = nullptr, uint64_t challenge = 0) {
  if (sock < 0 || !dest.sin_port) return false;
  Peer &p = peers[i];
  Header h{ 0x4c4e, 1, kind };
  memcpy(h.sender, self->pub_key, 8);
  h.boot = boot;
  h.token = challenge ? challenge : p.local;
  h.echo = echo;
  h.serial = ++p.serial;
  if (f)
    h.frame = f->number;
  else if (kind == Hello || kind == Welcome || kind == Pong)
    h.frame = capabilities | (settings.options[i].depth ? 0 : noAckFlag);
  if (!p.noAck) {
    h.ackHigh = p.seen;
    h.ackBits = p.seenBits;
  }
  uint8_t bytes[sizeof h + MAX_TRANS_UNIT + tagSize], digest[32];
  memcpy(bytes, &h, sizeof h);
  size_t len = sizeof h;
  if (f) {
    memcpy(bytes + len, f->bytes, f->size);
    len += f->size;
  }
  hmac<SHA256>(digest, tagSize, p.txKey, sizeof p.txKey, bytes, len);
  memcpy(bytes + len, digest, tagSize);
  if (sendto(sock, bytes, len + tagSize, MSG_DONTWAIT, (const sockaddr *)&dest, sizeof dest) !=
      int(len + tagSize)) {
    socketError = errno;
    return false;
  }
  ++wireTx;
  if (kind != Welcome && kind != Hello) p.ackPending = false;
  return true;
}
static bool inWindow(uint32_t n, uint32_t top, uint32_t bits) {
  uint32_t delta = top - n;
  return delta < 32 && (bits & (uint32_t(1) << delta));
}
static void scheduleAck(Peer &p) {
  if (!p.ackPending) p.ackDue = millis() + 15;
  p.ackPending = true;
}
void accepted() {
  if (incoming.slot < 0) return;
  auto &p = peers[incoming.slot];
  auto n = incoming.number;
  if (p.noAck) {
    ++p.rx;
    incoming.slot = -1;
    return;
  }
  if (n > p.seen) {
    auto delta = n - p.seen;
    p.seenBits = delta < 32 ? p.seenBits << delta : 0;
    p.seen = n;
  }
  if (p.seen - n < 32) p.seenBits |= uint32_t(1) << (p.seen - n);
  ++p.rx;
  scheduleAck(p);
  incoming.slot = -1;
}
int receive(uint8_t *raw, float &rssi) {
  if (incoming.slot < 0) return 0;
  memcpy(raw, incoming.bytes, incoming.len);
  rssi = cfg.peer[incoming.slot].rssi;
  return incoming.len;
}
static void closeSocket();
static void readMessage(uint32_t now) {
  // One extra byte detects oversized/truncated datagrams.
  uint8_t bytes[sizeof(Header) + MAX_TRANS_UNIT + tagSize + 1];
  sockaddr_in src{};
  socklen_t sl = sizeof src;
  int len = recvfrom(sock, bytes, sizeof bytes, MSG_DONTWAIT, (sockaddr *)&src, &sl);
  if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
    socketError = errno;
    closeSocket();
    return;
  }
  if (len < int(sizeof(Header) + tagSize) || len == sizeof bytes) return;
  ++wireRx;
  Header h;
  memcpy(&h, bytes, sizeof h);
  int size = len - sizeof h - tagSize;
  if (h.magic != 0x4c4e || h.version != 1 || !h.token || h.kind < Hello || h.kind > Close ||
      (h.kind == Data ? size < 2 || !h.frame : size != 0))
    return;
  for (unsigned i = 0; i < peersMax; ++i) {
    if (!enabled(i) || !peers[i].keyValid || memcmp(h.sender, cfg.peer[i].key, 8)) continue;
    Peer &p = peers[i];
    uint8_t digest[32];
    hmac<SHA256>(digest, tagSize, p.rxKey, sizeof p.rxKey, bytes, len - tagSize);
    if (!secure_compare(bytes + len - tagSize, digest, tagSize)) {
      ++badTag;
      continue;
    }
    if (h.kind == Hello) {
      // A hello may solicit a challenge, but cannot move an established endpoint.
      if (p.up && (h.token != p.remote || h.boot != p.remoteBoot)) {
        if (!p.challenge || p.candidate != h.token) {
          p.challenge = nonce();
          p.candidate = h.token;
        }
        sendMessage(i, Welcome, src, h.token, nullptr, p.challenge);
      } else
        sendMessage(i, Welcome, src, h.token);
      return;
    }
    if (p.challenge && h.echo == p.challenge && h.token == p.candidate) {
      if (h.kind != Pong) {
        // The confirmation may have been lost. Only the matching Pong commits the switch.
        if (h.kind == Ping || h.kind == Data) sendMessage(i, Welcome, src, h.token, nullptr, p.challenge);
        return;
      }
      p.local = p.challenge;
      p.challenge = 0;
      p.up = false;
      p.highest = 0;
      if (incoming.slot == int(i)) incoming.slot = -1;
    }
    if (h.echo != p.local) return;
    if (p.up && (p.remote != h.token || p.remoteBoot != h.boot)) return;
    if (h.serial <= p.highest) return; // lost/reordered DATA is resent with a fresh serial
    if (h.kind == Close) {
      if (p.up && p.modern) disconnected(i);
      return;
    }
    if (!p.up && h.kind != Welcome && h.kind != Pong) {
      sendMessage(i, Welcome, src, h.token);
      return;
    }
    p.highest = h.serial;
    if (!p.up) {
      const auto &opt = settings.options[i];
      p.modern = (h.frame & ~noAckFlag) == capabilities;
      p.noAck = p.modern && (!opt.depth || (h.frame & noAckFlag));
      unsigned depth = p.noAck ? 0 : (opt.depth ? opt.depth : defaultDepth);
      if (p.queue.capacity() != depth && !p.queue.allocate(depth)) {
        disconnected(i);
        return;
      }
      p.retryEndpoint = false;
      if (p.remoteBoot != h.boot) {
        p.seen = p.seenBits = 0;
      }
      p.remoteBoot = h.boot;
      p.remote = h.token;
      p.up = true;
      for (auto &f : p.queue)
        if (f.size) f.due = now;
      p.queueDue = now;
    }
    p.endpoint = src;
    p.lastRx = now;
    p.pingDue = now + uint32_t(cfg.keep) * 1000;
    if (h.kind == Welcome || h.kind == Ping) sendMessage(i, Pong, src, p.remote);
    if (h.kind == Welcome) return;
    if (!p.noAck && p.queue.count())
      for (auto &f : p.queue)
        if (f.size && f.attempts && inWindow(f.number, h.ackHigh, h.ackBits)) {
          if (f.attempts == 1) {
            p.rtt = now - f.firstSent;
            p.hasRtt = true;
          }
          p.queue.remove(f);
          ++p.tx;
          p.queueDue = now;
        }
    if (h.kind != Data) return;
    if (!p.noAck && inWindow(h.frame, p.seen, p.seenBits)) {
      scheduleAck(p);
      return;
    }
    if ((!p.noAck && p.seen >= h.frame && p.seen - h.frame >= 32) || incoming.slot >= 0) return;
    incoming.slot = i;
    incoming.number = h.frame;
    incoming.at = now;
    incoming.len = size;
    memcpy(incoming.bytes, bytes + sizeof h, size);
    return;
  }
}
void transmit(const mesh::Packet &packet) {
  if (!self || packet.getRawLength() > MAX_TRANS_UNIT) return;
  uint32_t now = millis();
  if (!++frameNumber) ++frameNumber; // Zero is reserved for control messages.
  for (unsigned i = 0; i < peersMax; ++i) {
    auto &p = peers[i];
    if (!enabled(i) || !p.keyValid || (!p.up && !cfg.peer[i].host[0])) continue;
    if (p.noAck) {
      Frame f;
      f.number = frameNumber;
      f.size = packet.writeTo(f.bytes);
      sendMessage(i, Data, p.endpoint, p.remote, &f);
    } else if (p.queue.capacity()) {
      if (p.queue.count() == p.queue.capacity()) ++p.full;
      Frame &f = p.queue.insert(frameNumber);
      f.born = f.due = p.queueDue = now;
      f.attempts = 0;
      f.size = packet.writeTo(f.bytes);
    }
  }
}

static void serviceQueue(unsigned i, uint32_t now) {
  auto &p = peers[i];
  const uint32_t ttl = uint32_t(settings.options[i].lifetime) * 1000;
  uint32_t wait = ttl;
  Frame *oldest = nullptr;
  for (auto &f : p.queue) {
    if (!f.size) continue;
    uint32_t age = now - f.born;
    if (age >= ttl) {
      p.queue.remove(f);
      ++p.expired;
      continue;
    }
    wait = std::min(wait, ttl - age);
    if (!oldest || int32_t(f.number - oldest->number) < 0) oldest = &f;
  }
  if (p.up && oldest) {
    bool sent = false;
    for (auto &f : p.queue) {
      // A newer delivery must not push an outstanding frame out of the ACK bitmap.
      if (!f.size || f.number - oldest->number >= 32) continue;
      if (!sent && due(now, f.due)) {
        sent = true;
        if (sendMessage(i, Data, p.endpoint, p.remote, &f)) {
          if (!f.attempts) f.firstSent = now;
          f.due = now + (320u << std::min<unsigned>(f.attempts, 4));
          if (f.attempts < 5) ++f.attempts;
        } else
          f.due = now + 200;
      }
      wait = std::min(wait, due(now, f.due) ? 0u : f.due - now);
    }
  }
  p.queueDue = now + wait;
}

static void service(unsigned i, uint32_t now) {
  auto &p = peers[i];
  const auto &c = cfg.peer[i];
  if (!p.keyValid || (!p.up && !c.host[0] && !p.retryEndpoint)) return;
  if (p.up && now - p.lastRx >= uint32_t(settings.options[i].timeout) * 1000) disconnected(i);
  if (p.queue.count() && due(now, p.queueDue)) serviceQueue(i, now);
  if (sock < 0) return;
  if (!p.up) {
    if (p.retryEndpoint && due(now, p.retryUntil)) {
      p.retryEndpoint = false;
      p.endpoint = {};
    }
    if (!c.host[0] && !p.retryEndpoint) return;
    if (!due(now, p.nextHello)) return;
    if (c.host[0] && settings.options[i].depth && !p.queue.capacity() &&
        !p.queue.allocate(settings.options[i].depth)) {
      p.nextHello = now + 5000;
      return;
    }
    resolve(i, now);
    const auto &target = p.retryEndpoint ? p.endpoint : p.dial;
    if (target.sin_port) sendMessage(i, Hello, target, 0);
    p.nextHello = now + p.backoff * 1000;
    p.backoff = std::min<unsigned>(p.backoff * 2, 16);
    return;
  }
  if (due(now, p.pingDue)) {
    sendMessage(i, Ping, p.endpoint, p.remote);
    p.pingDue = now + uint32_t(cfg.keep) * 1000;
  } else if (p.ackPending && due(now, p.ackDue)) {
    if (!sendMessage(i, Ack, p.endpoint, p.remote)) p.ackDue = now + 200;
  }
}
static void closeSocket() {
  if (sock >= 0) {
    close(sock);
    sock = -1;
  }
  incoming.slot = -1;
  for (unsigned i = 0; i < peersMax; ++i)
    if (enabled(i)) disconnected(i);
}
static void connectWifi() {
  if (cfg.ssid[0]) WiFi.begin(cfg.ssid, cfg.pass);
  wifiDue = millis() + 30000;
}
bool firstInstall() {
  prefs.begin("netlink", false);
  return !prefs.getBool("migrated", false);
}
void finishInstall() {
  prefs.putBool("migrated", true);
}
void begin(mesh::LocalIdentity &identity) {
  self = &identity;
  boot = nonce();
  cfg.version = 2;
  cfg.port = 4242;
  cfg.keep = 25;
  const size_t stored = prefs.getBytesLength("config");
  if (stored) {
    Config saved{};
    if ((stored == sizeof(ConfigV1) || stored == sizeof(Config)) &&
        prefs.getBytes("config", &saved, stored) == stored &&
        ((stored == sizeof(ConfigV1) && saved.base.version == 1) ||
         (stored == sizeof(Config) && saved.base.version == 2))) {
      settings = saved;
      cfg.version = 2;
    } else
      writableConfig = false; // Never replace an unknown schema with defaults.
  } else {
    // V1 stored version, role[8], ssid[33], wifiPass[65] at the start of its blob.
    Preferences legacy;
    if (legacy.begin("mc-tunnel", true)) {
      size_t size = legacy.getBytesLength("config");
      uint8_t bytes[320] = {};
      if (size >= 110 && size <= sizeof bytes && legacy.getBytes("config", bytes, size) == size &&
          bytes[0] == 1 && memchr(bytes + 12, 0, 33) && memchr(bytes + 45, 0, 65)) {
        strlcpy(cfg.ssid, (char *)bytes + 12, sizeof cfg.ssid);
        strlcpy(cfg.pass, (char *)bytes + 45, sizeof cfg.pass);
      }
      memset(bytes, 0, sizeof bytes);
    }
  }
  for (unsigned i = 0; i < peersMax; ++i) {
    if (stored != sizeof(Config) || !writableConfig) settings.options[i] = defaults(cfg.keep);
    initPeer(i);
    if (enabled(i) && cfg.peer[i].host[0]) peers[i].queue.allocate(settings.options[i].depth);
  }
  WiFi.onEvent([](WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED || event == ARDUINO_EVENT_WIFI_STA_LOST_IP ||
        event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
      wifiChanged.store(true);
  });
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(true);
  sntp_set_time_sync_notification_cb([](timeval *tv) { syncTime = tv->tv_sec; });
  configTime(0, 0, "pool.ntp.org");
  connectWifi();
  esp_task_wdt_init(15, true);
  enableLoopWDT();
}
static RTC_NOINIT_ATTR uint32_t memoryReset;
static void checkMemory(uint32_t now) {
  static uint32_t next = 30000;
  static unsigned low = 0;
  if (!due(now, next)) return;
  next = now + 5000;
  if (ESP.getFreeHeap() >= 24576 && ESP.getMaxAllocHeap() >= 8192) {
    low = 0;
    return;
  }
  if (++low == 4) closeSocket();
  if (low >= 8) {
    memoryReset = 0x4e4c4d45;
    ESP.restart();
  }
}
bool wifiOnline() {
  return WiFi.status() == WL_CONNECTED && uint32_t(WiFi.localIP()) != 0;
}
bool needsAwake() {
  return cfg.ssid[0] || scanState == ScanState::Running;
}

uint16_t listenPort() {
  return cfg.port;
}

unsigned connectedPeers() {
  unsigned count = 0;
  for (const Peer &peer : peers) {
    if (peer.up) ++count;
  }
  return count;
}

void poll(mesh::RTCClock &clock) {
  static uint32_t next;
  uint32_t now = millis();
  if (!due(now, next)) return;
  next = now + 5;
  pollScan(now);
  if (scanPausedConnect && scanState != ScanState::Running) {
    WiFi.setAutoReconnect(true);
    scanPausedConnect = false;
    wifiDue = now;
  }
  checkMemory(now);
  if (syncTime) {
    clock.setCurrentTime(syncTime);
    synced = true;
    syncTime = 0;
  }
  static bool online = false;
  static uint32_t checkWifiAt = 0;
  if (wifiChanged.exchange(false) || due(now, checkWifiAt)) {
    online = wifiOnline();
    checkWifiAt = now + 1000;
  }
  if (!online) {
    if (sock >= 0) closeSocket();
    if (scanState != ScanState::Running && due(now, wifiDue)) connectWifi();
  } else if (sock < 0 && due(now, socketDue)) {
    socketDue = now + 1000;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
      auto a = address(INADDR_ANY, cfg.port);
      if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && bind(fd, (sockaddr *)&a, sizeof a) == 0)
        sock = fd;
      else {
        socketError = errno;
        close(fd);
      }
    }
  }
  Lookup result;
  if (answers && xQueueReceive(answers, &result, 0) && result.slot < peersMax) {
    auto &c = cfg.peer[result.slot];
    if (enabled(result.slot) && !strcmp(result.host, c.host)) {
      if (result.ip)
        peers[result.slot].dial = address(result.ip, c.port);
      else
        peers[result.slot].dnsAt = now + 5000;
    }
  }
  if (incoming.slot >= 0 && now - incoming.at > 1000) incoming.slot = -1;
  if (sock >= 0) readMessage(now);
  for (unsigned i = 0; i < peersMax; ++i)
    if (enabled(i)) service(i, now);
}
static bool number(const char *s, long min, long max, long &out) {
  if (!s || !*s) return false;
  char *end;
  out = strtol(s, &end, 10);
  return !*end && out >= min && out <= max;
}
static bool decodeKey(const char *text, uint8_t *key) {
  if (!text || strlen(text) != 64) return false;
  for (const char *p = text; *p; ++p)
    if (!mesh::Utils::isHexChar(*p)) return false;
  return mesh::Utils::fromHex(key, 32, text) && memcmp(key, self->pub_key, 32) != 0;
}
bool command(const char *line, char *reply) {
  if (strncmp(line, "netlink.", 8)) return false;
  char args[160];
  strlcpy(args, line + 8, sizeof args);
  char *cursor = nullptr;
  char *verb = strtok_r(args, " ", &cursor);
  auto arg = [&]() { return strtok_r(nullptr, " ", &cursor); };
  char *first = arg();
  long value;
  strcpy(reply, "ERR args");
  if (!verb) return true;
  if (!strcmp(verb, "id")) {
    for (int j = 0; j < 32; ++j)
      sprintf(reply + j * 2, "%02x", self->pub_key[j]);
    return true;
  }
  if (!strcmp(verb, "wifi")) {
    if (first && !strcmp(first, "scan")) {
      if (arg()) return true;
      if (scanState != ScanState::Running) {
        startScan(millis());
      }
      if (scanState == ScanState::Running)
        strcpy(reply, "scanning; netlink.wifi list");
      else
        snprintf(reply, 150, "scan failed %s", esp_err_to_name(scanError));
      return true;
    }
    if (first && !strcmp(first, "list")) {
      char *pageArg = arg();
      long page = 1;
      if ((pageArg && !number(pageArg, 1, 86, page)) || arg()) return true;
      const char *state = scanState == ScanState::Running  ? "scanning"
                          : scanState == ScanState::Failed ? "scan failed"
                                                           : "";
      if (scannedCount < 0) {
        if (scanState == ScanState::Failed)
          snprintf(reply, 150, "scan failed %s", esp_err_to_name(scanError));
        else
          strcpy(reply, *state ? state : "no scan; netlink.wifi scan");
        return true;
      }
      if (!scannedCount) {
        snprintf(reply, 150, "%s%s0 networks", state, *state ? "; last: " : "");
        return true;
      }
      // Keep page boundaries stable while a new scan is pending. Reserve space
      // for the header, the reflected CLI prefix and the terminating NUL.
      constexpr size_t pageBytes = 136;
      int pages = 1;
      size_t used = 0;
      int start = -1, end = 0;
      for (int i = 0; i < scannedCount; ++i) {
        const auto &ap = scannedAPs[i];
        const size_t bytes = snprintf(nullptr, 0, "\n%d %s", ap.rssi, ap.ssid);
        if (used + bytes > pageBytes) {
          ++pages;
          used = 0;
        }
        used += bytes;
        if (pages == page) {
          if (start < 0) start = i;
          end = i + 1;
        }
      }
      if (page > pages) return true;
      const char *prefix = scanState == ScanState::Running  ? "busy "
                           : scanState == ScanState::Failed ? "error "
                                                            : "";
      used = snprintf(reply, 156, "%s%ld/%d dBm", prefix, page, pages);
      for (int i = start; i < end; ++i) {
        used += snprintf(reply + used, 156 - used, "\n%d %s", scannedAPs[i].rssi, scannedAPs[i].ssid);
      }
      return true;
    }
    if (first) return true;
    if (wifiOnline()) {
      snprintf(reply, 150, "up ssid=%s ip=%s rssi=%ld dBm", WiFi.SSID().c_str(),
               WiFi.localIP().toString().c_str(), (long)WiFi.RSSI());
    } else {
      snprintf(reply, 150, "%s ssid=%s", WiFi.status() == WL_CONNECTED ? "waiting for IP" : "no link",
               cfg.ssid[0] ? cfg.ssid : "-");
    }
    return true;
  }
  if (!strcmp(verb, "status")) {
    if (!first) {
      snprintf(reply, 150, "%s:%u ntp=%s", WiFi.localIP().toString().c_str(), cfg.port, synced ? "ok" : "-");
      for (unsigned i = 0; i < peersMax; ++i)
        if (cfg.peer[i].enabled) {
          size_t used = strlen(reply);
          auto &p = peers[i];
          snprintf(reply + used, 150 - used, "\n%u %02X%02X %s q%u", i + 1, cfg.peer[i].key[0],
                   cfg.peer[i].key[1],
                   settings.options[i].disabled ? "off"
                   : p.up                       ? "up"
                                                : "down",
                   p.queue.count());
        }
    } else if (number(first, 1, peersMax, value)) {
      unsigned i = value - 1;
      const auto &p = peers[i];
      if (!cfg.peer[i].enabled)
        strcpy(reply, "empty");
      else if (!enabled(i))
        strcpy(reply, "off");
      else {
        char tx[12] = "-", drop[12] = "-", rtt[12] = "-";
        if (!p.noAck && (settings.options[i].depth || p.up)) {
          snprintf(tx, sizeof tx, "%lu", (unsigned long)p.tx);
          snprintf(drop, sizeof drop, "%lu", (unsigned long)(p.full + p.expired + p.offline));
          if (p.hasRtt) snprintf(rtt, sizeof rtt, "%lu", (unsigned long)p.rtt);
        }
        const char *mode = !p.up ? "-" : p.noAck ? "noack" : p.modern ? "ack" : "legacy-ack";
        snprintf(reply, 156, "%s %s:%u age=%lus q=%u/%u tx=%s rx=%lu drop=%s rtt=%s mode=%s rssi=%d ka=%u",
                 p.up ? "up" : "down", IPAddress(p.endpoint.sin_addr.s_addr).toString().c_str(),
                 ntohs(p.endpoint.sin_port), (unsigned long)((millis() - p.lastRx) / 1000), p.queue.count(),
                 p.queue.capacity(), tx, (unsigned long)p.rx, drop, rtt, mode, cfg.peer[i].rssi, cfg.keep);
      }
    }
    return true;
  }
  if (!strcmp(verb, "diag")) {
    if (first) {
      if (!number(first, 1, peersMax, value) || arg()) return true;
      unsigned i = value - 1;
      const Peer &p = peers[i];
      if (!cfg.peer[i].enabled)
        strcpy(reply, "empty");
      else if (p.noAck)
        strcpy(reply, "full=- expired=- offline=-");
      else
        snprintf(reply, 100, "full=%lu expired=%lu offline=%lu", (unsigned long)p.full,
                 (unsigned long)p.expired, (unsigned long)p.offline);
      return true;
    }
    snprintf(reply, 140, "udp=%s out=%lu in=%lu badmac=%lu err=%d", sock >= 0 ? "up" : "down",
             (unsigned long)wireTx, (unsigned long)wireRx, (unsigned long)badTag, socketError);
    return true;
  }
  if (!strcmp(verb, "mem")) {
    snprintf(reply, 120, "heap=%u largest=%u min=%u reset=%s", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
             ESP.getMinFreeHeap(), memoryReset == 0x4e4c4d45 ? "lowmem" : "-");
    return true;
  }
  Config updated = settings;
  ConfigV1 &next = updated.base;
  int change = -1;
  bool wifi = false, rebind = false;
  if (!strcmp(verb, "port") && number(first, 1, 65535, value)) {
    next.port = value;
    rebind = true;
  } else if (!strcmp(verb, "ka") && number(first, 1, 3600, value)) {
    for (unsigned i = 0; i < peersMax; ++i)
      if (cfg.peer[i].enabled && value >= settings.options[i].timeout) {
        snprintf(reply, 64, "ERR timeout %u", i + 1);
        return true;
      }
    next.keep = value;
  } else if (!strcmp(verb, "ssid") || !strcmp(verb, "pass")) {
    const char *text = strchr(line, ' ');
    if (!text) return true;
    ++text;
    bool pass = !strcmp(verb, "pass");
    if (strlen(text) >= (pass ? sizeof cfg.pass : sizeof cfg.ssid)) return true;
    strcpy(pass ? next.pass : next.ssid, text);
    wifi = true;
  } else {
    if (!number(first, 1, peersMax, value)) return true;
    unsigned i = value - 1;
    auto &c = next.peer[i];
    char *item = arg();
    PeerOptions &opt = updated.options[i];
    if (!strcmp(verb, "peer") && item && (!strcmp(item, "on") || !strcmp(item, "off"))) {
      if (!c.enabled || arg()) return true;
      opt.disabled = !strcmp(item, "off");
    } else if (!strcmp(verb, "peer")) {
      uint8_t key[32];
      if (!decodeKey(item, key)) return true;
      for (unsigned j = 0; j < peersMax; ++j)
        if (j != i && next.peer[j].enabled && !memcmp(key, next.peer[j].key, 32)) {
          strcpy(reply, "ERR duplicate");
          return true;
        }
      if (!c.enabled || memcmp(key, c.key, 32)) {
        c = {};
        c.port = 4242;
        c.rssi = -30;
        opt = defaults(cfg.keep);
      }
      memcpy(c.key, key, 32);
      c.enabled = true;
    } else if (!strcmp(verb, "del"))
      c = {};
    else if (!c.enabled)
      return true;
    else if (!strcmp(verb, "queue")) {
      if (!item) {
        snprintf(reply, 48, "%u %us", opt.depth, opt.lifetime);
        return true;
      }
      if (!number(item, 0, maxDepth, value)) return true;
      opt.depth = value;
      char *ttl = arg();
      if (opt.depth || ttl) {
        if (!number(ttl, 3, 3600, value)) return true;
        opt.lifetime = value;
      }
      if (arg()) return true;
    } else if (!strcmp(verb, "timeout")) {
      if (!item) {
        snprintf(reply, 32, "%us", opt.timeout);
        return true;
      }
      if (!number(item, cfg.keep + 1, 65535, value) || arg()) return true;
      opt.timeout = value;
    } else if (!strcmp(verb, "rssi") && number(item, -127, 0, value))
      c.rssi = value;
    else if (!strcmp(verb, "addr") && item && strlen(item) < sizeof c.host) {
      char *port = arg();
      value = 4242;
      if (port && !number(port, 1, 65535, value)) return true;
      strcpy(c.host, !strcmp(item, "-") ? "" : item);
      c.port = value;
    } else
      return true;
    change = i;
  }
  if (!writableConfig) {
    strcpy(reply, "ERR config version");
    return true;
  }
  if (!memcmp(&updated, &settings, sizeof updated)) {
    strcpy(reply, "OK");
    return true;
  }
  FrameQueue replacement;
  bool reinit = false, renegotiate = false, resize = false, addressChanged = false;
  unsigned i = change < 0 ? 0 : unsigned(change);
  Peer &p = peers[i];
  if (change >= 0) {
    const auto &c = next.peer[i];
    const auto &opt = updated.options[i];
    reinit = c.enabled != cfg.peer[i].enabled || memcmp(c.key, cfg.peer[i].key, 32) ||
             opt.disabled != settings.options[i].disabled;
    renegotiate = !reinit && (!opt.depth) != (!settings.options[i].depth);
    addressChanged = strcmp(c.host, cfg.peer[i].host) || c.port != cfg.peer[i].port;
    unsigned target = 0;
    if (c.enabled && !opt.disabled && (c.host[0] || (!reinit && p.up))) {
      target = opt.depth;
      if (!reinit && p.up && !renegotiate) target = p.noAck ? 0 : opt.depth ? opt.depth : defaultDepth;
    }
    resize = reinit || target != p.queue.capacity();
    if (resize && !replacement.allocate(target)) {
      strcpy(reply, "ERR memory");
      return true;
    }
    if (resize && !reinit && !renegotiate) replacement.retainNewest(p.queue);
  }
  if (prefs.putBytes("config", &updated, sizeof updated) != sizeof updated) {
    strcpy(reply, "ERR NVS");
    return true;
  }
  if (reinit && p.up && p.modern) sendMessage(i, Close, p.endpoint, p.remote);
  const bool wasUp = p.up;
  const uint16_t oldKeep = cfg.keep;
  settings = updated;
  if (change >= 0) {
    if (reinit) {
      if (incoming.slot == change) incoming.slot = -1;
      initPeer(i);
    } else if (renegotiate) {
      if (incoming.slot == change) incoming.slot = -1;
      resetLink(p);
      p.retryEndpoint = wasUp;
      p.retryUntil = millis() + uint32_t(settings.options[i].timeout) * 1000;
    }
    if (resize) p.queue = std::move(replacement);
    p.queueDue = millis();
    if (addressChanged) {
      p.dial = {};
      p.dnsAt = p.nextHello = millis();
    }
  }
  if (cfg.keep != oldKeep)
    for (auto &peer : peers)
      peer.pingDue = peer.lastRx + uint32_t(cfg.keep) * 1000;
  if (rebind || wifi) closeSocket();
  if (wifi) {
    WiFi.disconnect();
    connectWifi();
  }
  strcpy(reply, "OK");
  return true;
}
} // namespace netlink
#endif
