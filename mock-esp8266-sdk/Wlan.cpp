// ESP8266WiFi-style host implementation using NetworkManager/libnm.
// Requires the matching ESP8266WiFi.h and the host project's Arduino.h.
// All callbacks and state are serviced on the calling thread's default GLib
// main context. This implementation is deliberately single-threaded.

#include "Arduino.h"
#include <NetworkManager.h>
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// ESP8266 Arduino's historic encryptionType() values.
constexpr uint8_t kEncWep = 5;
constexpr uint8_t kEncTkip = 2;
constexpr uint8_t kEncCcmp = 4;
constexpr uint8_t kEncNone = 7;
constexpr uint8_t kEncAuto = 8;
constexpr gint64 kScanTimeoutUs = 20 * G_USEC_PER_SEC;
constexpr gint64 kApTimeoutUs = 20 * G_USEC_PER_SEC;

bool loopback_wifi() {
  const char *value = std::getenv("MOCK_WIFI_LOOPBACK");
  return value && std::strcmp(value, "1") == 0;
}

struct Network {
  std::string ssid;
  int32_t rssi = -100; // Estimate: NM gives signal strength %, not dBm.
  int32_t channel = 0;
  uint8_t encryption = kEncNone;
};

struct State {
  NMClient *client = nullptr;        // Owned; retained for process lifetime.
  NMDeviceWifi *device = nullptr;    // Borrowed from client.
  NMActiveConnection *sta = nullptr; // Owned reference from async finish.
  NMActiveConnection *ap = nullptr;  // Owned reference from async finish.

  bool saveToDisk = false;
  bool autoReconnect = true;
  bool staPending = false;
  bool apPending = false;
  wl_status_t staStatus = WL_DISCONNECTED;
  guint64 stationEpoch = 0;
  guint64 apEpoch = 0;
  std::string ssid;
  std::string passphrase;
  int32_t requestedChannel = 0;
  std::string requestedBssid;
  std::string createdStationUuid;
  std::string apSsid;
  int apChannel = 1;

  bool stationStatic = false;
  IPAddress stationIp, stationGateway, stationSubnet, stationDns1, stationDns2;
  IPAddress hotspotIp = IPAddress(192, 168, 4, 1);
  IPAddress hotspotSubnet = IPAddress(255, 255, 255, 0);

  int scanResult = 0;
  bool scanRunning = false;
  bool scanRequestAccepted = false;
  guint64 scanEpoch = 0;
  gint64 scanBefore = -1;
  gint64 scanDeadline = 0;
  bool includeHidden = false;
  uint8_t scanChannel = 0;
  std::string scanSsidFilter;
  std::vector<Network> networks;
};

State &st() {
  static State s;
  return s;
}

void log_error(const char *op, GError *error) {
  if (error) {
    g_warning("WiFi.%s: %s", op, error->message);
    g_error_free(error);
  }
}

void pump_events() {
  // libnm's async callbacks need a running GMainContext. Arduino sketches
  // typically have no GLib loop, so poll it from each WiFi call.
  for (unsigned n = 0; n < 64 && g_main_context_pending(nullptr); ++n)
    g_main_context_iteration(nullptr, FALSE);
}

bool ensure_device() {
  State &s = st();
  if (!s.client) {
    GError *err = nullptr;
    s.client = nm_client_new(nullptr, &err);
    if (!s.client) {
      log_error("init", err);
      return false;
    }
  }
  pump_events();
  if (!nm_client_get_nm_running(s.client))
    return false;
  s.device = nullptr;
  const GPtrArray *devices = nm_client_get_devices(s.client);
  if (!devices)
    return false;
  for (guint i = 0; i < devices->len; ++i) {
    NMDevice *d = NM_DEVICE(g_ptr_array_index(devices, i));
    if (nm_device_get_device_type(d) == NM_DEVICE_TYPE_WIFI &&
        nm_device_get_managed(d)) {
      s.device = NM_DEVICE_WIFI(d);
      break;
    }
  }
  return s.device != nullptr;
}

NMDevice *dev() { return st().device ? NM_DEVICE(st().device) : nullptr; }

bool radio_available() {
  const State &s = st();
  return s.client && nm_client_wireless_get_enabled(s.client) &&
         nm_client_wireless_hardware_get_enabled(s.client);
}

// A libnm connection may be visible independently of our last request.
NMActiveConnection *device_active() {
  return dev() ? nm_device_get_active_connection(dev()) : nullptr;
}

bool active_is_ap(NMActiveConnection *active) {
  if (!active)
    return false;
  NMRemoteConnection *rc = nm_active_connection_get_connection(active);
  if (!rc)
    return false;
  NMSettingWireless *wireless =
      nm_connection_get_setting_wireless(NM_CONNECTION(rc));
  const char *m = wireless ? nm_setting_wireless_get_mode(wireless) : nullptr;
  return m && !strcmp(m, NM_SETTING_WIRELESS_MODE_AP);
}

bool active_connected(NMActiveConnection *active) {
  return active && nm_active_connection_get_state(active) ==
                       NM_ACTIVE_CONNECTION_STATE_ACTIVATED;
}

void clear_active(NMActiveConnection *&connection) {
  if (!connection)
    return;
  // The state-change handler rejects callbacks from obsolete connections.
  g_clear_object(&connection);
}

void deactivation_done(GObject *obj, GAsyncResult *result, gpointer) {
  GError *err = nullptr;
  nm_client_deactivate_connection_finish(NM_CLIENT(obj), result, &err);
  log_error("disconnect", err);
}

