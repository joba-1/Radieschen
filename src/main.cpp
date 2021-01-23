#include <Arduino.h>

#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Syslog.h>

#define LED_ON  LOW
#define LED_OFF HIGH


WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP);

WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);


void setup() {
  WiFi.mode(WIFI_STA);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_ON);

  Serial.begin(SERIAL_SPEED);
  Serial.println("\nStarting " PROGNAME " v" VERSION " " __DATE__ " " __TIME__);

  // Syslog setup
  syslog.server(SYSLOG_SERVER, SYSLOG_PORT);
  syslog.deviceHostname(HOSTNAME);
  syslog.appName("Joba1");
  syslog.defaultPriority(LOG_KERN);

  digitalWrite(LED_BUILTIN, LED_OFF);

  WiFiManager wm;
  //wm.resetSettings();
  if( ! wm.autoConnect() ) {
    Serial.println("Failed to connect WLAN");
    delay(1000);
    ESP.restart();
    while(true);
  }

  digitalWrite(LED_BUILTIN, LED_ON);
  char msg[80];
  snprintf(msg, sizeof(msg), "WLAN IP is %s", WiFi.localIP().toString().c_str());
  Serial.printf(msg);
  syslog.logf(LOG_NOTICE, msg);

  ntp.begin();
}

void loop() {
  ntp.update();

  Serial.println(ntp.getFormattedTime());

  delay(1000);
}
