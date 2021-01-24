#include <Arduino.h>

#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Syslog.h>

#define LED_ON  LOW
#define LED_OFF HIGH

#define IRQ_PIN D2


WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP);

WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);


uint32_t last_counter_reset = 0;       // millis() of last counter reset
volatile uint32_t counter_events = 0;  // events of current interval so far

ICACHE_RAM_ATTR void event()
{
  counter_events++;
}


void setup() {
  WiFi.mode(WIFI_STA);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LED_ON);
  analogWriteRange(PWMRANGE);

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
    for( int i = 0; i < 1000; i+= 200 ) {
      digitalWrite(LED_BUILTIN, LED_ON);
      delay(100);
      digitalWrite(LED_BUILTIN, LED_OFF);
      delay(100);
    }
    ESP.restart();
    while(true);
  }

  digitalWrite(LED_BUILTIN, LED_ON);
  char msg[80];
  snprintf(msg, sizeof(msg), "WLAN IP is %s", WiFi.localIP().toString().c_str());
  Serial.printf(msg);
  syslog.logf(LOG_NOTICE, msg);

  ntp.begin();

  last_counter_reset = millis();
  attachInterrupt(digitalPinToInterrupt(D2), event, FALLING);
}


bool check_ntptime() {
  static bool have_time = false;
  if( !have_time && ntp.getEpochTime() ) {
    have_time = true;
    syslog.logf(LOG_NOTICE, "%s UTC", ntp.getFormattedTime().c_str());
  }
  return have_time;
}


void on_interval_elapsed( uint32_t elapsed, uint32_t events ) {
  syslog.logf(LOG_INFO, "%u events in %u ms", events, elapsed);
}


void check_events()
{
  static const uint32_t counter_interval = 5000; // ms to accumulate irq events

  uint32_t now = millis();
  uint32_t elapsed = now - last_counter_reset;
  if (elapsed >= counter_interval)
  {
    uint32_t events = counter_events;

    counter_events = 0;
    last_counter_reset = now;

    on_interval_elapsed(elapsed, events);
  }
}


void breathe() {
  static const uint32_t interval = 4000;   // ms for one cycle
  static uint32_t start = 0;
  static uint32_t max_duty = PWMRANGE / 2; // max brightness
  static uint32_t prev_duty = 0;

  uint32_t now = millis();
  uint32_t elapsed = now - start;
  if( elapsed > interval ) {
    start = now;
    elapsed -= interval;
  }
  
  uint32_t duty = max_duty * elapsed * 2 / interval;
  if( duty > max_duty ) {
    duty = 2 * max_duty - duty;
  }

  duty = duty * duty / max_duty;

  if (duty != prev_duty)
  {
    prev_duty = duty;
    analogWrite(LED_BUILTIN, PWMRANGE - duty);
  }
}


void loop() {
  ntp.update();
  if( check_ntptime() ) {
    breathe();
  }
  check_events();
  delay(5);
}