void deactivate(NMActiveConnection *active) {
  State &s = st();
  if (!active || !s.client ||
      nm_active_connection_get_state(active) ==
          NM_ACTIVE_CONNECTION_STATE_DEACTIVATED)
    return;
  nm_client_deactivate_connection_async(s.client, active, nullptr,
                                        deactivation_done, nullptr);
}

void delete_done(GObject *obj, GAsyncResult *res, gpointer) {
  GError *err = nullptr;
  nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(obj), res, &err);
  log_error("eraseCredentials", err);
}

void erase_our_station_profile() {
  State &s = st();
  if (!s.client || s.createdStationUuid.empty())
    return;
  NMRemoteConnection *rc =
      nm_client_get_connection_by_uuid(s.client, s.createdStationUuid.c_str());
  if (rc)
    nm_remote_connection_delete_async(rc, nullptr, delete_done, nullptr);
  s.createdStationUuid.clear();
}

bool valid_ssid(const char *ssid) {
  return ssid && *ssid && strlen(ssid) <= 32;
}

bool valid_psk(const char *password) {
  if (!password || !*password)
    return true; // Open network.
  const size_t n = strlen(password);
  if (n >= 8 && n <= 63)
    return true;
  if (n != 64)
    return false;
  for (const char *p = password; *p; ++p)
    if (!std::isxdigit(static_cast<unsigned char>(*p)))
      return false;
  return true;
}

std::string from_bytes(GBytes *bytes) {
  if (!bytes)
    return {};
  gsize size = 0;
  const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
  return std::string(data, size);
}

String as_arduino(const std::string &s) { return String(s.c_str()); }

IPAddress parse_ip(const char *s) {
  struct in_addr a{};
  if (!s || inet_pton(AF_INET, s, &a) != 1)
    return IPAddress();
  const auto *p = reinterpret_cast<const uint8_t *>(&a.s_addr);
  return IPAddress(p[0], p[1], p[2], p[3]);
}

std::string ip_text(const IPAddress &ip) {
  return std::string(ip.toString().c_str());
}

bool is_zero_ip(const IPAddress &ip) { return ip_text(ip) == "0.0.0.0"; }

// Validate that a subnet mask consists of contiguous ones and calculate CIDR.
int mask_prefix(const IPAddress &mask) {
  struct in_addr a{};
  const std::string value = ip_text(mask);
  if (inet_pton(AF_INET, value.c_str(), &a) != 1)
    return -1;
  uint32_t maskBits = ntohl(a.s_addr);
  unsigned prefix = 0;
  bool zeroFound = false;
  for (int bit = 31; bit >= 0; --bit) {
    if (maskBits & (uint32_t(1) << bit)) {
      if (zeroFound)
        return -1;
      ++prefix;
    } else {
      zeroFound = true;
    }
  }
  return prefix ? static_cast<int>(prefix) : -1;
}

IPAddress prefix_mask(int prefix) {
  if (prefix < 0 || prefix > 32)
    return IPAddress();
  const uint32_t mask = prefix ? (0xffffffffU << (32 - prefix)) : 0;
  const uint32_t bits = htonl(mask);
  const auto *p = reinterpret_cast<const uint8_t *>(&bits);
  return IPAddress(p[0], p[1], p[2], p[3]);
}

bool add_static_ipv4(NMSettingIPConfig *ip, const IPAddress &address,
                     const IPAddress &mask) {
  GError *err = nullptr;
  const std::string text = ip_text(address);
  NMIPAddress *nmAddress =
      nm_ip_address_new(AF_INET, text.c_str(), mask_prefix(mask), &err);
  if (!nmAddress) {
    log_error("ip", err);
    return false;
  }
  const bool added = nm_setting_ip_config_add_address(ip, nmAddress);
  nm_ip_address_unref(nmAddress);
  return added;
}

NMIPConfig *ip4() { return dev() ? nm_device_get_ip4_config(dev()) : nullptr; }

NMIPAddress *first_address(NMIPConfig *ip) {
  if (!ip)
    return nullptr;
  GPtrArray *addresses = nm_ip_config_get_addresses(ip);
  return addresses && addresses->len
             ? static_cast<NMIPAddress *>(g_ptr_array_index(addresses, 0))
             : nullptr;
}

bool add_station_ip_settings(NMConnection *profile) {
  State &s = st();
  NMSetting *ipSetting = nm_setting_ip4_config_new();
  auto *ip = NM_SETTING_IP_CONFIG(ipSetting);
  if (s.stationStatic) {
    g_object_set(ipSetting, NM_SETTING_IP_CONFIG_METHOD,
                 NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
                 NM_SETTING_IP_CONFIG_GATEWAY,
                 ip_text(s.stationGateway).c_str(), NULL);
    if (!add_static_ipv4(ip, s.stationIp, s.stationSubnet)) {
      g_object_unref(ipSetting);
      return false;
    }
    if (!is_zero_ip(s.stationDns1))
      nm_setting_ip_config_add_dns(ip, ip_text(s.stationDns1).c_str());
    if (!is_zero_ip(s.stationDns2))
      nm_setting_ip_config_add_dns(ip, ip_text(s.stationDns2).c_str());
  } else {
    g_object_set(ipSetting, NM_SETTING_IP_CONFIG_METHOD,
                 NM_SETTING_IP4_CONFIG_METHOD_AUTO, NULL);
  }
  nm_connection_add_setting(profile, ipSetting);
  return true;
}

bool add_ap_ip_settings(NMConnection *profile) {
  State &s = st();
  NMSetting *ipSetting = nm_setting_ip4_config_new();
  auto *ip = NM_SETTING_IP_CONFIG(ipSetting);
  g_object_set(ipSetting, NM_SETTING_IP_CONFIG_METHOD,
               NM_SETTING_IP4_CONFIG_METHOD_SHARED, NULL);
  if (!add_static_ipv4(ip, s.hotspotIp, s.hotspotSubnet)) {
    g_object_unref(ipSetting);
    return false;
  }
  nm_connection_add_setting(profile, ipSetting);
  return true;
}

