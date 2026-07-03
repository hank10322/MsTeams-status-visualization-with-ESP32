#include <WiFi.h>
#include <PubSubClient.h>
#include <FastLED.h>
#include <WebServer.h>
#include <Preferences.h>
#include "time.h"
#include "esp_wifi_types.h"

// ===================== 使用者設定 =====================

// 預設 Wi-Fi，可在 Web 設定頁更新
String current_ssid = "Albert00123";
String current_password = "09059128761";

// Wi-Fi 失敗時開啟的 AP 名稱，手機連這個 AP 後進入 192.168.4.1
const char* ap_ssid = "ESP32_Teams_Config";

// MQTT 設定
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "teams/test/alber_sa126_01";

// WS2812 設定
// 建議外接 WS2812 用 GPIO10，比 GPIO8 安全
#define LED_PIN 6
#define NUM_LEDS 8

// 若你只用 ESP32-C3 SuperMini 板載 RGB，請改成：
// #define LED_PIN 8
// #define NUM_LEDS 1

#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define DEFAULT_BRIGHTNESS 120

// NTP 台灣時區 UTC+8
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 28800;
const int daylightOffset_sec = 0;

// ===================== 全域物件 =====================

CRGB leds[NUM_LEDS];

WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Preferences prefs;

// ===================== 狀態變數 =====================

String currentStatus = "Syncing...";
String lastStatusChangeTime = "尚未收到資料";
String scannedWifiHtml = "";

bool isAPMode = false;
bool wifiEverConnected = false;
bool ntpConfigured = false;

unsigned long wifiLostTime = 0;
unsigned long lastReconnectAttempt = 0;
unsigned long lastLedUpdate = 0;

volatile wifi_err_reason_t lastWifiFailReason = WIFI_REASON_UNSPECIFIED;
String wifiReasonText = "Connecting";
String mqttStatusText = "Disconnected";

wl_status_t lastPrintedStatus = (wl_status_t)(-1);

// ===================== 工具函式 =====================

void netLog(const String& msg) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print(" ms][系統] ");
  Serial.println(msg);
}

String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

String wlStatusToText(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS: return "閒置中";
    case WL_NO_SSID_AVAIL: return "找不到SSID";
    case WL_SCAN_COMPLETED: return "掃描完成";
    case WL_CONNECTED: return "已連線";
    case WL_CONNECT_FAILED: return "連線失敗";
    case WL_CONNECTION_LOST: return "連線遺失";
    case WL_DISCONNECTED: return "已斷線";
    default: return "未知狀態";
  }
}

String reasonToText(wifi_err_reason_t reason) {
  switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
      return "找不到AP";
    case WIFI_REASON_AUTH_FAIL:
      return "驗證失敗";
    case WIFI_REASON_AUTH_EXPIRE:
      return "驗證逾時";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "握手逾時";
#ifdef WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
      return "四向交握逾時";
#endif
    case WIFI_REASON_ASSOC_FAIL:
      return "關聯失敗";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "Beacon逾時";
    default:
      return "連線中";
  }
}

bool isAuthFailReason(wifi_err_reason_t reason) {
  if (reason == WIFI_REASON_AUTH_FAIL ||
      reason == WIFI_REASON_AUTH_EXPIRE ||
      reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
    return true;
  }

#ifdef WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
  if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
    return true;
  }
#endif

  return false;
}

// ===================== WS2812 顯示 =====================

void showAll(CRGB color, uint8_t brightness) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.setBrightness(brightness);
  FastLED.show();
}

void showBreath(CRGB color, uint8_t bpm, uint8_t minBrightness, uint8_t maxBrightness) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.setBrightness(beatsin8(bpm, minBrightness, maxBrightness));
  FastLED.show();
}

void showBlink(CRGB color, uint16_t intervalMs, uint8_t brightness) {
  bool on = ((millis() / intervalMs) % 2) == 0;
  fill_solid(leds, NUM_LEDS, on ? color : CRGB::Black);
  FastLED.setBrightness(brightness);
  FastLED.show();
}

void showAlternate(CRGB a, CRGB b, uint16_t intervalMs, uint8_t brightness) {
  bool first = ((millis() / intervalMs) % 2) == 0;
  fill_solid(leds, NUM_LEDS, first ? a : b);
  FastLED.setBrightness(brightness);
  FastLED.show();
}

