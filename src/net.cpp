#include "net.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "config.h"

bool wifiConnect(uint32_t timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(200);
  return WiFi.status() == WL_CONNECTED;
}

String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

// Plain GET that follows Google's redirect to the answer page.
static bool httpGet(const String& url, String& out) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) out = http.getString();
  http.end();
  return ok;
}

bool apiGet(const String& query, JsonDocument& doc) {
  if (!wifiConnect(5000)) return false;
  String body;
  String url = String(SCRIPT_URL) + "?" + query + "&key=" + urlEncode(DEVICE_KEY);
  if (!httpGet(url, body)) return false;
  if (deserializeJson(doc, body)) return false;
  return doc["ok"] | false;
}

bool apiPost(JsonDocument& doc) {
  if (!wifiConnect(5000)) return false;
  doc["key"] = DEVICE_KEY;
  doc["device"] = "Cardputer";
  String body;
  serializeJson(doc, body);

  int code = 0;
  String location, response;
  {
    // Apps Script runs doPost, then answers with a redirect to the result.
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setTimeout(20000);
    if (!http.begin(client, SCRIPT_URL)) return false;
    const char* headers[] = {"Location"};
    http.collectHeaders(headers, 1);
    http.addHeader("Content-Type", "text/plain");
    code = http.POST(body);
    location = http.header("Location");
    if (code == 200) response = http.getString();
    http.end();
  }

  if (code == 302 || code == 303) {
    if (!location.length()) return true;          // script ran; can't read its reply
    String out;
    if (!httpGet(location, out)) return true;     // script ran; reply page didn't load
    JsonDocument r;
    if (deserializeJson(r, out)) return true;
    return r["ok"] | false;                        // false = wrong key
  }
  if (code == 200) {
    JsonDocument r;
    if (deserializeJson(r, response)) return false;
    return r["ok"] | false;
  }
  return false;
}