NMConnection *make_profile(const std::string &ssid, const std::string &password,
                           bool isAp, int32_t channel, const std::string &bssid,
                           bool hidden, std::string *outUuid) {
  State &s = st();
  NMConnection *profile = nm_simple_connection_new();
  NMSetting *general = nm_setting_connection_new();
  gchar *uuid = nm_utils_uuid_generate();
  if (outUuid)
    *outUuid = uuid;
  g_object_set(general, NM_SETTING_CONNECTION_ID,
               (std::string("ESP8266WiFi:") + ssid).c_str(),
               NM_SETTING_CONNECTION_UUID, uuid, NM_SETTING_CONNECTION_TYPE,
               NM_SETTING_WIRELESS_SETTING_NAME,
               NM_SETTING_CONNECTION_INTERFACE_NAME, nm_device_get_iface(dev()),
               NM_SETTING_CONNECTION_AUTOCONNECT,
               (!isAp && s.autoReconnect) ? TRUE : FALSE, NULL);
  g_free(uuid);
  nm_connection_add_setting(profile, general);

  NMSetting *wireless = nm_setting_wireless_new();
  GBytes *ssidBytes = g_bytes_new(ssid.data(), ssid.size());
  g_object_set(
      wireless, NM_SETTING_WIRELESS_SSID, ssidBytes, NM_SETTING_WIRELESS_MODE,
      isAp ? NM_SETTING_WIRELESS_MODE_AP : NM_SETTING_WIRELESS_MODE_INFRA,
      NM_SETTING_WIRELESS_HIDDEN, hidden ? TRUE : FALSE, NULL);
  g_bytes_unref(ssidBytes);
  if (channel) {
    // ESP8266-style 2.4GHz channels (1-14).
    g_object_set(wireless, NM_SETTING_WIRELESS_BAND, "bg",
                 NM_SETTING_WIRELESS_CHANNEL, static_cast<guint>(channel),
                 NULL);
  }
  if (!bssid.empty())
    g_object_set(wireless, NM_SETTING_WIRELESS_BSSID, bssid.c_str(), NULL);
  nm_connection_add_setting(profile, wireless);

  if (!password.empty()) {
    NMSetting *security = nm_setting_wireless_security_new();
    g_object_set(security, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
                 NM_SETTING_WIRELESS_SECURITY_PSK, password.c_str(), NULL);
    nm_setting_wireless_security_add_proto(
        NM_SETTING_WIRELESS_SECURITY(security), "rsn");
    nm_connection_add_setting(profile, security);
  }

  if (!(isAp ? add_ap_ip_settings(profile)
             : add_station_ip_settings(profile))) {
    g_object_unref(profile);
    return nullptr;
  }
  NMSetting *ipv6 = nm_setting_ip6_config_new();
  g_object_set(ipv6, NM_SETTING_IP_CONFIG_METHOD,
               NM_SETTING_IP6_CONFIG_METHOD_DISABLED, NULL);
  nm_connection_add_setting(profile, ipv6);

  GError *error = nullptr;
  if (!nm_connection_verify(profile, &error)) {
    log_error("profile", error);
    g_object_unref(profile);
    return nullptr;
  }
  return profile;
}

// Two async activation paths: an existing saved NM profile, or a new profile.
struct Request {
  bool ap = false;
  bool newProfile = false;
  guint64 epoch = 0;
};

wl_status_t classify_failure() {
  NMDevice *d = dev();
  if (!d)
    return WL_NO_SHIELD;
  const NMDeviceStateReason reason = nm_device_get_state_reason(d);
  if (reason == NM_DEVICE_STATE_REASON_NO_SECRETS)
    return WL_WRONG_PASSWORD; // Usually a missing/invalid secret; not
                              // definitive.
  if (reason == NM_DEVICE_STATE_REASON_SSID_NOT_FOUND)
    return WL_NO_SSID_AVAIL;
  return WL_CONNECT_FAILED;
}

void active_state_changed(NMActiveConnection *active, guint state, guint reason,
                          gpointer userData) {
  (void)reason;
  State &s = st();
  const bool isAp = GPOINTER_TO_INT(userData) != 0;
  if (active != (isAp ? s.ap : s.sta))
    return;
  if (isAp) {
    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED ||
        state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED)
      s.apPending = false;
  } else {
    switch (state) {
    case NM_ACTIVE_CONNECTION_STATE_ACTIVATED:
      s.staStatus = WL_CONNECTED;
      s.staPending = false;
      break;
    case NM_ACTIVE_CONNECTION_STATE_DEACTIVATED:
      s.staStatus = s.staPending ? classify_failure() : WL_CONNECTION_LOST;
      s.staPending = false;
      break;
    default:
      s.staStatus = WL_IDLE_STATUS;
      break;
    }
  }
}

