// Generic, provisioned-over-USB firmware: no secrets.h, web UI, model or compiler.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <Updater.h>
#include <WiFiClientSecureBearSSL.h>
#include <umm_malloc/umm_heap_select.h>
#include <time.h>
#include "protocol.h"

extern "C" uint32_t _FS_start, _FS_end;
namespace {
constexpr char Version[] = "minimal-agent-0.1.0";
constexpr char ConfigFile[] = "/minimal.json";
// Preserve the same high-water/result journal when changing firmware families.
constexpr char JournalFile[] = "/gateway-job.json";
constexpr char AckFile[] = "/gateway-ack";
String ssid, wifiPassword, serverUrl, token, caPem, publicKey;
String device, boot, journal, serialLine, error = "unconfigured";
bool storage = false, configured = false, enabled = false, pending = false;
bool lineOverflow = false, paused = false;
uint32_t lastSeq = 0, pollsOk = 0, resultsOk = 0, lastSuccess = 0;
uint32_t nextAt = 0, pollMs = 3000, retryMs = 3000, rebootAt = 0;
int httpStatus = 0, tlsError = 0;
BearSSL::Session tlsSession;

String readFile(const char* path, size_t limit) {
  File f = LittleFS.open(path, "r");
  if (!f || f.size() > limit) return String();
  return f.readString();
}
bool save(const char* path, const String& content) {
  if (!storage) return false;
  String temp = String(path) + ".tmp";
  File f = LittleFS.open(temp, "w");
  if (!f) return false;
  bool ok = f.print(content) == content.length();
  f.flush(); f.close();
  return ok && LittleFS.rename(temp, path);
}
bool mount() {
  LittleFS.setConfig(LittleFSConfig(false));
  if (LittleFS.begin()) return true;
  uint32_t first = reinterpret_cast<uint32_t>(&_FS_start) - 0x40200000u;
  uint32_t end = reinterpret_cast<uint32_t>(&_FS_end) - 0x40200000u;
  if (first >= end || end > ESP.getFlashChipRealSize()) return false;
  uint32_t data[64];
  for (uint32_t at = first; at < end; at += sizeof(data)) {
    if (!ESP.flashRead(at, data, sizeof(data))) return false;
    for (uint32_t word : data) if (word != 0xffffffffu) return false;
    yield();
  }
  return LittleFS.format() && LittleFS.begin();
}
const char* validateConfig(JsonObjectConst obj) {
  const char* failure = minimal::configError(obj);
  if (failure) return failure;
  BearSSL::X509List anchors(obj["ca_pem"].as<const char*>());
  if (!anchors.getCount()) return "invalid_ca_certificate";
  const char* key = obj["firmware_public_key"] | "";
  if (*key) {
    BearSSL::PublicKey parsed(key);
    if (!parsed.isRSA() || parsed.getRSA()->nlen != 256) return "rsa2048_public_key_required";
  }
  return nullptr;
}
void loadConfig(JsonObjectConst obj) {
  // A changed host or trust anchor must never inherit an old authenticated
  // session. Resume only connections made under the current configuration.
  tlsSession = BearSSL::Session();
  ssid = obj["wifi_ssid"].as<String>(); wifiPassword = obj["wifi_password"].as<String>();
  serverUrl = obj["server_url"].as<String>(); token = obj["device_token"].as<String>();
  caPem = obj["ca_pem"].as<String>(); publicKey = obj["firmware_public_key"] | "";
  pollMs = obj["poll_ms"]; retryMs = pollMs; enabled = obj["enabled"];
  configured = true; paused = false; nextAt = millis() + 1000;
  error = enabled ? "waiting_for_wifi_and_clock" : "disabled";
}
void envelope(JsonDocument& d) {
  d["protocol"] = 1; d["device_id"] = device; d["boot_id"] = boot;
}
void state(JsonObject o) {
  o["version"] = Version; o["protocol"] = 1; o["device_id"] = device; o["boot_id"] = boot;
  o["target"] = minimal::Target;
  o["wifi"] = WiFi.status() == WL_CONNECTED; o["ip"] = WiFi.localIP().toString();
  o["uptime_s"] = millis() / 1000; o["heap"] = ESP.getFreeHeap();
  o["max_block"] = ESP.getMaxFreeBlockSize();
  { HeapSelectIram selected; o["iram_heap"] = ESP.getFreeHeap(); }
  o["configured"] = configured; o["enabled"] = enabled && !paused;
  o["storage_ready"] = storage; o["server_url"] = serverUrl;
  o["token_configured"] = !token.isEmpty(); o["ca_configured"] = !caPem.isEmpty();
  o["signed_ota_ready"] = !publicKey.isEmpty();
  o["last_seq"] = lastSeq; o["result_pending"] = pending;
  o["polls_ok"] = pollsOk; o["results_ok"] = resultsOk;
  o["last_success_uptime_s"] = lastSuccess / 1000;
  o["http_status"] = httpStatus; o["tls_error"] = tlsError;
  o["poll_ms"] = pollMs; o["error"] = error;
}
String status() {
  DynamicJsonDocument d(1536); state(d.to<JsonObject>());
  String out; serializeJson(d, out); return out;
}
class Body : public Stream {
 public:
  String data;
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    if (count > minimal::MaxWire - data.length()) return 0;
    return data.concat(reinterpret_cast<const char*>(bytes), count) ? count : 0;
  }
  int availableForWrite() override { return minimal::MaxWire - data.length(); }
  bool outputCanTimeout() override { return false; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};
bool post(const char* path, const String& request, String& response) {
  if (ESP.getFreeHeap() < 28500) { error = "insufficient_tls_heap"; return false; }
  BearSSL::WiFiClientSecure client;
  BearSSL::X509List anchors(caPem.c_str());
  client.setTrustAnchors(&anchors); client.setX509Time(time(nullptr));
  client.setBufferSizes(16384, 1024); client.setSSLVersion(BR_TLS12, BR_TLS12);
  client.setSession(&tlsSession); client.setTimeout(5000);
  HTTPClient http; http.setTimeout(5000); http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(client, serverUrl + path)) { error = "http_begin_failed"; return false; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + token);
  { HeapSelectIram second; httpStatus = http.POST(request); }
  tlsError = client.getLastSSLError();
  Body body;
  bool ok = httpStatus == 200 && http.getSize() <= static_cast<int>(minimal::MaxWire);
  if (ok) ok = http.writeToStream(&body) >= 0;
  http.end(); client.stop();
  if (!ok) { error = "gateway_transport_failed"; return false; }
  response = std::move(body.data); return true;
}
bool result(uint32_t seq, const String& id, const char* outcome, bool ok, const String& data) {
  DynamicJsonDocument d(2048); envelope(d);
  d["seq"] = seq; d["id"] = id; d["status"] = outcome; d["ok"] = ok;
  d["data"] = serialized(data);
  String out; serializeJson(d, out);
  if (d.overflowed() || out.length() > minimal::MaxWire || !save(JournalFile, out)) {
    enabled = false; error = "journal_write_failed"; return false;
  }
  journal = std::move(out); lastSeq = seq; pending = true; return true;
}