void showMovingDot(CRGB color, uint8_t brightness) {
  fadeToBlackBy(leds, NUM_LEDS, 40);

  int pos = 0;
  if (NUM_LEDS > 1) {
    pos = beatsin16(30, 0, NUM_LEDS - 1);
  }

  leds[pos] = color;
  FastLED.setBrightness(brightness);
  FastLED.show();
}

void showRandomFlicker(CRGB color, uint8_t minBrightness, uint8_t maxBrightness) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.setBrightness(random8(minBrightness, maxBrightness));
  FastLED.show();
}

void updateLights() {
  // 降低 FastLED show 頻率，避免太頻繁刷新
  if (millis() - lastLedUpdate < 20) return;
  lastLedUpdate = millis();

  wl_status_t wifiStatus = WiFi.status();

  // AP 設定模式：藍色呼吸
  if (isAPMode) {
    showBreath(CRGB::Blue, 25, 10, 130);
    return;
  }

  // Wi-Fi 未連線：依照錯誤原因顯示
  if (wifiStatus != WL_CONNECTED) {
    if (lastWifiFailReason == WIFI_REASON_NO_AP_FOUND) {
      // 找不到 Wi-Fi：黃色慢閃
      showBlink(CRGB::Yellow, 700, 120);
    } else if (isAuthFailReason(lastWifiFailReason)) {
      // 密碼錯或驗證失敗：紅色快閃
      showBlink(CRGB::Red, 180, 140);
    } else {
      // 其他連線中狀態：紅色呼吸
      showBreath(CRGB::Red, 45, 10, 150);
    }
    return;
  }

  // Wi-Fi 已連線但 MQTT 尚未連線
  if (!mqttClient.connected()) {
    if (mqttStatusText.startsWith("Failed")) {
      // MQTT 連線失敗：紅藍交替
      showAlternate(CRGB::Red, CRGB::Blue, 350, 120);
    } else {
      // MQTT 連線中：青色呼吸
      showBreath(CRGB::Cyan, 30, 10, 120);
    }
    return;
  }

  // Teams 狀態顯示
  String localStatus = currentStatus;

  if (localStatus == "Syncing...") {
    showBreath(CRGB::Yellow, 30, 15, 130);
  } else if (localStatus == "Available") {
    showBreath(CRGB::Green, 35, 20, 140);
  } else if (localStatus == "Busy" || localStatus == "InAMeeting") {
    showAll(CRGB::Red, 140);
  } else if (localStatus == "DoNotDisturb") {
    showMovingDot(CRGB::Purple, 150);
  } else if (localStatus == "Away") {
    showBreath(CRGB::Orange, 35, 20, 140);
  } else if (localStatus == "BeRightBack") {
    showBlink(CRGB::Orange, 500, 130);
  } else if (localStatus == "Offline") {
    showAll(CRGB::Blue, 20);
  } else {
    // 未知狀態：低亮度藍色
    showAll(CRGB::Blue, 35);
  }
}

// ===================== Wi-Fi 事件 =====================

void WiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    wifiEverConnected = true;
    lastWifiFailReason = WIFI_REASON_UNSPECIFIED;
    wifiReasonText = "Connected";
    netLog("已連上基地台，SSID：" + String(current_ssid));
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiReasonText = "Connected";
    netLog("已取得 IP：" + WiFi.localIP().toString());

    if (!ntpConfigured) {
      netLog("初始化 NTP 時間同步...");
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      ntpConfigured = true;
    }
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiFailReason = (wifi_err_reason_t)info.wifi_sta_disconnected.reason;
    wifiReasonText = reasonToText(lastWifiFailReason);
    mqttStatusText = "Disconnected";

    netLog("Wi-Fi 斷線，原因碼=" + String((int)lastWifiFailReason) +
           "，原因：" + wifiReasonText);
  } else if (event == ARDUINO_EVENT_WIFI_AP_START) {
    netLog("AP 模式已啟動");
  } else if (event == ARDUINO_EVENT_WIFI_AP_STOP) {
    netLog("AP 模式已關閉");
  }
}

// ===================== MQTT =====================

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";

  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  msg.trim();

  if (msg.length() == 0) return;

  netLog("收到 MQTT 訊息: " + msg);

  if (currentStatus != msg) {
    netLog("狀態更新：" + currentStatus + " -> " + msg);
    currentStatus = msg;

    struct tm ti;
    if (ntpConfigured && getLocalTime(&ti)) {
      char buf[10];
      strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
      lastStatusChangeTime = String(buf);
    } else {
      lastStatusChangeTime = "時間未知";
    }
  }
}

