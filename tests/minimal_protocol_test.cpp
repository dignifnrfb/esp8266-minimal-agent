#include "minimal/protocol.h"
#include <cstdlib>
#include <iostream>
#include <string>

int checks = 0;
void require(bool value, const char* name) {
  ++checks;
  if (!value) { std::cerr << "FAIL " << name << '\n'; std::exit(1); }
}
void config(DynamicJsonDocument& d) {
  d.clear(); d["wifi_ssid"] = "test-network"; d["wifi_password"] = "test-password";
  d["server_url"] = "https://esp.example.com"; d["device_token"] = "test-token-at-least-24-characters";
  d["ca_pem"] = std::string(80, 'x'); d["enabled"] = true; d["allow_lan_http"] = false; d["poll_ms"] = 3000;
}
void update(DynamicJsonDocument& d) {
  d.clear(); d["path"] = "/v1/firmware/test.bin.signed"; d["version"] = "minimal-agent-0.1.0";
  d["target"] = minimal::Target; d["size"] = 500000; d["sha256"] = std::string(64,'a');
}
int main() {
  DynamicJsonDocument d(4096);
  config(d); require(!minimal::configError(d.as<JsonObjectConst>()), "valid_config");
  for (const char* u : {"http://192.168.1.2", "https://user@host", "https:///bad", "https://host:0", "https://host:65536", "https://host:bad", "https://host/?q=1", "https://host/", "https://host\\evil", "https://host\r\nX:1"}) {
    config(d); d["server_url"] = u; require(minimal::configError(d.as<JsonObjectConst>()), "bad_url");
  }
  config(d); d["server_url"] = "https://host:8443/prefix"; require(!minimal::configError(d.as<JsonObjectConst>()), "https_prefix_port");
  config(d); d["device_token"] = "token-with-a-newline-characters\n"; require(minimal::configError(d.as<JsonObjectConst>()), "token_injection");
  config(d); d["device_token"] = "short"; require(minimal::configError(d.as<JsonObjectConst>()), "short_token");
  for (int n : {0,999,60001,-1}) { config(d); d["poll_ms"] = n; require(minimal::configError(d.as<JsonObjectConst>()), "poll_range"); }
  config(d); d["poll_ms"] = true; require(minimal::configError(d.as<JsonObjectConst>()), "poll_bool");
  config(d); d["poll_ms"] = 3000.5; require(minimal::configError(d.as<JsonObjectConst>()), "poll_float");
  config(d); d["allow_lan_http"] = true; require(minimal::configError(d.as<JsonObjectConst>()), "http_policy");
  config(d); d["wifi_password"] = ""; require(!minimal::configError(d.as<JsonObjectConst>()), "open_wifi");
  config(d); d["wifi_password"] = "short"; require(minimal::configError(d.as<JsonObjectConst>()), "short_wifi_password");
  config(d); d["wifi_ssid"] = std::string(33,'s'); require(minimal::configError(d.as<JsonObjectConst>()), "ssid_limit");
  const char* json = "{\"s\":\"abc\\u0000def\"}";
  deserializeJson(d,json); require(!minimal::text(d["s"],1,20), "embedded_nul");
  update(d); require(!minimal::updateError(d.as<JsonObjectConst>()), "valid_update");
  for (const char* p : {"https://evil/a", "/v1/firmware/../secret", "/v1/firmware/%2e%2e/a", "/v1/firmware/a?token=x", "/v1/firmware/", "/v1/firmware//a"}) {
    update(d); d["path"] = p; require(minimal::updateError(d.as<JsonObjectConst>()), "unsafe_path");
  }
  update(d); d["target"] = "esp32"; require(minimal::updateError(d.as<JsonObjectConst>()), "wrong_cpu");
  for (int n : {0,267,1048577,-1}) { update(d); d["size"] = n; require(minimal::updateError(d.as<JsonObjectConst>()), "size_range"); }
  update(d); d["size"] = true; require(minimal::updateError(d.as<JsonObjectConst>()), "size_bool");
  update(d); d["sha256"] = std::string(64,'z'); require(minimal::updateError(d.as<JsonObjectConst>()), "digest_hex");
  update(d); d["version"] = "bad version"; require(minimal::updateError(d.as<JsonObjectConst>()), "version_identifier");
  d.clear(); d["seq"] = 25; d["id"] = "job-25"; d["type"] = "status"; d.createNestedObject("args");
  require(minimal::envelope(d.as<JsonObjectConst>()), "valid_job");
  d["seq"] = 0; require(!minimal::envelope(d.as<JsonObjectConst>()), "zero_seq");
  d["seq"] = -1; require(!minimal::envelope(d.as<JsonObjectConst>()), "negative_seq");
  d["seq"] = true; require(!minimal::envelope(d.as<JsonObjectConst>()), "boolean_seq");
  d["seq"] = 25; d["id"] = "bad/id"; require(!minimal::envelope(d.as<JsonObjectConst>()), "unsafe_id");
  std::cout << "PASS " << checks << " minimal protocol checks\n";
}