// Stream into the OTA staging area. Updater commits only after both the package
// SHA256 and the required RSA signature validate; never buffer a whole image.
bool updateFirmware(const String& path, uint32_t bytes, const String& sha) {
  if (publicKey.isEmpty()) { error = "firmware_signing_key_not_configured"; return false; }
  if (ESP.getFreeHeap() < 28500 || bytes > ESP.getFreeSketchSpace()) { error = "insufficient_update_space"; return false; }
  BearSSL::PublicKey key(publicKey.c_str());
  BearSSL::HashSHA256 imageHash;
  BearSSL::SigningVerifier verifier(&key);
  BearSSL::HashSHA256 packageHash;
  Update.installSignature(&imageHash, &verifier);
  bool committed = false;
  {
    BearSSL::WiFiClientSecure client;
    BearSSL::X509List anchors(caPem.c_str());
    client.setTrustAnchors(&anchors); client.setX509Time(time(nullptr));
    client.setBufferSizes(16384, 1024); client.setSSLVersion(BR_TLS12, BR_TLS12);
    client.setSession(&tlsSession); client.setTimeout(8000);
    HTTPClient http; http.setTimeout(8000); http.setReuse(false);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    if (!http.begin(client, serverUrl + path)) error = "update_http_begin_failed";
    else {
      http.addHeader("Authorization", String("Bearer ") + token);
      { HeapSelectIram second; httpStatus = http.GET(); }
      tlsError = client.getLastSSLError();
      if (httpStatus != 200 || http.getSize() != static_cast<int>(bytes)) error = "update_http_or_size_mismatch";
      else if (!Update.begin(bytes, U_FLASH)) error = "update_begin_failed";
      else {
        packageHash.begin(); uint32_t received = 0, started = millis(), lastByte = started;
        size_t withheld = 0;
        uint8_t buffer[512];
        WiFiClient* stream = http.getStreamPtr();
        while (received < bytes && millis() - started < 180000 && millis() - lastByte < 8000) {
          int ready = stream->available();
          if (ready > 0) {
            size_t count = min<size_t>(sizeof(buffer), min<uint32_t>(ready, bytes - received));
            int n = stream->read(buffer, count);
            if (n <= 0) break;
            packageHash.add(buffer, n);
            // Keep the final block out of Updater until the package hash is
            // checked. end(false) can then safely abort any rejected download:
            // it must never see a complete image on a SHA-mismatch path.
            if (received + static_cast<uint32_t>(n) == bytes) {
              received += n; withheld = n; break;
            }
            if (Update.write(buffer, n) != static_cast<size_t>(n)) break;
            received += n; lastByte = millis();
          } else if (!stream->connected()) break;
          delay(0);
        }
        packageHash.end();
        const uint8_t* digest = static_cast<const uint8_t*>(packageHash.hash());
        char hex[65];
        for (size_t i = 0; i < 32; ++i) snprintf(hex + 2*i, 3, "%02x", digest[i]);
        if (received != bytes) error = "update_incomplete_or_timeout";
        else if (!sha.equalsIgnoreCase(hex)) error = "update_package_sha256_mismatch";
        else if (!withheld || Update.write(buffer, withheld) != withheld) error = "update_write_failed";
        else if (!Update.end()) error = "update_signature_or_image_rejected";
        else { committed = true; error = "none"; }
        if (!committed) Update.end(false); // abort incomplete/unverified staging
      }
      http.end(); client.stop();
    }
  }
  // Never leave Updater holding pointers to stack-local cryptographic objects.
  Update.installSignature(nullptr, nullptr);
  return committed;
}

