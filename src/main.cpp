#include <Arduino.h>

#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Syslog.h>

#define LED_ON  LOW
#define LED_OFF HIGH

#define IRQ_PIN D2

#define IDLE_CPM 25

#define ARRAY_SIZE(a) (sizeof(a)/sizeof(*(a)))


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
  snprintf(msg, sizeof(msg), "Version %s, WLAN IP is %s", VERSION, WiFi.localIP().toString().c_str());
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


uint32_t sum( uint32_t *values, size_t size ) {
  uint32_t sum = 0;
  while( size-- ) {
    sum += *values;
    values++;
  }
  return sum;
}


void fix_cpm( uint32_t &cpm ) {
  cpm = (cpm > IDLE_CPM) ? cpm - IDLE_CPM : 0;
}


void on_interval_elapsed( uint32_t interval, uint32_t events ) {
  static uint32_t minute_events[12];
  static uint32_t minute_cpm = 0;
  static size_t minute_event = 0;
  static bool minute_valid = false;
  static uint32_t ten_minute_events[10];
  static uint32_t ten_minute_cpm = 0;
  static size_t ten_minute_event = 0;
  static bool ten_minute_valid = false;
  static uint32_t hour_events[6];
  static uint32_t hour_cpm = 0;
  static size_t hour_event = 0;
  static bool hour_valid = false;
  static uint32_t day_events[24];
  static uint32_t day_cpm = 0;
  static size_t day_event = 0;
  static bool day_valid = false;

  uint32_t cpm = (uint32_t)((uint64_t)events * 60000 / interval);
  fix_cpm(cpm);

  if (minute_valid == false && minute_event == 0)
  {
    for( size_t i = 0; i < ARRAY_SIZE(minute_events); i++ ) {
      minute_events[i] = events;
    }
    minute_cpm = minute_events[0] * ARRAY_SIZE(minute_events);
    fix_cpm(minute_cpm);

    for( size_t i = 0; i < ARRAY_SIZE(ten_minute_events); i++ ) {
      ten_minute_events[i] = minute_events[0] * ARRAY_SIZE(minute_events);
    }
    ten_minute_cpm = ten_minute_events[0] * ARRAY_SIZE(ten_minute_events) / 10;
    fix_cpm(ten_minute_cpm);

    for( size_t i = 0; i < ARRAY_SIZE(hour_events); i++ ) {
      hour_events[i] = ten_minute_events[0] * ARRAY_SIZE(ten_minute_events);
    }
    hour_cpm = hour_events[0] * ARRAY_SIZE(hour_events) / 60;
    fix_cpm(hour_cpm);

    for( size_t i = 0; i < ARRAY_SIZE(day_events); i++ ) {
      day_events[i] = hour_events[0] * ARRAY_SIZE(hour_events);
    }
    day_cpm = day_events[0] * ARRAY_SIZE(day_events) / (24 * 60);
    fix_cpm(day_cpm);

    minute_event = 1;
  }
  else {
    minute_events[minute_event] = events;
    minute_cpm = sum(minute_events, ARRAY_SIZE(minute_events));
    fix_cpm(minute_cpm);
    minute_event++;
    if( minute_event == ARRAY_SIZE(minute_events) ) {
      minute_event = 0;
      minute_valid = true;

      ten_minute_events[ten_minute_event] = sum(minute_events, ARRAY_SIZE(minute_events));
      ten_minute_cpm = (sum(ten_minute_events, ARRAY_SIZE(ten_minute_events)) + 5) / 10;
      fix_cpm(ten_minute_cpm);
      ten_minute_event++;
      if( ten_minute_event == ARRAY_SIZE(ten_minute_events) ) {
        ten_minute_event = 0;
        ten_minute_valid = true;

        hour_events[hour_event] = sum(ten_minute_events, ARRAY_SIZE(ten_minute_events));
        hour_cpm = (sum(hour_events, ARRAY_SIZE(hour_events)) + 30) / 60;
        fix_cpm(hour_cpm);
        hour_event++;
        if(hour_event == ARRAY_SIZE(hour_events) ) {
          hour_event = 0;
          hour_valid = true;

          day_events[day_event] = sum(hour_events, ARRAY_SIZE(hour_events));
          day_cpm = (sum(day_events, ARRAY_SIZE(day_events)) + 12 * 60) / (24 * 60);
          fix_cpm(day_cpm);
          day_event++;
          if( day_event == ARRAY_SIZE(day_events) ) {
            day_event = 0;
            day_valid = true;
          }
        }
      }
    }
  }

  syslog.logf(LOG_INFO, "%u events in %u ms => cpm curr=%u, min=%u, 10min=%u, hour=%u, day=%u",
              events, interval, cpm, minute_cpm, ten_minute_cpm, hour_cpm, day_cpm);
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
    last_counter_reset += counter_interval;

    on_interval_elapsed(counter_interval, events);
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