String makeMqttClientId() {
  uint64_t chipid = ESP.getEfuseMac();

  String id = "ESP32C3Teams-";
  id += String((uint32_t)(chipid >> 32), HEX);
  id += String((uint32_t)chipid, HEX);

  return id;
}

void tryReconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (millis() - lastReconnectAttempt < 5000) return;

  lastReconnectAttempt = millis();
  mqttStatusText = "Connecting";

  String clientId = makeMqttClientId();

  netLog("嘗試連線 MQTT，Client ID：" + clientId);

  if (mqttClient.connect(clientId.c_str())) {
    mqttStatusText = "Connected";
    mqttClient.subscribe(mqtt_topic);

    netLog("MQTT 連線成功");
    netLog("已訂閱 Topic：" + String(mqtt_topic));
  } else {
    mqttStatusText = "Failed(" + String(mqttClient.state()) + ")";
    netLog("MQTT 連線失敗，state=" + String(mqttClient.state()));
  }
}

// ===================== Preferences =====================

void loadWiFiSettings() {
  prefs.begin("wifi", true);
  current_ssid = prefs.getString("ssid", current_ssid);
  current_password = prefs.getString("pass", current_password);
  prefs.end();

  netLog("載入已儲存 Wi-Fi SSID：" + current_ssid);
}

void saveWiFiSettings() {
  prefs.begin("wifi", false);
  prefs.putString("ssid", current_ssid);
  prefs.putString("pass", current_password);
  prefs.end();

  netLog("Wi-Fi 設定已寫入記憶體");
}

// ===================== Web 設定頁 =====================

String getStatusColor() {
  if (currentStatus == "Available") return "#28a745";
  if (currentStatus == "Busy" || currentStatus == "InAMeeting") return "#d73a49";
  if (currentStatus == "Away" || currentStatus == "BeRightBack") return "#e2b100";
  if (currentStatus == "DoNotDisturb") return "#7e57c2";
  if (currentStatus == "Offline") return "#6c757d";
  return "#007bff";
}

