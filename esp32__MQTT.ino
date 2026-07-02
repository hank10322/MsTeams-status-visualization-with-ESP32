#include <WiFi.h>
#include <PubSubClient.h>
#include <FastLED.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WebServer.h>
#include "time.h"
#include "esp_wifi_types.h"

String current_ssid = "Your wifi_ssid";
String current_password = "Your wifi_password";
const char* ap_ssid = "ESP32_Teams_Config";
const char* ap_password = "password123";

// MQTT 設定
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_topic = "Your mqtt Topic";

#define LED_PIN 13
#define NUM_LEDS 8
#define BUTTON_PIN 14
#define STATUS_RED_LED 15
#define STATUS_YELLOW_LED 2
#define STATUS_BLUE_LED 4

CRGB leds[NUM_LEDS];
LiquidCrystal_I2C lcd(0x27, 16, 2);
WebServer server(80);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 28800;
const int daylightOffset_sec = 0;

String currentStatus = "Syncing...";
String lastStatusChangeTime = "尚未收到資料";
unsigned long modeStartTime = 0;
int displayMode = 0;
bool isAPMode = false;
unsigned long wifiLostTime = 0;
String scannedWifiHtml = "";

volatile wifi_err_reason_t lastWifiFailReason = WIFI_REASON_UNSPECIFIED;
String wifiReasonText = "Connecting";
bool wifiEverConnected = false;
String mqttStatusText = "Disconnected";

unsigned long lastReconnectAttempt = 0;
unsigned long lastButtonTime = 0;
wl_status_t lastPrintedStatus = (wl_status_t)(-1);

