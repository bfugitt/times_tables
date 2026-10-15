#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

bool wifiConnect(uint32_t timeoutMs);
// GET ?<query>&key=... ; fills doc and returns true when the script answers ok
bool apiGet(const String& query, JsonDocument& doc);
// POSTs doc (key and device are added); returns true when the script confirms
bool apiPost(JsonDocument& doc);
String urlEncode(const String& s);