void activation_done(GObject *obj, GAsyncResult *result, gpointer userData) {
  const std::unique_ptr<Request> req(static_cast<Request *>(userData));
  State &s = st();
  GError *error = nullptr;
  NMActiveConnection *active =
      req->newProfile ? nm_client_add_and_activate_connection2_finish(
                            NM_CLIENT(obj), result, nullptr, &error)
                      : nm_client_activate_connection_finish(NM_CLIENT(obj),
                                                             result, &error);

  const bool current =
      req->ap ? req->epoch == s.apEpoch : req->epoch == s.stationEpoch;
  if (!current) {
    if (active) {
      deactivate(active);
      g_object_unref(active);
    }
    if (error)
      g_error_free(error);
    return;
  }
  if (!active) {
    log_error(req->ap ? "softAP" : "begin", error);
    if (req->ap)
      s.apPending = false;
    else {
      s.staPending = false;
      s.staStatus = classify_failure();
    }
    return;
  }

  NMActiveConnection *&slot = req->ap ? s.ap : s.sta;
  clear_active(slot);
  slot = active; // The finish function gave us this reference.
  g_signal_connect(active, "state-changed", G_CALLBACK(active_state_changed),
                   GINT_TO_POINTER(req->ap));
  active_state_changed(active, nm_active_connection_get_state(active),
                       nm_active_connection_get_state_reason(active),
                       GINT_TO_POINTER(req->ap));
}

bool activate_profile(NMConnection *profile, bool ap,
                      const std::string &uuid = {}) {
  State &s = st();
  if (!profile || !dev())
    return false;
  auto *req = new Request;
  req->ap = ap;
  req->epoch = ap ? ++s.apEpoch : ++s.stationEpoch;
  req->newProfile = !NM_IS_REMOTE_CONNECTION(profile);
  if (ap)
    s.apPending = true;
  else {
    s.staPending = true;
    s.staStatus = WL_IDLE_STATUS;
    if (req->newProfile)
      s.createdStationUuid = uuid;
  }
  if (req->newProfile) {
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(
        &opts, "{sv}", "persist",
        g_variant_new_string(s.saveToDisk ? "disk" : "volatile"));
    nm_client_add_and_activate_connection2(s.client, profile, dev(), nullptr,
                                           g_variant_builder_end(&opts),
                                           nullptr, activation_done, req);
  } else {
    nm_client_activate_connection_async(s.client, profile, dev(), nullptr,
                                        nullptr, activation_done, req);
  }
  return true;
}

NMRemoteConnection *find_saved_station(const std::string &ssid) {
  State &s = st();
  if (!s.client)
    return nullptr;
  const GPtrArray *profiles = nm_client_get_connections(s.client);
  if (!profiles)
    return nullptr;
  // Prefer profiles created by this adapter, then other stored profiles.
  NMRemoteConnection *fallback = nullptr;
  for (guint i = 0; i < profiles->len; ++i) {
    auto *rc = NM_REMOTE_CONNECTION(g_ptr_array_index(profiles, i));
    auto *general = nm_connection_get_setting_connection(NM_CONNECTION(rc));
    auto *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(rc));
    if (!general || !wireless)
      continue;
    const char *mode = nm_setting_wireless_get_mode(wireless);
    const char *iface = nm_setting_connection_get_interface_name(general);
    if (mode && strcmp(mode, NM_SETTING_WIRELESS_MODE_INFRA))
      continue;
    if (iface && strcmp(iface, nm_device_get_iface(dev())))
      continue;
    if (from_bytes(nm_setting_wireless_get_ssid(wireless)) != ssid)
      continue;
    const char *id = nm_setting_connection_get_id(general);
    if (id && g_str_has_prefix(id, "ESP8266WiFi:"))
      return rc;
    if (!fallback)
      fallback = rc;
  }
  return fallback;
}

void scan_requested(GObject *obj, GAsyncResult *result, gpointer userData) {
  const guint64 epoch = *static_cast<guint64 *>(userData);
  delete static_cast<guint64 *>(userData);
  State &s = st();
  GError *error = nullptr;
  const bool accepted =
      nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(obj), result, &error);
  if (epoch != s.scanEpoch) {
    if (error)
      g_error_free(error);
    return;
  }
  if (!accepted) {
    log_error("scanNetworks", error);
    s.scanRunning = false;
    s.scanResult = WIFI_SCAN_FAILED;
    return;
  }
  s.scanRequestAccepted = true;
}

int dbm_estimate(guint8 strength) {
  // NMAccessPoint exposes signal strength 0-100%, not measured dBm.
  return -100 + (static_cast<int>(strength) * 70) / 100;
}

uint8_t encryption(NMAccessPoint *ap) {
  const auto flags = nm_access_point_get_flags(ap);
  const guint32 sec = static_cast<guint32>(nm_access_point_get_wpa_flags(ap) |
                                           nm_access_point_get_rsn_flags(ap));
  if (sec == NM_802_11_AP_SEC_NONE)
    return (flags & NM_802_11_AP_FLAGS_PRIVACY) ? kEncWep : kEncNone;
  if (sec & (NM_802_11_AP_SEC_PAIR_CCMP | NM_802_11_AP_SEC_GROUP_CCMP))
    return kEncCcmp;
  if (sec & (NM_802_11_AP_SEC_PAIR_TKIP | NM_802_11_AP_SEC_GROUP_TKIP))
    return kEncTkip;
  if (sec & (NM_802_11_AP_SEC_PAIR_WEP40 | NM_802_11_AP_SEC_PAIR_WEP104))
    return kEncWep;
  return kEncAuto; // e.g. WPA3 SAE/OWE: historic ESP enum has no entry.
}

