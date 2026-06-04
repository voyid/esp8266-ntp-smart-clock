/*
 * ESP8266 NTP时钟 + OLED + 2个闹钟 + Web配置
 * 硬件：ESP-12F, 0.96寸OLED(I2C), 有源蜂鸣器, ADC锂电池
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <EEPROM.h>

// ==================== 硬件引脚 ====================
#define BUZZER_PIN        D5
#define FACTORY_RESET_PIN D6
#define BATTERY_ADC_PIN   A0

// ==================== OLED ====================
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ==================== Web服务器 ====================
ESP8266WebServer server(80);

// ==================== EEPROM布局 ====================
// [0-63]   WiFi SSID
// [64-127] WiFi密码
// [128]    闹钟1 小时
// [129]    闹钟1 分钟
// [130]    闹钟2 小时
// [131]    闹钟2 分钟
// [132]    Magic标记 0xAA
#define MAGIC_ADDR 132
#define MAGIC_VAL  0xAA

// ==================== AP配置 ====================
const char* AP_SSID = "esp8266";
const char* AP_PASS = "caiyang123456";

// ==================== 用户数据 ====================
char storedSSID[64] = "";
char storedPASS[64] = "";
int alarm1_H = -1, alarm1_M = -1;
int alarm2_H = -1, alarm2_M = -1;

// ==================== 状态 ====================
bool isFirstBoot = false;
bool wifiConnected = false;
int currentPage = 0;  // 0=欢迎/等待  1=连接失败  2=主时钟
unsigned long failPageStart = 0;
int lastAlarmMinute = -1;

// ==================== 按键 ====================
unsigned long btnPressStart = 0;
bool btnHeld = false;

// ==================== 蜂鸣器(非阻塞) ====================
bool bzActive = false;
int bzRemaining = 0;
unsigned long bzNext = 0;
bool bzState = false;
unsigned long bzOnMs, bzOffMs;

// ==================== 版本 ====================
#define FW_VER "v1.0.0"
#define FW_DEV "CaiYang Team"

// ================================================================
//                        EEPROM
// ================================================================
void saveAlarms() {
  EEPROM.begin(256);
  EEPROM.write(128, (byte)alarm1_H);
  EEPROM.write(129, (byte)alarm1_M);
  EEPROM.write(130, (byte)alarm2_H);
  EEPROM.write(131, (byte)alarm2_M);
  EEPROM.commit();
}

void loadAlarms() {
  EEPROM.begin(256);
  alarm1_H = (int)EEPROM.read(128);
  alarm1_M = (int)EEPROM.read(129);
  alarm2_H = (int)EEPROM.read(130);
  alarm2_M = (int)EEPROM.read(131);
  if (alarm1_H > 23) alarm1_H = -1;
  if (alarm1_M > 59) alarm1_M = -1;
  if (alarm2_H > 23) alarm2_H = -1;
  if (alarm2_M > 59) alarm2_M = -1;
}

void saveWiFi(const char* ssid, const char* pass) {
  EEPROM.begin(256);
  for (int i = 0; i < 64; i++) EEPROM.write(i, ssid[i]);
  for (int i = 0; i < 64; i++) EEPROM.write(64 + i, pass[i]);
  EEPROM.write(MAGIC_ADDR, MAGIC_VAL);
  EEPROM.commit();
  strcpy(storedSSID, ssid);
  strcpy(storedPASS, pass);
}

void loadWiFi() {
  EEPROM.begin(256);
  for (int i = 0; i < 64; i++) storedSSID[i] = (char)EEPROM.read(i);
  for (int i = 0; i < 64; i++) storedPASS[i] = (char)EEPROM.read(64 + i);
  storedSSID[63] = '\0';
  storedPASS[63] = '\0';
}

bool checkFirstBoot() {
  EEPROM.begin(256);
  return EEPROM.read(MAGIC_ADDR) != MAGIC_VAL;
}

void factoryReset() {
  EEPROM.begin(256);
  for (int i = 0; i < 256; i++) EEPROM.write(i, 0xFF);
  EEPROM.commit();
}

// ================================================================
//                       电池电量
// ================================================================
int readBattery() {
  int raw = analogRead(BATTERY_ADC_PIN);
  // 分压比2:1，根据实际电路修改
  float v = (float)raw / 1023.0 * 3.3 * 2.0;
  int pct = (int)((v - 3.7) / 0.5 * 100.0);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// ================================================================
//                    格式化闹钟字符串
// ================================================================
String fmtAlarm(int h, int m) {
  if (h < 0 || h > 23 || m < 0 || m > 59) return "NULL";
  char buf[6];
  sprintf(buf, "%02d:%02d", h, m);
  return String(buf);
}

// ================================================================
//                    非阻塞蜂鸣器
// ================================================================
void startBuzzer(int n, unsigned long onMs, unsigned long offMs) {
  bzActive = true;
  bzRemaining = n;
  bzOnMs = onMs;
  bzOffMs = offMs;
  bzState = true;
  digitalWrite(BUZZER_PIN, HIGH);
  bzNext = millis() + onMs;
}

void updateBuzzer() {
  if (!bzActive) return;
  if (millis() < bzNext) return;
  bzState = !bzState;
  digitalWrite(BUZZER_PIN, bzState ? HIGH : LOW);
  if (bzState) {
    bzNext = millis() + bzOnMs;
  } else {
    bzRemaining--;
    if (bzRemaining <= 0) {
      bzActive = false;
      digitalWrite(BUZZER_PIN, LOW);
    } else {
      bzNext = millis() + bzOffMs;
    }
  }
}

// ================================================================
//                    恢复出厂按键检测
// ================================================================
void checkResetBtn() {
  if (digitalRead(FACTORY_RESET_PIN) == LOW) {
    if (btnPressStart == 0) {
      btnPressStart = millis();
    } else if (millis() - btnPressStart > 5000 && !btnHeld) {
      btnHeld = true;
      factoryReset();
      display.clearDisplay();
      display.setTextSize(1);
      display.setCursor(20, 28);
      display.print(F("Factory Reset!"));
      display.display();
      delay(2000);
      ESP.restart();
    }
  } else {
    btnPressStart = 0;
    btnHeld = false;
  }
}

// ================================================================
//                        OLED页面
// ================================================================

// 欢迎页
void showWelcome() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(32, 0);
  display.print(F("= Welcome ="));
  display.setCursor(0, 16);
  display.print(F("AP:"));
  display.print(AP_SSID);
  display.setCursor(0, 28);
  display.print(F("PW:"));
  display.print(AP_PASS);
  display.setCursor(0, 42);
  display.print(F("IP:"));
  display.print(wifiConnected ? WiFi.localIP().toString() : "192.168.4.1");
  display.display();
}

// 连接失败页
void showFail(const char* ssid) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 4);
  display.print(F("WiFi Connect Failed"));
  display.setCursor(0, 20);
  display.print(F("SSID:"));
  display.print(ssid);
  display.setCursor(0, 36);
  display.print(F("AP IP: 192.168.4.1"));
  display.setCursor(0, 50);
  display.print(F("Please connect AP"));
  display.display();
}

// ================================================================
//                   Web页面处理器(简洁中文)
// ================================================================

// WiFi配置
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;max-width:500px;margin:20px auto;padding:0 10px}";
  html += "h2{color:#1a73e8;border-bottom:1px solid #ddd;padding-bottom:8px}";
  html += "label{display:block;margin:10px 0 4px;font-weight:bold}";
  html += "input[type=text],input[type=password]{width:100%;padding:8px;box-sizing:border-box;border:1px solid #ccc;border-radius:4px}";
  html += "input[type=submit]{background:#1a73e8;color:#fff;border:none;padding:10px 20px;border-radius:4px;margin-top:12px;cursor:pointer;font-size:15px}";
  html += ".nav a{display:inline-block;margin-right:12px;color:#1a73e8;text-decoration:none}</style></head><body>";
  html += "<div class='nav'><a href='/'>WiFi配置</a> <a href='/alarm'>闹钟设置</a> <a href='/info'>设备信息</a></div>";
  html += "<h2>WiFi配置</h2>";
  html += "<form method='POST' action='/save'>";
  html += "<label>WiFi名称</label><input type='text' name='ssid' value='";
  html += storedSSID;
  html += "'>";
  html += "<label>WiFi密码</label><input type='password' name='pass'>";
  html += "<br><input type='submit' value='保存并连接'>";
  html += "</form>";
  html += "<br><p style='color:#999;font-size:13px'>连接成功后设备将关闭AP模式，使用WiFi网络中的IP地址访问</p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// 闹钟设置
void handleAlarm() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;max-width:500px;margin:20px auto;padding:0 10px}";
  html += "h2{color:#1a73e8;border-bottom:1px solid #ddd;padding-bottom:8px}";
  html += "label{display:block;margin:10px 0 4px;font-weight:bold}";
  html += "input[type=number]{width:80px;padding:8px;border:1px solid #ccc;border-radius:4px;font-size:16px;text-align:center}";
  html += "input[type=submit]{background:#1a73e8;color:#fff;border:none;padding:10px 20px;border-radius:4px;margin-top:12px;cursor:pointer;font-size:15px}";
  html += ".nav a{display:inline-block;margin-right:12px;color:#1a73e8;text-decoration:none}";
  html += ".row{margin:12px 0}</style></head><body>";
  html += "<div class='nav'><a href='/'>WiFi配置</a> <a href='/alarm'>闹钟设置</a> <a href='/info'>设备信息</a></div>";
  html += "<h2>闹钟设置</h2>";
  html += "<form method='GET' action='/setAlarms'>";
  html += "<div class='row'><label>闹钟1</label>";
  html += "<input type='number' name='h1' min='0' max='23' value='";
  html += (alarm1_H >= 0 ? String(alarm1_H) : "");
  html += "' placeholder='时'> : ";
  html += "<input type='number' name='m1' min='0' max='59' value='";
  html += (alarm1_M >= 0 ? String(alarm1_M) : "");
  html += "' placeholder='分'></div>";
  html += "<div class='row'><label>闹钟2</label>";
  html += "<input type='number' name='h2' min='0' max='23' value='";
  html += (alarm2_H >= 0 ? String(alarm2_H) : "");
  html += "' placeholder='时'> : ";
  html += "<input type='number' name='m2' min='0' max='59' value='";
  html += (alarm2_M >= 0 ? String(alarm2_M) : "");
  html += "' placeholder='分'></div>";
  html += "<br><input type='submit' value='保存闹钟'>";
  html += "</form></body></html>";
  server.send(200, "text/html", html);
}

// 设备信息
void handleInfo() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<style>body{font-family:sans-serif;max-width:500px;margin:20px auto;padding:0 10px}";
  html += "h2{color:#1a73e8;border-bottom:1px solid #ddd;padding-bottom:8px}";
  html += ".nav a{display:inline-block;margin-right:12px;color:#1a73e8;text-decoration:none}";
  html += "td{padding:6px 8px;border-bottom:1px solid #eee}";
  html += "td:first-child{font-weight:bold;color:#555;width:100px}</style></head><body>";
  html += "<div class='nav'><a href='/'>WiFi配置</a> <a href='/alarm'>闹钟设置</a> <a href='/info'>设备信息</a></div>";
  html += "<h2>设备信息</h2><table>";
  html += "<tr><td>固件版本</td><td>" FW_VER "</td></tr>";
  html += "<tr><td>开发团队</td><td>" FW_DEV "</td></tr>";
  html += "<tr><td>芯片ID</td><td>" + String(ESP.getChipId(), HEX) + "</td></tr>";
  html += "<tr><td>Flash</td><td>" + String((int)(ESP.getFlashChipRealSize()/1024)) + " KB</td></tr>";
  html += "<tr><td>可用内存</td><td>" + String(ESP.getFreeHeap()) + " B</td></tr>";
  if (wifiConnected) {
    html += "<tr><td>设备IP</td><td>" + WiFi.localIP().toString() + "</td></tr>";
  }
  html += "<tr><td>运行时间</td><td>" + String(millis()/1000) + " 秒</td></tr>";
  html += "</table></body></html>";
  server.send(200, "text/html", html);
}

// 保存WiFi
void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  saveWiFi(ssid.c_str(), pass.c_str());
  server.send(200, "text/html",
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2 style='color:#1a73e8'>保存成功！正在重启...</h2>"
    "<p>请等待设备重新启动</p></body>");
  delay(1000);
  ESP.restart();
}

// 保存闹钟
void handleSetAlarms() {
  int h1 = server.arg("h1").toInt();
  int m1 = server.arg("m1").toInt();
  int h2 = server.arg("h2").toInt();
  int m2 = server.arg("m2").toInt();

  alarm1_H = (h1 >= 0 && h1 <= 23) ? h1 : -1;
  alarm1_M = (m1 >= 0 && m1 <= 59 && alarm1_H >= 0) ? m1 : -1;
  alarm2_H = (h2 >= 0 && h2 <= 23) ? h2 : -1;
  alarm2_M = (m2 >= 0 && m2 <= 59 && alarm2_H >= 0) ? m2 : -1;

  saveAlarms();
  server.send(200, "text/html",
    "<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2 style='color:#1a73e8'>闹钟已保存！</h2>"
    "<p><a href='/alarm'>返回</a></p></body>");
}

// ================================================================
//                          SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

  // OLED初始化
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // 加载EEPROM
  loadWiFi();
  loadAlarms();
  isFirstBoot = checkFirstBoot();

  // 启动AP
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  // 情况1: 首次开机/无WiFi配置 → 显示欢迎页，等待用户配置
  if (isFirstBoot || strlen(storedSSID) == 0) {
    currentPage = 0;
    showWelcome();
    Serial.println(F("[BOOT] No WiFi. Waiting for config via AP 192.168.4.1"));
  }
  // 情况2: 有存储WiFi → 尝试连接
  else {
    showWelcome();  // 连接期间显示欢迎页
    Serial.println("[BOOT] Connecting to: " + String(storedSSID));
    WiFi.begin(storedSSID, storedPASS);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 60000) {
      server.handleClient();
      delay(500);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      currentPage = 2;
      // 关闭AP模式
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      Serial.println("[BOOT] Connected! IP: " + WiFi.localIP().toString());
      // 重新显示欢迎页(带STA IP)
      showWelcome();
      delay(10000);  // 显示3秒让用户看到IP
    } else {
      currentPage = 1;
      showFail(storedSSID);
      Serial.println(F("[BOOT] WiFi timeout"));
    }
  }

  // 注册Web路由
  server.on("/", handleRoot);
  server.on("/alarm", handleAlarm);
  server.on("/info", handleInfo);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/setAlarms", handleSetAlarms);
  server.begin();
  Serial.println(F("[BOOT] Web server started"));

  // 配置SNTP (ESP8266内置，自动同步)
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
  Serial.println(F("[BOOT] SNTP configured"));
}

// ================================================================
//                          LOOP
// ================================================================
void loop() {
  server.handleClient();
  updateBuzzer();

  // === page=0: 欢迎/等待配置 ===
  if (currentPage == 0) {
    static unsigned long lastRefresh = 0;
    if (millis() - lastRefresh > 5000) {
      showWelcome();
      lastRefresh = millis();
    }
    checkResetBtn();
    delay(100);
    return;
  }

  // === page=1: 连接失败(10秒后进入主时钟) ===
  if (currentPage == 1) {
    if (failPageStart == 0) failPageStart = millis();
    if (millis() - failPageStart > 10000) {
      currentPage = 2;
    } else {
      checkResetBtn();
      delay(100);
      return;
    }
  }

  // === page=2: 主时钟 ===
  time_t now = time(nullptr);
  struct tm* t = localtime(&now);
  int h = t->tm_hour;
  int m = t->tm_min;
  int s = t->tm_sec;
  int bat = readBattery();

  // NTP同步检测: 年份>2010视为已同步
  bool timeValid = (t->tm_year + 1900 > 2010);

  display.clearDisplay();

  // 第一行: 日期 + WiFi符号 + 电量
  display.setTextSize(1);

  // 日期 (左上角)
  if (timeValid) {
    display.setCursor(0, 0);
    display.printf("%04d-%02d-%02d", t->tm_year+1900, t->tm_mon+1, t->tm_mday);
  } else {
    display.setCursor(0, 0);
    display.print(F("NTP Sync..."));
  }

  // WiFi状态 (日期右边): "WiFi" + 对号/叉号
  display.setCursor(68, 0);
  display.print(F("WiFi"));
  if (wifiConnected) {
    display.drawLine(94, 4, 96, 7, WHITE);     // 对号
    display.drawLine(96, 7, 100, 1, WHITE);
  } else {
    display.drawLine(94, 1, 100, 7, WHITE);    // 叉号
    display.drawLine(100, 1, 94, 7, WHITE);
  }

  // 电量百分比 (最右边, 右对齐到x=127)
  char batStr[5];
  sprintf(batStr, "%d%%", bat);
  display.setCursor(127 - strlen(batStr) * 6, 0);
  display.print(batStr);

  // 中间: 大字体时分秒
  display.setTextSize(2);
  display.setCursor(12, 23);
  display.printf("%02d:%02d:%02d", h, m, s);

  // 底部: 两个闹钟 (带闹钟符号)
  // 8x8闹钟位图: 顶铃 + 圆形表盘 + 底脚
  static const uint8_t alarmIcon[] PROGMEM = {
    0x00,0x18,0x24,0x42,0x42,0x24,0x18,0x00
  };
  display.setTextSize(1);
  // 闹钟1
  display.drawBitmap(0, 48, alarmIcon, 8, 8, WHITE);
  display.setCursor(10, 48);
  display.print("1:" + fmtAlarm(alarm1_H, alarm1_M));
  // 闹钟2
  display.drawBitmap(64, 48, alarmIcon, 8, 8, WHITE);
  display.setCursor(74, 48);
  display.print("2:" + fmtAlarm(alarm2_H, alarm2_M));

  display.display();

  // === 闹钟触发 ===
  bool match = false;
  if (alarm1_H >= 0 && h == alarm1_H && m == alarm1_M) match = true;
  if (alarm2_H >= 0 && h == alarm2_H && m == alarm2_M) match = true;

  if (match && m != lastAlarmMinute && !bzActive) {
    startBuzzer(5, 400, 400);
    lastAlarmMinute = m;
  }
  // 重置闹钟触发标记
  bool anyMatch = false;
  if (alarm1_H >= 0 && m == alarm1_M) anyMatch = true;
  if (alarm2_H >= 0 && m == alarm2_M) anyMatch = true;
  if (!anyMatch) lastAlarmMinute = -1;

  checkResetBtn();
  delay(100);
}