void receivePoll(String& response) {
  DynamicJsonDocument d(1536);
  if (response.isEmpty() || deserializeJson(d, &response[0]) || !d["protocol"].is<int>() ||
      d["protocol"] != 1 || !d.containsKey("job")) { error = "invalid_poll"; return; }
  if (d["poll_after_ms"].is<uint32_t>()) pollMs = constrain(d["poll_after_ms"].as<uint32_t>(), 1000u, 60000u);
  if (d["job"].isNull()) { ++pollsOk; lastSuccess = millis(); error = "none"; return; }
  JsonObjectConst j = d["job"].as<JsonObjectConst>();
  if (!minimal::envelope(j)) { error = "invalid_job_envelope"; return; }
  ++pollsOk; lastSuccess = millis();
  uint32_t seq = j["seq"]; String id = j["id"].as<String>();
  if (seq <= lastSeq) { pending = !journal.isEmpty(); error = "duplicate_job"; return; }
  String kind = j["type"].as<String>(), path, wantedVersion, sha;
  uint32_t bytes = 0;
  const char* invalid = nullptr;
  bool inspect = kind == "device_inspect";
  if (kind == "firmware_update") {
    invalid = minimal::updateError(j["args"].as<JsonObjectConst>());
    if (!invalid) {
      path = j["args"]["path"].as<String>(); wantedVersion = j["args"]["version"].as<String>();
      sha = j["args"]["sha256"].as<String>(); bytes = j["args"]["size"];
    }
  } else if (inspect) {
    if (!minimal::text(j["args"]["resource"], 1, 80) || strcmp(j["args"]["resource"], "environment"))
      invalid = "resource_hosted_on_server_or_unavailable";
  } else if (kind != "status") invalid = "unsupported_type_on_minimal_firmware";
  String failure = invalid ? invalid : "";
  d.clear(); response = String();
  if (!result(seq, id, "indeterminate", false, "{\"error\":\"execution_interrupted_or_result_not_committed\"}")) return;
  bool ok = failure.isEmpty(); String data;
  if (!ok) {
    DynamicJsonDocument detail(192); detail["error"] = failure; serializeJson(detail, data);
  } else if (kind == "firmware_update") {
    ok = updateFirmware(path, bytes, sha);
    DynamicJsonDocument detail(256);
    detail["state"] = ok ? "verified_pending_reboot" : "rejected";
    detail["requested_version"] = wantedVersion; detail["error"] = error;
    serializeJson(detail, data);
    if (ok) rebootAt = millis() + 10000;
  } else data = status();
  if (result(seq, id, "completed", ok, data)) error = "none";
}
void tick() {
  if (rebootAt && (!pending || static_cast<int32_t>(millis() - rebootAt) >= 0)) { ESP.restart(); return; }
  if (!enabled || paused || !configured || !storage || !serialLine.isEmpty() ||
      WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000 ||
      static_cast<int32_t>(millis() - nextAt) < 0) return;
  String request, response; uint32_t previousPolls = pollsOk;
  bool success = false;
  if (pending) {
    if (post("/v1/device/result", journal, response)) {
      DynamicJsonDocument ack(256);
      if (!deserializeJson(ack, response) && ack["protocol"] == 1 && ack["ack"].is<uint32_t>() &&
          ack["ack"].as<uint32_t>() == lastSeq && save(AckFile, String(lastSeq))) {
        pending = false; ++resultsOk; lastSuccess = millis(); error = "none"; success = true;
      } else error = "invalid_ack_or_checkpoint_failed";
    }
  } else {
    { DynamicJsonDocument d(768); envelope(d);
      d["firmware"] = Version; d["last_seq"] = lastSeq; d["uptime_s"] = millis()/1000;
      d["heap"] = ESP.getFreeHeap(); d["rssi"] = WiFi.RSSI();
      JsonArray types = d.createNestedArray("job_types");
      types.add("status"); types.add("device_inspect"); types.add("firmware_update");
      d["target"] = minimal::Target; d["signed_ota_ready"] = !publicKey.isEmpty();
      serializeJson(d, request); }
    if (post("/v1/device/poll", request, response)) receivePoll(response);
    success = pollsOk > previousPolls;
  }
  retryMs = success ? pollMs : min<uint32_t>(max<uint32_t>(retryMs*2, 3000), 60000);
  nextAt = millis() + (pending && success ? 100u : retryMs);
}
void serialControl() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (lineOverflow) error = "serial_line_too_long";
      else if (serialLine.startsWith("config ")) {
        DynamicJsonDocument d(1536);
        if (deserializeJson(d, &serialLine[7])) error = "invalid_config_json";
        else {
          const char* failure = validateConfig(d.as<JsonObjectConst>());
          if (failure) error = failure;
          else {
            // Only the documented fields enter persistent configuration.
            DynamicJsonDocument clean(512);
            for (const char* name : {"wifi_ssid","wifi_password","server_url","device_token","ca_pem","enabled","allow_lan_http","poll_ms","firmware_public_key"})
              if (d.containsKey(name)) clean[name] = d[name];
            String out; serializeJson(clean, out);
            if (clean.overflowed() || !save(ConfigFile, out)) error = "config_write_failed";
            else { loadConfig(d.as<JsonObjectConst>()); WiFi.begin(ssid.c_str(), wifiPassword.c_str()); }
          }
        }
      } else if (serialLine == "pause") { paused = true; error = "paused_until_resume_or_reboot"; }
      else if (serialLine == "resume") { paused = false; nextAt = millis(); error = "waiting_for_connection"; }
      else if (serialLine == "reboot") { Serial.println("{\"restarting\":true}"); Serial.flush(); ESP.restart(); }
      else if (serialLine != "status") error = "unknown_serial_command";
      serialLine = String(); lineOverflow = false;
      Serial.println(status()); // Never echo user input, tokens or Wi-Fi keys.
    } else if (c != '\r') {
      if (serialLine.length() < minimal::MaxConfig + 7) serialLine += c;
      else lineOverflow = true;
    }
  }
}
}
void setup() {
  Serial.begin(115200);
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true);
  WiFi.hostname("esp8266-minimal");
  device = "esp8266-" + WiFi.macAddress(); device.replace(":", ""); device.toLowerCase();
  char random[9]; snprintf(random, sizeof(random), "%08x", ESP.random()); boot = random;
  storage = mount();
  if (!storage) error = "storage_unavailable_manual_recovery_required";
  if (storage && LittleFS.exists(JournalFile)) {
    journal = readFile(JournalFile, 6144); DynamicJsonDocument d(6144);
    if (journal.isEmpty() || deserializeJson(d, journal) || !d["seq"].is<uint32_t>() ||
        d["device_id"].as<String>() != device || !minimal::text(d["id"],1,64) || !minimal::identifier(d["id"])) {
      storage = false; error = "invalid_journal_manual_recovery_required";
    } else { lastSeq = d["seq"]; pending = readFile(AckFile,10) != String(lastSeq); }
  }
  if (storage && LittleFS.exists(ConfigFile)) {
    String text = readFile(ConfigFile, minimal::MaxConfig); DynamicJsonDocument d(1536);
    if (text.isEmpty() || deserializeJson(d, &text[0])) error = "invalid_saved_config";
    else {
      const char* failure = validateConfig(d.as<JsonObjectConst>());
      if (failure) error = failure;
      else loadConfig(d.as<JsonObjectConst>());
    }
  }
  if (configured) WiFi.begin(ssid.c_str(), wifiPassword.c_str());
  configTime(0,0,"ntp.aliyun.com","time.cloudflare.com","pool.ntp.org");
  Serial.println(); Serial.println(status());
}
void loop() { serialControl(); tick(); delay(1); }