void finish_scan() {
  State &s = st();
  s.networks.clear();
  const GPtrArray *aps = nm_device_wifi_get_access_points(s.device);
  if (aps) {
    for (guint i = 0; i < aps->len && s.networks.size() < 127; ++i) {
      NMAccessPoint *ap = NM_ACCESS_POINT(g_ptr_array_index(aps, i));
      Network net;
      net.ssid = from_bytes(nm_access_point_get_ssid(ap));
      net.channel =
          nm_utils_wifi_freq_to_channel(nm_access_point_get_frequency(ap));
      if ((!s.includeHidden && net.ssid.empty()) ||
          (s.scanChannel && net.channel != s.scanChannel) ||
          (!s.scanSsidFilter.empty() && net.ssid != s.scanSsidFilter))
        continue;
      net.rssi = dbm_estimate(nm_access_point_get_strength(ap));
      net.encryption = encryption(ap);
      s.networks.emplace_back(std::move(net));
    }
  }
  std::sort(s.networks.begin(), s.networks.end(),
            [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
  s.scanRunning = false;
  s.scanResult = static_cast<int>(s.networks.size());
}

void update_scan() {
  State &s = st();
  if (!s.scanRunning)
    return;
  if (s.scanRequestAccepted && s.device &&
      nm_device_wifi_get_last_scan(s.device) > s.scanBefore) {
    finish_scan();
  } else if (g_get_monotonic_time() >= s.scanDeadline) {
    s.scanRunning = false;
    s.scanResult = WIFI_SCAN_FAILED;
  }
}

void service() {
  pump_events();
  update_scan();
}

// Used to represent the gateway/subnet pair supported by NM's IPv4 sharing.
bool valid_address_tuple(IPAddress ip, IPAddress subnet) {
  return !is_zero_ip(ip) && mask_prefix(subnet) >= 1;
}

} // namespace

ESP8266WiFiClass WiFi;

bool ESP8266WiFiClass::mode(WiFiMode_t newMode) {
  if (loopback_wifi()) {
    if (newMode != WIFI_OFF && newMode != WIFI_STA)
      return false;
    _mode = newMode;
    if (newMode == WIFI_OFF)
      st().staStatus = WL_DISCONNECTED;
    return true;
  }
  service();
  if (newMode == WIFI_AP_STA)
    return false; // A single NetworkManager device isn't a dual-mode radio.
  if (newMode != WIFI_OFF && newMode != WIFI_STA && newMode != WIFI_AP)
    return false;
  if (newMode != WIFI_OFF && (!ensure_device() || !radio_available()))
    return false;
  if (newMode == WIFI_AP &&
      !(nm_device_wifi_get_capabilities(st().device) & NM_WIFI_DEVICE_CAP_AP))
    return false;
  if (newMode == WIFI_OFF || newMode == WIFI_AP) {
    ++st().stationEpoch;
    st().staPending = false;
    st().staStatus = WL_DISCONNECTED;
    deactivate(st().sta ? st().sta
                        : (device_active() && !active_is_ap(device_active())
                               ? device_active()
                               : nullptr));
    clear_active(st().sta);
  }
  if (newMode == WIFI_OFF || newMode == WIFI_STA) {
    ++st().apEpoch;
    st().apPending = false;
    deactivate(st().ap ? st().ap
                       : (device_active() && active_is_ap(device_active())
                              ? device_active()
                              : nullptr));
    clear_active(st().ap);
  }
  _mode = newMode;
  return true;
}

WiFiMode_t ESP8266WiFiClass::getMode() { return _mode; }
void ESP8266WiFiClass::persistent(bool enabled) { st().saveToDisk = enabled; }

bool ESP8266WiFiClass::enableSTA(bool enabled) {
  if (enabled)
    return mode(WIFI_STA);
  return _mode != WIFI_STA || mode(WIFI_OFF);
}

bool ESP8266WiFiClass::enableAP(bool enabled) {
  if (enabled)
    return mode(WIFI_AP);
  return _mode != WIFI_AP || mode(WIFI_OFF);
}

uint8_t ESP8266WiFiClass::channel() {
  service();
  if (_mode == WIFI_AP)
    return static_cast<uint8_t>(st().apChannel);
  if (!ensure_device())
    return 0;
  NMAccessPoint *ap = nm_device_wifi_get_active_access_point(st().device);
  return ap ? static_cast<uint8_t>(nm_utils_wifi_freq_to_channel(
                  nm_access_point_get_frequency(ap)))
            : 0;
}

wl_status_t ESP8266WiFiClass::begin(const char *ssid, const char *passphrase,
                                    int32_t channel, const uint8_t *bssid,
                                    bool connect) {
  const std::string newSsid = ssid ? ssid : "";
  const std::string newPass = passphrase ? passphrase : "";
  if (!valid_ssid(ssid) || !valid_psk(passphrase) || channel < 0 ||
      channel > 14)
    return WL_CONNECT_FAILED;
  if (loopback_wifi()) {
    if (_mode != WIFI_STA && !mode(WIFI_STA))
      return WL_NO_SHIELD;
    State &s = st();
    s.ssid = "host-loopback";
    s.passphrase.clear();
    s.staStatus = connect ? WL_CONNECTED : WL_DISCONNECTED;
    return s.staStatus;
  }
  if (_mode != WIFI_STA && !mode(WIFI_STA))
    return WL_NO_SHIELD;
  if (!ensure_device() || !radio_available())
    return WL_NO_SHIELD;

  State &s = st();
  s.ssid = newSsid;
  s.passphrase = newPass;
  s.requestedChannel = channel;
  s.requestedBssid.clear();
  if (bssid) {
    char mac[18];
    std::snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", bssid[0],
                  bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    s.requestedBssid = mac;
  }
  if (!connect) {
    s.staStatus = WL_DISCONNECTED;
    return s.staStatus;
  }
  ++s.stationEpoch;
  deactivate(s.sta);
  clear_active(s.sta);

  // If a matching stored connection has the same visible passphrase and no
  // per-call constraints, reuse it. Otherwise create an isolated profile.
  NMRemoteConnection *stored = find_saved_station(s.ssid);
  if (stored && !channel && !bssid && !s.stationStatic) {
    NMSettingWirelessSecurity *sec =
        nm_connection_get_setting_wireless_security(NM_CONNECTION(stored));
    const char *storedPass =
        sec ? nm_setting_wireless_security_get_psk(sec) : nullptr;
    if ((newPass.empty() && !sec) || (storedPass && newPass == storedPass)) {
      activate_profile(NM_CONNECTION(stored), false);
      return WL_IDLE_STATUS;
    }
  }

  std::string uuid;
  NMConnection *profile =
      make_profile(s.ssid, s.passphrase, false, s.requestedChannel,
                   s.requestedBssid, false, &uuid);
  if (!profile) {
    s.staStatus = WL_CONNECT_FAILED;
    return s.staStatus;
  }
  const bool started = activate_profile(profile, false, uuid);
  g_object_unref(profile);
  return started ? WL_IDLE_STATUS : WL_CONNECT_FAILED;
}

wl_status_t ESP8266WiFiClass::begin() {
  State &s = st();
  if (!s.ssid.empty()) {
    // With no passphrase in the mock, a known saved NetworkManager profile
    // may still carry the secret in its keyring. Activate it directly.
    if (s.passphrase.empty() && !s.stationStatic && s.requestedChannel == 0 &&
        s.requestedBssid.empty()) {
      if (_mode != WIFI_STA && !mode(WIFI_STA))
        return WL_NO_SHIELD;
      if (!ensure_device())
        return WL_NO_SHIELD;
      if (auto *saved = find_saved_station(s.ssid)) {
        ++s.stationEpoch;
        deactivate(s.sta);
        clear_active(s.sta);
        activate_profile(NM_CONNECTION(saved), false);
        return WL_IDLE_STATUS;
      }
    }
    const std::string ssid = s.ssid;
    const std::string pass = s.passphrase;
    const int32_t chan = s.requestedChannel;
    uint8_t bssid[6];
    const uint8_t *bssidPtr = nullptr;
    unsigned v[6];
    if (std::sscanf(s.requestedBssid.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
                    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
      for (int i = 0; i < 6; ++i)
        bssid[i] = static_cast<uint8_t>(v[i]);
      bssidPtr = bssid;
    }
    return begin(ssid.c_str(), pass.empty() ? nullptr : pass.c_str(), chan,
                 bssidPtr, true);
  }
  if (_mode != WIFI_STA && !mode(WIFI_STA))
    return WL_NO_SHIELD;
  if (!ensure_device())
    return WL_NO_SHIELD;
  // ESP begin() without credentials: try any saved infrastructure profile.
  const GPtrArray *profiles = nm_client_get_connections(s.client);
  if (profiles) {
    for (guint i = 0; i < profiles->len; ++i) {
      auto *rc = NM_REMOTE_CONNECTION(g_ptr_array_index(profiles, i));
      auto *general = nm_connection_get_setting_connection(NM_CONNECTION(rc));
      auto *wireless = nm_connection_get_setting_wireless(NM_CONNECTION(rc));
      if (!general || !wireless)
        continue;
      const char *mode = nm_setting_wireless_get_mode(wireless);
      const char *iface = nm_setting_connection_get_interface_name(general);
      if ((!mode || !strcmp(mode, NM_SETTING_WIRELESS_MODE_INFRA)) &&
          (!iface || !strcmp(iface, nm_device_get_iface(dev())))) {
        s.ssid = from_bytes(nm_setting_wireless_get_ssid(wireless));
        if (s.ssid.empty())
          continue;
        activate_profile(NM_CONNECTION(rc), false);
        return WL_IDLE_STATUS;
      }
    }
  }
  return WL_NO_SSID_AVAIL;
}

bool ESP8266WiFiClass::disconnect(bool wifioff) {
  return disconnect(wifioff, false);
}

bool ESP8266WiFiClass::disconnect(bool wifioff, bool eraseCredentials) {
  if (loopback_wifi()) {
    st().staStatus = WL_DISCONNECTED;
    if (eraseCredentials)
      st().ssid.clear();
    if (wifioff)
      _mode = WIFI_OFF;
    return true;
  }
  service();
  State &s = st();
  ++s.stationEpoch;
  s.staPending = false;
  s.staStatus = WL_DISCONNECTED;
  NMActiveConnection *ac = s.sta;
  if (!ac && device_active() && !active_is_ap(device_active()))
    ac = device_active();
  deactivate(ac);
  clear_active(s.sta);
  if (eraseCredentials) {
    erase_our_station_profile();
    s.ssid.clear();
    std::fill(s.passphrase.begin(), s.passphrase.end(), '\0');
    s.passphrase.clear();
    s.requestedBssid.clear();
    s.requestedChannel = 0;
  }
  if (wifioff)
    _mode = WIFI_OFF; // Does not rfkill the host's other Wi-Fi connections.
  return true; // Deactivation has been queued; not necessarily completed.
}

bool ESP8266WiFiClass::reconnect() { return begin() == WL_IDLE_STATUS; }

bool ESP8266WiFiClass::isConnected() { return status() == WL_CONNECTED; }

wl_status_t ESP8266WiFiClass::status() {
  if (loopback_wifi())
    return _mode == WIFI_STA ? st().staStatus : WL_DISCONNECTED;
  service();
  State &s = st();
  if (!ensure_device())
    return WL_NO_SHIELD;
  if (_mode != WIFI_STA || !radio_available())
    return WL_DISCONNECTED;
  if (s.staPending)
    return s.staStatus;
  if (s.sta && active_connected(s.sta))
    return WL_CONNECTED;
  NMActiveConnection *ac = device_active();
  if (ac && !active_is_ap(ac) && active_connected(ac))
    return WL_CONNECTED;
  if (s.sta && nm_active_connection_get_state(s.sta) ==
                   NM_ACTIVE_CONNECTION_STATE_ACTIVATING)
    return WL_IDLE_STATUS;
  return s.staStatus;
}

String ESP8266WiFiClass::SSID() const {
  if (loopback_wifi())
    return as_arduino(st().ssid);
  service();
  const State &s = st();
  if (ensure_device() && s.device &&
      nm_device_wifi_get_mode(s.device) == NM_802_11_MODE_INFRA) {
    NMAccessPoint *ap = nm_device_wifi_get_active_access_point(s.device);
    if (ap)
      return as_arduino(from_bytes(nm_access_point_get_ssid(ap)));
  }
  return as_arduino(s.ssid);
}

String ESP8266WiFiClass::psk() const { return as_arduino(st().passphrase); }

int8_t ESP8266WiFiClass::RSSI() {
  if (loopback_wifi())
    return st().staStatus == WL_CONNECTED ? -50 : 0;
  service();
  if (!ensure_device())
    return 0;
  NMAccessPoint *ap = nm_device_wifi_get_active_access_point(st().device);
  return ap ? static_cast<int8_t>(
                  dbm_estimate(nm_access_point_get_strength(ap)))
            : 0;
}

IPAddress ESP8266WiFiClass::localIP() {
  if (loopback_wifi())
    return status() == WL_CONNECTED ? IPAddress(127, 0, 0, 1) : IPAddress();
  service();
  if (status() != WL_CONNECTED)
    return IPAddress();
  NMIPAddress *a = first_address(ip4());
  return a ? parse_ip(nm_ip_address_get_address(a)) : IPAddress();
}

IPAddress ESP8266WiFiClass::subnetMask() {
  service();
  if (status() != WL_CONNECTED)
    return IPAddress();
  NMIPAddress *a = first_address(ip4());
  return a ? prefix_mask(nm_ip_address_get_prefix(a)) : IPAddress();
}

IPAddress ESP8266WiFiClass::gatewayIP() {
  service();
  if (status() != WL_CONNECTED)
    return IPAddress();
  NMIPConfig *config = ip4();
  return config ? parse_ip(nm_ip_config_get_gateway(config)) : IPAddress();
}

IPAddress ESP8266WiFiClass::dnsIP(uint8_t dns_no) {
  service();
  if (status() != WL_CONNECTED)
    return IPAddress();
  NMIPConfig *config = ip4();
  const char *const *dns =
      config ? nm_ip_config_get_nameservers(config) : nullptr;
  if (!dns)
    return IPAddress();
  for (unsigned i = 0; dns[i]; ++i)
    if (i == dns_no)
      return parse_ip(dns[i]);
  return IPAddress();
}

String ESP8266WiFiClass::macAddress() {
  if (loopback_wifi())
    return String("02:00:00:00:00:01");
  service();
  if (!ensure_device())
    return String("");
  const char *mac = nm_device_get_hw_address(dev());
  return String(mac ? mac : "");
}

bool ESP8266WiFiClass::config(IPAddress local_ip, IPAddress gateway,
                              IPAddress subnet, IPAddress dns1,
                              IPAddress dns2) {
  State &s = st();
  if (is_zero_ip(local_ip)) {
    s.stationStatic = false; // DHCP on the next begin()/reconnect().
    return true;
  }
  if (!valid_address_tuple(local_ip, subnet) || is_zero_ip(gateway))
    return false;
  s.stationStatic = true;
  s.stationIp = local_ip;
  s.stationGateway = gateway;
  s.stationSubnet = subnet;
  s.stationDns1 = dns1;
  s.stationDns2 = dns2;
  return true; // Applied when the next station profile is activated.
}

bool ESP8266WiFiClass::setAutoReconnect(bool enabled) {
  st().autoReconnect = enabled;
  return true; // Applies to newly created profiles, not older saved profiles.
}

bool ESP8266WiFiClass::getAutoReconnect() { return st().autoReconnect; }

bool ESP8266WiFiClass::softAP(const char *ssid, const char *psk, int channel,
                              int ssid_hidden, int max_connection,
                              int beacon_interval) {
  if (!valid_ssid(ssid) || !valid_psk(psk) || channel < 1 || channel > 14 ||
      max_connection != 4 || beacon_interval != 100)
    return false; // libnm does not expose max-clients or beacon-interval.
  if (_mode != WIFI_AP && !mode(WIFI_AP))
    return false;
  if (!ensure_device() || !radio_available() ||
      !(nm_device_wifi_get_capabilities(st().device) & NM_WIFI_DEVICE_CAP_AP))
    return false;
  State &s = st();
  ++s.apEpoch;
  deactivate(s.ap);
  clear_active(s.ap);
  s.apSsid = ssid;
  s.apChannel = channel;

  NMConnection *profile = make_profile(s.apSsid, psk ? psk : "", true, channel,
                                       "", ssid_hidden != 0, nullptr);
  if (!profile)
    return false;
  bool queued = activate_profile(profile, true);
  g_object_unref(profile);
  if (!queued)
    return false;
  // softAP() returns a bool, so wait until AP activation truly succeeds.
  const gint64 deadline = g_get_monotonic_time() + kApTimeoutUs;
  while (s.apPending && g_get_monotonic_time() < deadline) {
    service();
    g_usleep(20000);
  }
  if (s.ap && active_connected(s.ap))
    return true;
  // A timeout must not leave a hotspot activating in the background.
  ++s.apEpoch;
  s.apPending = false;
  deactivate(s.ap);
  clear_active(s.ap);
  return false;
}

bool ESP8266WiFiClass::softAPdisconnect(bool wifioff) {
  service();
  State &s = st();
  ++s.apEpoch;
  s.apPending = false;
  NMActiveConnection *ac = s.ap;
  if (!ac && device_active() && active_is_ap(device_active()))
    ac = device_active();
  deactivate(ac);
  clear_active(s.ap);
  if (wifioff)
    _mode = WIFI_OFF;
  return true;
}

bool ESP8266WiFiClass::softAPConfig(IPAddress local_ip, IPAddress gateway,
                                    IPAddress subnet) {
  if (!valid_address_tuple(local_ip, subnet) ||
      ip_text(local_ip) != ip_text(gateway))
    return false; // NetworkManager sharing uses the AP IP as gateway.
  st().hotspotIp = local_ip;
  st().hotspotSubnet = subnet;
  return true; // Applied to the next softAP() call.
}

IPAddress ESP8266WiFiClass::softAPIP() {
  service();
  if (_mode != WIFI_AP)
    return IPAddress();
  if (st().ap && active_connected(st().ap)) {
    NMIPAddress *a = first_address(ip4());
    if (a)
      return parse_ip(nm_ip_address_get_address(a));
  }
  return IPAddress();
}

uint8_t ESP8266WiFiClass::softAPgetStationNum() {
  service();
  if (!ensure_device() || _mode != WIFI_AP || !st().ap ||
      !active_connected(st().ap))
    return 0;
  // libnm doesn't expose associated-client count. If `iw` is installed,
  // use nl80211 via its CLI, otherwise the only safe fallback is zero.
  const char *iface = nm_device_get_iface(dev());
  if (!iface || !*iface)
    return 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(iface);
       *p; ++p) {
    if (!std::isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
      return 0;
  }
  const std::string cmd =
      std::string("iw dev ") + iface + " station dump 2>/dev/null";
  FILE *f = popen(cmd.c_str(), "r");
  if (!f)
    return 0;
  uint8_t count = 0;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (!strncmp(line, "Station ", 8) && count < 255)
      ++count;
  }
  pclose(f);
  return count;
}

String ESP8266WiFiClass::softAPSSID() const { return as_arduino(st().apSsid); }

int8_t ESP8266WiFiClass::scanNetworks(bool async, bool show_hidden,
                                      uint8_t channel, uint8_t *ssid) {
  if (loopback_wifi())
    return 0;
  service();
  if (st().scanRunning)
    return WIFI_SCAN_RUNNING;
  if (channel > 14 || (_mode == WIFI_AP))
    return WIFI_SCAN_FAILED;
  if (_mode == WIFI_OFF && !mode(WIFI_STA))
    return WIFI_SCAN_FAILED;
  if (!ensure_device() || !radio_available())
    return WIFI_SCAN_FAILED;

  State &s = st();
  ++s.scanEpoch;
  s.networks.clear();
  s.scanResult = WIFI_SCAN_RUNNING;
  s.scanRunning = true;
  s.scanRequestAccepted = false;
  s.scanBefore = nm_device_wifi_get_last_scan(s.device);
  s.scanDeadline = g_get_monotonic_time() + kScanTimeoutUs;
  s.scanChannel = channel;
  s.includeHidden = show_hidden;
  s.scanSsidFilter = ssid ? reinterpret_cast<const char *>(ssid) : "";

  if (ssid && *ssid) {
    // Request an active, directed scan for the specified SSID.
    GVariantBuilder ssids;
    g_variant_builder_init(&ssids, G_VARIANT_TYPE("aay"));
    const char *filter = reinterpret_cast<const char *>(ssid);
    g_variant_builder_add_value(
        &ssids, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, filter,
                                          strlen(filter), sizeof(guint8)));
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&opts, "{sv}", "ssids",
                          g_variant_builder_end(&ssids));
    GVariant *variant = g_variant_ref_sink(g_variant_builder_end(&opts));
    nm_device_wifi_request_scan_options_async(
        s.device, variant, nullptr, scan_requested, new guint64(s.scanEpoch));
    g_variant_unref(variant);
  } else {
    nm_device_wifi_request_scan_async(s.device, nullptr, scan_requested,
                                      new guint64(s.scanEpoch));
  }
  if (async)
    return WIFI_SCAN_RUNNING;
  while (s.scanRunning && g_get_monotonic_time() < s.scanDeadline) {
    service();
    g_usleep(20000);
  }
  update_scan();
  return static_cast<int8_t>(s.scanResult);
}

