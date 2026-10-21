#include "globals.h"
#include "wifi_config.h"
#include "config.h"
#include "web_handlers.h"
#include "web_handlers.h"
#include "modem.h"
#include "web_handlers.h"
#include "push.h"
#include "web_handlers.h"
#include "sms_process.h"
#include "web_handlers.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  configValid = isConfigValid();

  // ---- WiFi 连接（优化双频合一与多 AP 环境连接） ----
  WiFi.disconnect(true, true);             // 清除历史 WiFi 连接与 NVS 缓存
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                    // 关闭 Modem Sleep，提高连接响应速度
  WiFi.setAutoReconnect(true);             // 断线后自动重连
  WiFi.setTxPower(WIFI_POWER_17dBm);       // 稳定射频功耗，防止供电波动

  // 扫描并锁定信号最佳的 2.4G 物理 BSSID 与信道，避免双频合一或虚拟 AP 协商失败
  int n = WiFi.scanNetworks(false, false, false, 300);
  int bestRssi = -1000;
  uint8_t bestBssid[6];
  int bestChannel = 0;
  bool found = false;
  for (int i = 0; i < n; ++i) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      if (WiFi.RSSI(i) > bestRssi) {
        bestRssi = WiFi.RSSI(i);
        bestChannel = WiFi.channel(i);
        memcpy(bestBssid, WiFi.BSSID(i), 6);
        found = true;
      }
    }
  }
  WiFi.scanDelete();
  delay(50);

  if (found) {
    logCaptureLn(String("连接wifi: ") + String(WIFI_SSID) + " (已锁定 2.4G 信道: " + String(bestChannel) + ")");
    WiFi.begin(WIFI_SSID, WIFI_PASS, bestChannel, bestBssid);
  } else {
    logCaptureLn(String("连接wifi: ") + String(WIFI_SSID));
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  // 带超时的等待连接，失败则重启重试
  unsigned long wifiStart = millis();
  const unsigned long WIFI_TIMEOUT = 20000; // 20秒超时
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT) {
    blink_short(200);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logCaptureLn(String("wifi已连接"));
    logCapture(String("IP地址: "));
    logCaptureLn(WiFi.localIP().toString());
    logCapture(String("信号强度(RSSI): "));
    logCaptureLn(String(WiFi.RSSI()) + " dBm");
  } else {
    logCaptureLn(String("⚠️ WiFi连接超时，即将重启重试..."));
    delay(1000);
    ESP.restart();
  }

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleRoot);
  server.on("/sms", handleRoot);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/query", handleQuery);
  server.on("/flight", handleFlightMode);
  server.on("/at", handleATCommand);
  server.on("/log", handleLog);
  server.on("/modem", handleModem);
  server.on("/wifi", handleWifi);
  server.on("/system", handleSystem);
  server.begin();
  logCaptureLn(String("HTTP服务器已启动"));

  // ---- NTP 时间同步 ----
  logCaptureLn(String("正在同步NTP时间..."));
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) {
    delay(1);
    server.handleClient();
    ntpRetry++;
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    logCaptureLn(String("NTP时间同步成功"));
    time_t now = time(nullptr);
    logCapture(String("当前UTC时间戳: "));
    logCaptureLn(String(now));
  } else {
    logCaptureLn(String("NTP时间同步失败，将使用设备时间"));
  }

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);

  // ---- 启动通知（网页已可用，发邮件不会影响用户访问） ----
  if (configValid) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }

  // ---- 模组初始化（较慢，但网页已可访问） ----
  modemInit();
}

void loop() {
  server.handleClient();
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      logCaptureLn(String("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数"));
    }
  }

  // ---- 4G 网络注册后台守护与自愈机制 ----
  static unsigned long lastModemCheck = 0;
  static int retryCounter = 0;
  if (!modemReady && millis() - lastModemCheck > 15000) {
    lastModemCheck = millis();
    retryCounter++;
    int stat = getCEREGStatus();
    logCaptureLn(String("[网络守护] 第 ") + String(retryCounter) + " 次自愈检查: CEREG=" + String(stat));

    if (stat == 1 || stat == 5) {
      modemReady = true;
      logCaptureLn(String("🎉 4G网络已在后台成功自动就绪！(状态: ") + String(stat == 1 ? "本地" : "漫游") + ")");
    } else {
      if (config.operatorPlmn.length() > 0) {
        logCaptureLn(String("[网络守护] 重新下发首选运营商: ") + config.operatorPlmn);
        applyOperatorConfig();
      } else {
        // 未指定运营商且处于受限(11)或无服务(0)，每3次尝试一次射频重新激活
        if (retryCounter % 3 == 0) {
          logCaptureLn(String("[网络守护] 触发射频重新激活 (CFUN 0/1)..."));
          sendATandWaitOK("AT+CFUN=0", 2000);
          delay(1000);
          sendATandWaitOK("AT+CFUN=1", 2000);
        }
      }
    }
  }

  checkConcatTimeout();
  if (Serial.available()) Serial1.write(Serial.read());
  checkSerial1URC();
}
