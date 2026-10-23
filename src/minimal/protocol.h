#pragma once
// Shared, host-testable validation. No hardware or credentials are needed.
#include <ArduinoJson.h>
#include <stdint.h>
#include <string.h>

namespace minimal {
constexpr uint32_t MaxFirmware = 1048576;
constexpr size_t MaxConfig = 8192;
constexpr size_t MaxWire = 2048;
constexpr const char* Target = "esp8266:nodemcuv2:4m1m";

inline bool text(JsonVariantConst v, size_t low, size_t high) {
  if (!v.is<const char*>()) return false;
  JsonString s = v.as<JsonString>();
  return s.size() >= low && s.size() <= high && strlen(s.c_str()) == s.size();
}
inline bool printable(const char* s) {
  for (; *s; ++s) if (*s < 33 || *s > 126) return false;
  return true;
}
inline bool identifier(const char* s) {
  if (!*s || strlen(s) > 64) return false;
  for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
      (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-')) return false;
  return true;
}
inline bool httpsBase(const char* s) {
  size_t n = strlen(s);
  if (n < 9 || n > 200 || strncmp(s, "https://", 8) || s[n-1] == '/' || !printable(s) ||
      strpbrk(s, "@?#\\")) return false;
  const char* host = s + 8;
  if (*host == '/' || *host == ':') return false;
  const char* p = host;
  for (; *p && *p != '/' && *p != ':'; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
  if (*p == ':') {
    uint32_t port = 0; const char* first = ++p;
    for (; *p && *p != '/'; ++p) {
      if (*p < '0' || *p > '9' || port > 6553) return false;
      port = port * 10 + (*p - '0');
    }
    if (first == p || !port || port > 65535) return false;
  }
  return true;
}
inline const char* configError(JsonObjectConst d) {
  if (!text(d["wifi_ssid"], 1, 32) || !text(d["wifi_password"], 0, 64)) return "invalid_wifi";
  size_t passwordLength = d["wifi_password"].as<JsonString>().size();
  if (passwordLength && passwordLength < 8) return "invalid_wifi_password";
  if (!text(d["server_url"], 9, 200) || !httpsBase(d["server_url"])) return "https_required";
  if (!text(d["device_token"], 24, 192) || !printable(d["device_token"])) return "invalid_token";
  if (!text(d["ca_pem"], 64, 4096)) return "invalid_ca";
  if (!d["enabled"].is<bool>() || !d["allow_lan_http"].is<bool>() || d["allow_lan_http"].as<bool>()) return "invalid_policy";
  if (!d["poll_ms"].is<uint32_t>() || d["poll_ms"].as<uint32_t>() < 1000 ||
      d["poll_ms"].as<uint32_t>() > 60000) return "invalid_poll_interval";
  if (d.containsKey("firmware_public_key") && !text(d["firmware_public_key"], 0, 1024)) return "invalid_update_key";
  return nullptr;
}
inline bool updatePath(const char* s) {
  if (strncmp(s, "/v1/firmware/", 13) || strlen(s) <= 13 || strlen(s) > 160 ||
      strstr(s, "..") || strstr(s, "//")) return false;
  for (; *s; ++s) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
      (*s >= '0' && *s <= '9') || *s == '.' || *s == '_' || *s == '-' || *s == '/')) return false;
  return true;
}
inline bool sha256(const char* s) {
  if (strlen(s) != 64) return false;
  for (; *s; ++s) if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) return false;
  return true;
}
inline const char* updateError(JsonObjectConst a) {
  if (!text(a["path"], 14, 160) || !updatePath(a["path"])) return "invalid_update_path";
  if (!text(a["version"], 1, 64) || !identifier(a["version"])) return "invalid_update_version";
  if (!text(a["target"], 1, 64) || strcmp(a["target"], Target)) return "wrong_update_target";
  if (!a["size"].is<uint32_t>() || a["size"].as<uint32_t>() < 268 || a["size"].as<uint32_t>() > MaxFirmware) return "invalid_update_size";
  if (!text(a["sha256"], 64, 64) || !sha256(a["sha256"])) return "invalid_update_sha256";
  return nullptr;
}
inline bool envelope(JsonObjectConst j) {
  return j["seq"].is<uint32_t>() && j["seq"].as<uint32_t>() > 0 &&
      text(j["id"], 1, 64) && identifier(j["id"]) && text(j["type"], 1, 32) &&
      j["args"].is<JsonObjectConst>();
}
}