int8_t ESP8266WiFiClass::scanComplete() {
  service();
  return static_cast<int8_t>(st().scanRunning ? WIFI_SCAN_RUNNING
                                              : st().scanResult);
}

void ESP8266WiFiClass::scanDelete() {
  State &s = st();
  ++s.scanEpoch;
  s.scanRunning = false;
  s.scanRequestAccepted = false;
  s.scanResult = 0;
  s.networks.clear();
}

String ESP8266WiFiClass::SSID(uint8_t networkItem) {
  service();
  const auto &networks = st().networks;
  return networkItem < networks.size() ? as_arduino(networks[networkItem].ssid)
                                       : String("");
}

int32_t ESP8266WiFiClass::RSSI(uint8_t networkItem) {
  service();
  const auto &networks = st().networks;
  return networkItem < networks.size() ? networks[networkItem].rssi : 0;
}

int32_t ESP8266WiFiClass::channel(uint8_t networkItem) {
  service();
  const auto &networks = st().networks;
  return networkItem < networks.size() ? networks[networkItem].channel : 0;
}

uint8_t ESP8266WiFiClass::encryptionType(uint8_t networkItem) {
  service();
  const auto &networks = st().networks;
  return networkItem < networks.size() ? networks[networkItem].encryption
                                       : kEncAuto;
}