void handleRoot() {
  String statusColor = getStatusColor();

  String wifiText = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "未連接";
  String ipText = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "-";
  String apText = isAPMode ? "已開啟，IP: 192.168.4.1" : "未開啟";

  String html = "<!DOCTYPE html><html lang='zh-TW'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>ESP32-C3 Teams Status</title>";
  html += "<style>";
  html += "body{font-family:'Segoe UI',Arial,sans-serif;background:#f0f2f5;display:flex;justify-content:center;align-items:center;flex-direction:column;min-height:100vh;margin:0;padding:20px;box-sizing:border-box;}";
  html += ".card{background:white;border-radius:20px;padding:26px;box-shadow:0 10px 20px rgba(0,0,0,0.1);text-align:center;width:100%;max-width:380px;margin-bottom:18px;box-sizing:border-box;}";
  html += ".status-val{font-size:2.1em;font-weight:bold;color:" + statusColor + ";margin:18px 0;word-break:break-word;}";
  html += ".time-info{text-align:left;color:#555;font-size:0.95em;border-top:1px solid #eee;padding-top:15px;line-height:2.0;}";
  html += ".row{display:flex;justify-content:space-between;gap:10px;border-bottom:1px dashed #eee;}";
  html += ".row:last-child{border-bottom:none;}";
  html += ".highlight{color:#000;font-weight:600;text-align:right;word-break:break-word;}";
  html += "input[type=text],input[type=password],select{width:100%;padding:10px;margin:10px 0;border:1px solid #ddd;border-radius:8px;box-sizing:border-box;font-size:1em;}";
  html += "input[type=submit],.btn-scan{width:100%;background:#007bff;color:white;border:none;padding:12px;border-radius:8px;cursor:pointer;font-weight:bold;font-size:1em;}";
  html += ".btn-scan{background:#6c757d;margin-bottom:10px;text-decoration:none;display:block;}";
  html += ".hint{font-size:0.85em;color:#777;text-align:left;line-height:1.6;}";
  html += "</style></head><body>";

  html += "<div class='card'>";
  html += "<h2>Teams 狀態監控</h2>";
  html += "<div class='status-val'>" + htmlEscape(currentStatus) + "</div>";
  html += "<div class='time-info'>";
  html += "<div class='row'><span>現在時間</span><span id='clock' class='highlight'>--:--:--</span></div>";
  html += "<div class='row'><span>狀態更新</span><span class='highlight'>" + htmlEscape(lastStatusChangeTime) + "</span></div>";
  html += "<div class='row'><span>Wi-Fi</span><span class='highlight'>" + htmlEscape(wifiText) + "</span></div>";
  html += "<div class='row'><span>IP</span><span class='highlight'>" + htmlEscape(ipText) + "</span></div>";
  html += "<div class='row'><span>Wi-Fi 狀態</span><span class='highlight'>" + htmlEscape(wifiReasonText) + "</span></div>";
  html += "<div class='row'><span>MQTT</span><span class='highlight'>" + htmlEscape(mqttStatusText) + "</span></div>";
  html += "<div class='row'><span>AP 設定模式</span><span class='highlight'>" + htmlEscape(apText) + "</span></div>";
  html += "<div class='row'><span>LED Pin</span><span class='highlight'>GPIO" + String(LED_PIN) + "</span></div>";
  html += "</div></div>";

  html += "<div class='card'>";
  html += "<h3>Wi-Fi 設定</h3>";

  html += "<form action='/scan' method='POST'>";
  html += "<input type='submit' class='btn-scan' value='掃描周邊網路'>";
  html += "</form>";

  html += "<form action='/save' method='POST'>";

  if (scannedWifiHtml.length() > 0) {
    html += "<div class='hint'>選擇網路，或在下方手動輸入 SSID。</div>";
    html += "<select name='s_select'>" + scannedWifiHtml + "</select>";
  } else {
    html += "<div class='hint'>尚未掃描。可以直接手動輸入 SSID。</div>";
  }

  html += "<input type='text' name='s' placeholder='輸入 Wi-Fi 名稱'>";
  html += "<input type='password' name='p' placeholder='密碼，無密碼則留空'>";
  html += "<input type='submit' value='更新並重新連線'>";
  html += "</form>";

  html += "<p class='hint'>Wi-Fi 連不上超過約 10 秒時，裝置會開啟 AP：";
  html += htmlEscape(String(ap_ssid));
  html += "，手機連上後開啟 192.168.4.1 設定。</p>";

  html += "</div>";

  html += "<script>";
  html += "function updateClock(){const d=new Date();document.getElementById('clock').innerHTML=d.toTimeString().split(' ')[0];}";
  html += "setInterval(updateClock,1000);updateClock();";
  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleScan() {
  netLog("暫停 STA 連線以利掃描...");
  WiFi.disconnect();
  delay(150);

  netLog("開始掃描周邊 Wi-Fi...");
  int n = WiFi.scanNetworks(false, false, false, 150);

  scannedWifiHtml = "<option value=''>-- 請選擇 --</option>";

  if (n == 0) {
    netLog("未找到任何 Wi-Fi 網路");
  } else {
    netLog("掃描完成，找到 " + String(n) + " 個網路");

    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      String safeSsid = htmlEscape(ssid);

      scannedWifiHtml += "<option value='";
      scannedWifiHtml += safeSsid;
      scannedWifiHtml += "'>";
      scannedWifiHtml += safeSsid;
      scannedWifiHtml += " (";
      scannedWifiHtml += String(WiFi.RSSI(i));
      scannedWifiHtml += " dBm)";
      scannedWifiHtml += WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " 開放" : " 加密";
      scannedWifiHtml += "</option>";
    }
  }

  WiFi.scanDelete();

  if (current_ssid.length() > 0) {
    if (current_password.length() > 0) {
      WiFi.begin(current_ssid.c_str(), current_password.c_str());
    } else {
      WiFi.begin(current_ssid.c_str());
    }
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSave() {
  String new_ssid = "";

  if (server.hasArg("s") && server.arg("s").length() > 0) {
    new_ssid = server.arg("s");
  } else if (server.hasArg("s_select")) {
    new_ssid = server.arg("s_select");
  }

  new_ssid.trim();

  if (new_ssid.length() > 0) {
    current_ssid = new_ssid;
    current_password = server.hasArg("p") ? server.arg("p") : "";

    saveWiFiSettings();

    String html = "<!DOCTYPE html><html lang='zh-TW'><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial,sans-serif;padding:30px;line-height:1.8;}</style>";
    html += "</head><body>";
    html += "<h2>Wi-Fi 設定已更新</h2>";
    html += "<p>裝置正在重新連線到：<b>" + htmlEscape(current_ssid) + "</b></p>";
    html += "<p>如果目前是 AP 設定模式，請稍後重新整理 192.168.4.1，或回到你的正常 Wi-Fi 網路查看裝置 IP。</p>";
    html += "<p><a href='/'>回首頁</a></p>";
    html += "</body></html>";

    server.send(200, "text/html", html);
    delay(800);

    mqttClient.disconnect();
    mqttStatusText = "Disconnected";
    currentStatus = "Syncing...";
    lastStatusChangeTime = "尚未收到資料";

    WiFi.disconnect(false);

    if (current_password.length() > 0) {
      WiFi.begin(current_ssid.c_str(), current_password.c_str());
    } else {
      netLog("嘗試連線至無密碼公開網路：" + current_ssid);
      WiFi.begin(current_ssid.c_str());
    }

    wifiLostTime = millis();
  } else {
    server.send(400, "text/plain", "SSID 缺失");
  }
}

// ===================== Wi-Fi 狀態管理 =====================

void connectToConfiguredWiFi() {
  if (current_ssid.length() == 0) {
    netLog("沒有設定 SSID，等待 AP 設定模式");
    return;
  }

  netLog("開始連線 Wi-Fi：" + current_ssid);

  if (current_password.length() > 0) {
    WiFi.begin(current_ssid.c_str(), current_password.c_str());
  } else {
    WiFi.begin(current_ssid.c_str());
  }

  wifiLostTime = millis();
}

void startConfigAP() {
  if (isAPMode) return;

  netLog("超過 10 秒未連上 Wi-Fi，啟動 AP 設定模式");
  WiFi.mode(WIFI_AP_STA);

  // 不帶密碼，開放式 AP
  bool ok = WiFi.softAP(ap_ssid);

  if (ok) {
    isAPMode = true;
    netLog("AP 名稱：" + String(ap_ssid) + "，AP IP：" + WiFi.softAPIP().toString());
  } else {
    netLog("AP 啟動失敗");
  }
}

void stopConfigAPIfNeeded() {
  if (!isAPMode) return;

  netLog("偵測到 Wi-Fi 已連上，準備關閉 AP 設定模式");
  WiFi.softAPdisconnect(true);
  isAPMode = false;
  WiFi.mode(WIFI_STA);
  netLog("AP 模式已關閉，切回 STA 模式");
}

void handleWiFiState() {
  wl_status_t s = WiFi.status();

  if (s != lastPrintedStatus) {
    netLog("Wi-Fi 狀態改變：" + wlStatusToText(lastPrintedStatus) + " -> " + wlStatusToText(s));
    lastPrintedStatus = s;
  }

  if (s == WL_CONNECTED) {
    wifiLostTime = millis();
    stopConfigAPIfNeeded();
  } else {
    mqttStatusText = "Disconnected";

    if (wifiLostTime == 0) {
      wifiLostTime = millis();
    }

    if (millis() - wifiLostTime > 10000) {
      startConfigAP();
    }
  }
}

// ===================== Arduino setup / loop =====================

void setup() {
  Serial.begin(115200);
  delay(500);

  netLog("ESP32-C3 SuperMini Teams WS2812 版本開機");

  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(DEFAULT_BRIGHTNESS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);
  FastLED.clear(true);

  // 開機測試燈號
  showAll(CRGB::Red, 80);
  delay(180);
  showAll(CRGB::Green, 80);
  delay(180);
  showAll(CRGB::Blue, 80);
  delay(180);
  FastLED.clear(true);

  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_STA);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  loadWiFiSettings();
  connectToConfiguredWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/scan", HTTP_POST, handleScan);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();

  wifiLostTime = millis();

  netLog("WebServer 已啟動");
  netLog("MQTT Topic：" + String(mqtt_topic));
  netLog("WS2812 GPIO：" + String(LED_PIN));
  netLog("系統啟動完成");
}

void loop() {
  server.handleClient();

  handleWiFiState();

  if (WiFi.status() == WL_CONNECTED) {
    tryReconnectMQTT();
    mqttClient.loop();
  }

  updateLights();

  delay(10);
}