void netLog(String msg) {
  Serial.print("[");
  Serial.print(millis());
  Serial.print(" ms][系統] ");
  Serial.println(msg);
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

void setStatusLEDs(bool r, bool y, bool b) {
  digitalWrite(STATUS_RED_LED, r ? HIGH : LOW);
  digitalWrite(STATUS_YELLOW_LED, y ? HIGH : LOW);
  digitalWrite(STATUS_BLUE_LED, b ? HIGH : LOW);
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

void WiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    wifiEverConnected = true;
    lastWifiFailReason = WIFI_REASON_UNSPECIFIED;
    wifiReasonText = "Connected";
    netLog("已連上基地台，SSID：" + String(current_ssid));
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    wifiReasonText = "Connected";
    netLog("已取得IP：" + WiFi.localIP().toString());
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiFailReason = (wifi_err_reason_t)info.wifi_sta_disconnected.reason;
    wifiReasonText = reasonToText(lastWifiFailReason);
    mqttStatusText = "Disconnected";
    netLog("Wi-Fi斷線，原因碼=" + String((int)lastWifiFailReason) + "，原因：" + wifiReasonText);
  } else if (event == ARDUINO_EVENT_WIFI_AP_START) {
    netLog("AP模式已啟動");
  } else if (event == ARDUINO_EVENT_WIFI_AP_STOP) {
    netLog("AP模式已關閉");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  msg.trim();

  if (msg.length() == 0) return;

  netLog("收到MQTT訊息: " + msg);

  if (currentStatus != msg) {
    netLog("狀態更新：" + currentStatus + " -> " + msg);
    currentStatus = msg;

    struct tm ti;
    if (getLocalTime(&ti)) {
      char buf[10];
      strftime(buf, 10, "%H:%M:%S", &ti);
      lastStatusChangeTime = String(buf);
    } else {
      lastStatusChangeTime = "時間未知";
    }
  }
}

void tryReconnectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;
  if (millis() - lastReconnectAttempt < 3000) return;

  lastReconnectAttempt = millis();
  mqttStatusText = "Connecting";

  String clientId = "ESP32Teams-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  netLog("嘗試連線 MQTT...");

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

void handleRoot() {
  String statusColor = "#007bff";

  if (currentStatus == "Available") statusColor = "#28a745";
  else if (currentStatus == "Busy" || currentStatus == "InAMeeting") statusColor = "#d73a49";
  else if (currentStatus == "Away") statusColor = "#e2b100";
  else if (currentStatus == "DoNotDisturb") statusColor = "#7e57c2";

  String html = "<!DOCTYPE html><html lang='zh-TW'><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'><style>";
  html += "body{font-family:'Segoe UI',sans-serif;background:#f0f2f5;display:flex;justify-content:center;align-items:center;flex-direction:column;min-height:100vh;margin:0;}";
  html += ".card{background:white;border-radius:20px;padding:30px;box-shadow:0 10px 20px rgba(0,0,0,0.1);text-align:center;width:340px;margin-bottom:20px;}";
  html += ".status-val{font-size:2.2em;font-weight:bold;color:" + statusColor + ";margin:20px 0;}";
  html += ".time-info{text-align:left;color:#555;font-size:0.95em;border-top:1px solid #eee;padding-top:15px;line-height:2.0;}";
  html += ".highlight{color:#000;font-weight:600;float:right;}";
  html += "input[type=text],input[type=password],select{width:100%;padding:10px;margin:10px 0;border:1px solid #ddd;border-radius:8px;box-sizing:border-box;}";
  html += "input[type=submit],.btn-scan{width:100%;background:#007bff;color:white;border:none;padding:12px;border-radius:8px;cursor:pointer;font-weight:bold;font-size:1em;}";
  html += ".btn-scan{background:#6c757d;margin-bottom:10px;text-decoration:none;display:block;}</style></head><body>";
  html += "<div class='card'><h2>Teams 狀態監控</h2><div class='status-val'>" + currentStatus + "</div>";
  html += "<div class='time-info'><div>🕒 現在時間：<span id='clock' class='highlight'>--:--:--</span></div>";
  html += "<div>🔴 狀態更新：<span class='highlight'>" + lastStatusChangeTime + "</span></div>";
  html += "<div>🌐 當前 Wi-Fi：<span class='highlight'>" + (WiFi.status() == WL_CONNECTED ? String(WiFi.SSID()) : "未連接") + "</span></div>";
  html += "<div>📡 Wi-Fi 狀態：<span class='highlight'>" + wifiReasonText + "</span></div>";
  html += "<div>☁️ MQTT 狀態：<span class='highlight'>" + mqttStatusText + "</span></div>";
  html += "</div></div>";

  html += "<div class='card'><h3>Wi-Fi 設定</h3><form action='/scan' method='POST'><input type='submit' class='btn-scan' value='🔍 掃描周邊網路'></form>";
  html += "<form action='/save' method='POST'>";
  if (scannedWifiHtml.length() > 0) {
    html += "選擇網路: <select name='s_select'>" + scannedWifiHtml + "</select>或手動輸入:";
  } else {
    html += "SSID:";
  }
  html += "<input type='text' name='s' placeholder='輸入 Wi-Fi 名稱'><input type='password' name='p' placeholder='密碼'><input type='submit' value='更新並重新連線'></form></div>";
  html += "<script>function updateClock(){const d=new Date();document.getElementById('clock').innerHTML=d.toTimeString().split(' ')[0];}setInterval(updateClock,1000);updateClock();</script></body></html>";

  server.send(200, "text/html", html);
}

void handleScan() {
  netLog("開始掃描周邊Wi-Fi...");
  int n = WiFi.scanNetworks();

  scannedWifiHtml = "<option value=''>-- 請選擇 --</option>";
  for (int i = 0; i < n; ++i) {
    scannedWifiHtml += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleSave() {
  String new_ssid = (server.hasArg("s") && server.arg("s").length() > 0) ? server.arg("s") : server.arg("s_select");
  if (new_ssid.length() > 0 && server.hasArg("p")) {
    current_ssid = new_ssid;
    current_password = server.arg("p");

    server.send(200, "text/plain", "Wi-Fi 設定已更新，裝置正在重新連線 Wi-Fi，請稍候重新開啟頁面。");
    delay(1000);

    WiFi.disconnect(true);
    WiFi.begin(current_ssid.c_str(), current_password.c_str());
    wifiLostTime = millis();
    mqttStatusText = "Disconnected";
  } else {
    server.send(400, "text/plain", "SSID或密碼缺失");
  }
}

void updateLCDStatus() {
  String localStatus = currentStatus;
  if (localStatus.length() > 11) localStatus = localStatus.substring(0, 11);

  lcd.setCursor(0, 1);
  lcd.print("Sts: ");
  lcd.print(localStatus);
  lcd.print("           ");
}

void displayFirstLine() {
  unsigned long now = millis();
  lcd.setCursor(0, 0);

  if (displayMode == 1) {
    if (isAPMode) {
      if (now - modeStartTime < 5000) {
        lcd.print("AP:");
        lcd.print(ap_ssid);
        lcd.print("    ");
      } else if (now - modeStartTime < 12000) {
        lcd.print("IP:192.168.4.1  ");
      } else {
        displayMode = 0;
        lcd.clear();
        updateLCDStatus();
      }
    } else {
      if (now - modeStartTime < 3000) {
        lcd.print("SSID: ");
        lcd.print(WiFi.SSID());
        lcd.print("        ");
      } else {
        displayMode = 2;
        modeStartTime = now;
        lcd.clear();
        updateLCDStatus();
      }
    }
  } else if (displayMode == 2) {
    if (now - modeStartTime < 5000) {
      lcd.print("IP:");
      lcd.print(WiFi.localIP().toString());
      lcd.print("    ");
    } else {
      displayMode = 0;
      lcd.clear();
      updateLCDStatus();
    }
  } else {
    struct tm ti;
    if (getLocalTime(&ti)) {
      char tStr[17];
      strftime(tStr, 17, "Time: %H:%M:%S", &ti);
      lcd.print(tStr);
    } else {
      if (lastWifiFailReason == WIFI_REASON_NO_AP_FOUND) lcd.print("No WiFi Found   ");
      else if (lastWifiFailReason == WIFI_REASON_AUTH_FAIL) lcd.print("Wrong Password  ");
      else lcd.print(WiFi.status() == WL_CONNECTED ? "Syncing...       " : "WiFi Connecting ");
    }
  }
}

void updateLights() {
  if (WiFi.status() != WL_CONNECTED) {
    fill_solid(leds, NUM_LEDS, CRGB::Red);
    FastLED.setBrightness(beatsin8(60, 20, 200));
  } else {
    String localStatus = currentStatus;

    if (localStatus == "Syncing...") {
      fill_solid(leds, NUM_LEDS, CRGB::Yellow);
      FastLED.setBrightness(beatsin8(30, 20, 150));
    } else if (localStatus == "Available") {
      fill_solid(leds, NUM_LEDS, CRGB::Green);
      FastLED.setBrightness(beatsin8(40, 20, 150));
    } else if (localStatus == "Busy" || localStatus == "InAMeeting") {
      fill_solid(leds, NUM_LEDS, CRGB::Red);
      FastLED.setBrightness(150);
    } else if (localStatus == "DoNotDisturb") {
      fadeToBlackBy(leds, NUM_LEDS, 20);
      int pos = beatsin16(30, 0, NUM_LEDS - 1);
      leds[pos] = CRGB::Purple;
      FastLED.setBrightness(150);
    } else if (localStatus == "Away") {
      fill_solid(leds, NUM_LEDS, CRGB::Orange);
      FastLED.setBrightness(random8(60, 150));
    } else {
      fill_solid(leds, NUM_LEDS, CRGB::Blue);
      FastLED.setBrightness(30);
    }
  }
  FastLED.show();
}

void handleButton() {
  if (digitalRead(BUTTON_PIN) == LOW && displayMode == 0) {
    if (millis() - lastButtonTime > 250) {
      lastButtonTime = millis();
      netLog("按鈕被按下，切換LCD顯示模式");
      displayMode = 1;
      modeStartTime = millis();
      lcd.clear();
      updateLCDStatus();
    }
  }
}

void handleWiFiState() {
  wl_status_t s = WiFi.status();

  if (s != lastPrintedStatus) {
    netLog("Wi-Fi狀態改變：" + wlStatusToText(lastPrintedStatus) + " -> " + wlStatusToText(s));
    lastPrintedStatus = s;
  }

  bool red = false, yellow = false, blue = isAPMode;

  if (s == WL_CONNECTED) {
    wifiLostTime = 0;

    if (isAPMode) {
      netLog("偵測到已重新連上Wi-Fi，準備關閉AP模式");
      WiFi.softAPdisconnect(true);
      isAPMode = false;
      WiFi.mode(WIFI_STA);
      netLog("AP模式已關閉，切回STA模式");
    }

    red = false;
    yellow = false;
    blue = false;
  } else {
    mqttStatusText = "Disconnected";

    if (lastWifiFailReason == WIFI_REASON_NO_AP_FOUND) {
      yellow = true;
      red = false;
    } else if (lastWifiFailReason == WIFI_REASON_AUTH_FAIL
#ifdef WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
               || lastWifiFailReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
#endif
               || lastWifiFailReason == WIFI_REASON_HANDSHAKE_TIMEOUT
               || lastWifiFailReason == WIFI_REASON_AUTH_EXPIRE) {
      red = true;
      yellow = false;
    } else {
      yellow = true;
      red = false;
    }

    if (wifiLostTime == 0) {
      wifiLostTime = millis();
      netLog("開始計時：Wi-Fi斷線持續時間");
    }

    if (millis() - wifiLostTime > 30000 && !isAPMode) {
      netLog("超過30秒未連上Wi-Fi，啟動AP模式");
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(ap_ssid, ap_password);
      isAPMode = true;
      netLog("AP名稱：" + String(ap_ssid) + "，AP IP：192.168.4.1");
    }
  }

  setStatusLEDs(red, yellow, isAPMode);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  netLog("系統開機");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(STATUS_RED_LED, OUTPUT);
  pinMode(STATUS_YELLOW_LED, OUTPUT);
  pinMode(STATUS_BLUE_LED, OUTPUT);

  Serial.println("燈號測試");
  Serial.println("紅色 ON (GPIO 15)");
  digitalWrite(STATUS_RED_LED, HIGH);
  delay(500);
  digitalWrite(STATUS_RED_LED, LOW);

  // 測試黃色 (GPIO 2)
  Serial.println("黃色 ON (GPIO 2)");
  digitalWrite(STATUS_YELLOW_LED, HIGH);
  delay(500);
  digitalWrite(STATUS_YELLOW_LED, LOW);

  // 測試藍色 (GPIO 4)
  Serial.println("藍色 ON (GPIO 4)");
  digitalWrite(STATUS_BLUE_LED, HIGH);
  delay(500);
  digitalWrite(STATUS_BLUE_LED, LOW);
  
  lcd.init();
  lcd.backlight();
  lcd.clear();

  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);

  WiFi.onEvent(WiFiEvent);
  WiFi.mode(WIFI_STA);
  WiFi.begin(current_ssid.c_str(), current_password.c_str());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  server.on("/", handleRoot);
  server.on("/scan", handleScan);
  server.on("/save", handleSave);
  server.begin();

  displayFirstLine();
  updateLCDStatus();

  netLog("單核 MQTT 版本啟動完成");
}

void loop() {
  server.handleClient();
  handleButton();
  handleWiFiState();

  if (WiFi.status() == WL_CONNECTED) {
    tryReconnectMQTT();
    mqttClient.loop();
  }

  displayFirstLine();
  updateLights();

  static String lastStatus = "";
  if (currentStatus != lastStatus) {
    netLog("LCD偵測到狀態改變：" + lastStatus + " -> " + currentStatus);
    updateLCDStatus();
    lastStatus = currentStatus;
  }

  delay(10);
}