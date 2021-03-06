#include <Arduino.h>

// Web Updater
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>

// Post to InfluxDB
#include <ESP8266HTTPClient.h>

// Infrastructure
#include <NTPClient.h>
#include <Syslog.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof(*(a)))

#define LED_ON LOW
#define LED_OFF HIGH

#define WEBSERVER_PORT 80

ESP8266WebServer web_server(WEBSERVER_PORT);

ESP8266HTTPUpdateServer esp_updater;

// Post to InfluxDB
WiFiClient client;
HTTPClient http;
int influx_status = 0;

const uint32_t ok_interval = 5000;
const uint32_t err_interval = 1000;

uint32_t breathe_interval = ok_interval; // ms for one led breathe cycle

WiFiUDP ntpUDP;
NTPClient ntp(ntpUDP, NTP_SERVER);
static char start_time[30] = "";

WiFiUDP logUDP;
Syslog syslog(logUDP, SYSLOG_PROTO_IETF);

typedef struct counts {
  uint32_t last_5s;
  uint32_t last_1m;
  uint32_t last_10m;
  uint32_t last_1h;
  uint32_t last_1d;
} counts_t;

typedef struct events {
  time_t measured;
  counts_t raw; // raw events
  counts_t cpm; // events per minute without idle average
} events_t;

events_t events = {0};                // last measurement
uint32_t last_counter_reset = 0;      // millis() of last counter reset
volatile uint32_t counter_events = 0; // events of current interval so far

ICACHE_RAM_ATTR void event() { counter_events++; }

// Post data to InfluxDB
void post_data() {
  static const char uri[] = "/write?db=" INFLUX_DB "&precision=s";

  char fmt[] = "events,dev=" HOSTNAME ",ver=%s count_5s=%u,cpm=%u,usv_per_h=%.3f\n";
  char msg[sizeof(fmt) + 20 + 2 * 10];
  snprintf(msg, sizeof(msg), fmt, VERSION, events.raw.last_5s,
           events.cpm.last_1m, (double)events.cpm.last_1m / CONVERSION_INDEX);
  http.begin(client, INFLUX_SERVER, INFLUX_PORT, uri);
  http.setUserAgent(PROGNAME);
  influx_status = http.POST(msg);
  String payload = http.getString();
  http.end();
  if (influx_status < 200 || influx_status > 299) {
    breathe_interval = err_interval;
    syslog.logf(LOG_ERR, "Post %s:%d%s status %d response '%s'", INFLUX_SERVER,
                INFLUX_PORT, uri, influx_status, payload.c_str());
  } else {
    breathe_interval = ok_interval;
  };
}

// Define web pages for update, reset or for event infos
void setup_webserver() {
  web_server.on("/events", []() {
    static const char fmt[] = "{\n"
                              " \"meta\": {\n"
                              "  \"device\": \"" HOSTNAME "\",\n"
                              "  \"program\": \"" PROGNAME "\",\n"
                              "  \"version\": \"" VERSION "\",\n"
                              "  \"cpm_idle\": %u,\n"
                              "  \"conv_index\": %u,\n"
                              "  \"started\": \"%s\",\n"
                              "  \"measured\": \"%s\"\n"
                              " },\n"
                              " \"raw\": {\n"
                              "  \"5s\": %u,\n"
                              "  \"1m\": %u,\n"
                              "  \"10m\": %u,\n"
                              "  \"1h\": %u,\n"
                              "  \"1d\": %u\n"
                              " },\n"
                              " \"cpm\": {\n"
                              "  \"5s\": %u,\n"
                              "  \"1m\": %u,\n"
                              "  \"10m\": %u,\n"
                              "  \"1h\": %u,\n"
                              "  \"1d\": %u\n"
                              " },\n"
                              " \"mSv/y\": {\n"
                              "  \"5s\": %.3f,\n"
                              "  \"1m\": %.3f,\n"
                              "  \"10m\": %.3f,\n"
                              "  \"1h\": %.3f,\n"
                              "  \"1d\": %.3f\n"
                              " }\n"
                              "}\n";
    static char msg[sizeof(fmt) + 20 + 6 * 10];
    static char iso_time[30];
    strftime(iso_time, sizeof(iso_time), "%FT%T%Z", localtime(&events.measured));
    snprintf(msg, sizeof(msg), fmt, IDLE_CPM, CONVERSION_INDEX, start_time,
             iso_time, events.raw.last_5s, events.raw.last_1m,
             events.raw.last_10m, events.raw.last_1h, events.raw.last_1d,
             events.cpm.last_5s, events.cpm.last_1m, events.cpm.last_10m,
             events.cpm.last_1h, events.cpm.last_1d,
             365.25 * 24 * events.cpm.last_5s / CONVERSION_INDEX / 1000,
             365.25 * 24 * events.cpm.last_1m / CONVERSION_INDEX / 1000,
             365.25 * 24 * events.cpm.last_10m / CONVERSION_INDEX / 1000,
             365.25 * 24 * events.cpm.last_1h / CONVERSION_INDEX / 1000,
             365.25 * 24 * events.cpm.last_1d / CONVERSION_INDEX / 1000);
    web_server.send(200, "application/json", msg);
  });

  // Call this page to reset the ESP
  web_server.on("/reset", HTTP_POST, []() {
    syslog.log(LOG_NOTICE, "RESET");
    web_server.send(200, "text/html", 
      "<html>\n"
      " <head>\n"
      "  <title>" PROGNAME " v" VERSION "</title>\n"
      "  <meta http-equiv=\"refresh\" content=\"7; url=/\"> \n"
      " </head>\n"
      " <body>Resetting...</body>\n"
      "</html>\n");
    delay(200);
    ESP.restart();
  });

  // Standard page
  static const char fmt[] =
      "<html>\n"
      " <head>\n"
      "  <title>" PROGNAME " v" VERSION "</title>\n"
      "  <meta http-equiv=\"expires\" content=\"5\">\n"
      " </head>\n"
      " <body>\n"
      "  <h1>" PROGNAME " v" VERSION "</h1>\n"
      "  <table><tr>\n"
      "   <td><form action=\"events\">\n"
      "    <input type=\"submit\" name=\"events\" value=\"Events as JSON\" />\n"
      "   </form></td>\n"
      "   <td><form action=\"reset\" method=\"post\">\n"
      "    <input type=\"submit\" name=\"reset\" value=\"Reset\" />\n"
      "   </form></td>\n"
      "  </tr></table>\n"
      "  <div>Post firmware image to /update<div>\n"
      "  <div>Influx status: %d<div>\n"
      " </body>\n"
      "</html>\n";
  static char page[sizeof(fmt) + 10] = "";

  // Index page
  web_server.on("/", []() {
    snprintf(page, sizeof(page), fmt, influx_status);
    web_server.send(200, "text/html", page);
  });

  // Catch all page, gives a hint on valid URLs
  web_server.onNotFound([]() {
    snprintf(page, sizeof(page), fmt, influx_status);
    web_server.send(404, "text/html", page);
  });

  web_server.begin();

  MDNS.addService("http", "tcp", WEBSERVER_PORT);
  syslog.logf(LOG_NOTICE, "Serving HTTP on port %d", WEBSERVER_PORT);
}

void setup() {
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);

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
  // wm.resetSettings();
  if (!wm.autoConnect()) {
    Serial.println("Failed to connect WLAN");
    for (int i = 0; i < 1000; i += 200) {
      digitalWrite(LED_BUILTIN, LED_ON);
      delay(100);
      digitalWrite(LED_BUILTIN, LED_OFF);
      delay(100);
    }
    ESP.restart();
    while (true)
      ;
  }

  digitalWrite(LED_BUILTIN, LED_ON);
  char msg[80];
  snprintf(msg, sizeof(msg), "%s Version %s, WLAN IP is %s", PROGNAME, VERSION,
           WiFi.localIP().toString().c_str());
  Serial.printf(msg);
  syslog.logf(LOG_NOTICE, msg);

  ntp.begin();

  MDNS.begin(HOSTNAME);

  esp_updater.setup(&web_server);
  setup_webserver();

  last_counter_reset = millis();
  attachInterrupt(digitalPinToInterrupt(EVENT_PIN), event, FALLING);
}

bool check_ntptime() {
  static bool have_time = false;
  if (!have_time && ntp.getEpochTime()) {
    have_time = true;
    time_t now = time(NULL);
    strftime(start_time, sizeof(start_time), "%FT%T%Z", localtime(&now));
    syslog.logf(LOG_NOTICE, "Booted at %s", start_time);
  }
  return have_time;
}

uint32_t sum(uint32_t *values, size_t size) {
  uint32_t sum = 0;
  while (size--) {
    sum += *values;
    values++;
  }
  return sum;
}

void fix_cpm(uint32_t &cpm) { cpm = (cpm > IDLE_CPM) ? cpm - IDLE_CPM : 0; }

void on_interval_elapsed(uint32_t interval, uint32_t counts) {
  static bool first = true;
  static uint32_t minute_events[12];
  static size_t minute_event = 0;
  static uint32_t ten_minute_events[10];
  static size_t ten_minute_event = 0;
  static uint32_t hour_events[6];
  static size_t hour_event = 0;
  static uint32_t day_events[24];
  static size_t day_event = 0;

  bool time_to_log = false;

  events.measured = time(NULL);
  events.raw.last_5s = counts;
  events.cpm.last_5s = (uint32_t)((uint64_t)counts * 60000 / interval);
  fix_cpm(events.cpm.last_5s);

  if (first) {
    first = false;
    time_to_log = true;

    for (size_t i = 0; i < ARRAY_SIZE(minute_events); i++) {
      minute_events[i] = counts;
    }
    events.raw.last_1m = minute_events[0] * ARRAY_SIZE(minute_events);
    events.cpm.last_1m = events.raw.last_1m;
    fix_cpm(events.cpm.last_1m);

    for (size_t i = 0; i < ARRAY_SIZE(ten_minute_events); i++) {
      ten_minute_events[i] = minute_events[0] * ARRAY_SIZE(minute_events);
    }
    events.raw.last_10m = ten_minute_events[0] * ARRAY_SIZE(ten_minute_events);
    events.cpm.last_10m = events.raw.last_10m / 10;
    fix_cpm(events.cpm.last_10m);

    for (size_t i = 0; i < ARRAY_SIZE(hour_events); i++) {
      hour_events[i] = ten_minute_events[0] * ARRAY_SIZE(ten_minute_events);
    }
    events.raw.last_1h = hour_events[0] * ARRAY_SIZE(hour_events);
    events.cpm.last_1h = events.raw.last_1h / 60;
    fix_cpm(events.cpm.last_1h);

    for (size_t i = 0; i < ARRAY_SIZE(day_events); i++) {
      day_events[i] = hour_events[0] * ARRAY_SIZE(hour_events);
    }
    events.raw.last_1d = day_events[0] * ARRAY_SIZE(day_events);
    events.cpm.last_1d = events.raw.last_1d / (24 * 60);
    fix_cpm(events.cpm.last_1d);

    minute_event = 1;
  } else {
    minute_events[minute_event] = counts;
    events.raw.last_1m = sum(minute_events, ARRAY_SIZE(minute_events));
    events.cpm.last_1m = events.raw.last_1m;
    fix_cpm(events.raw.last_1m);
    minute_event++;
    if (minute_event == ARRAY_SIZE(minute_events)) {
      minute_event = 0;
      ten_minute_events[ten_minute_event] =
          sum(minute_events, ARRAY_SIZE(minute_events));
      events.raw.last_10m =
          sum(ten_minute_events, ARRAY_SIZE(ten_minute_events));
      events.cpm.last_10m = (events.raw.last_10m + 5) / 10;
      fix_cpm(events.cpm.last_10m);
      ten_minute_event++;
      time_to_log = true; // log once every 10 minutes
      if (ten_minute_event == ARRAY_SIZE(ten_minute_events)) {
        ten_minute_event = 0;
        hour_events[hour_event] =
            sum(ten_minute_events, ARRAY_SIZE(ten_minute_events));
        events.raw.last_1h = sum(hour_events, ARRAY_SIZE(hour_events));
        events.cpm.last_1h = (events.raw.last_1h + 30) / 60;
        fix_cpm(events.cpm.last_1h);
        hour_event++;
        if (hour_event == ARRAY_SIZE(hour_events)) {
          hour_event = 0;
          day_events[day_event] = sum(hour_events, ARRAY_SIZE(hour_events));
          events.raw.last_1d = sum(day_events, ARRAY_SIZE(day_events));
          events.cpm.last_1d = (events.raw.last_1d + (12 * 60)) / (24 * 60);
          fix_cpm(events.cpm.last_1d);
          day_event++;
          if (day_event == ARRAY_SIZE(day_events)) {
            day_event = 0;
          }
        }
      }
    }
  }

  if (time_to_log) {
    syslog.logf(
        LOG_INFO,
        "Events CPM: 5s=%2u,  1m=%2u,  10m=%2u,  1h=%2u,  1d=%2u,  RAW: "
        "5s=%2u,  1m=%2u,  10m=%3u,  1h=%4u,  1d=%5u",
        events.cpm.last_5s, events.cpm.last_1m, events.cpm.last_10m,
        events.cpm.last_1h, events.cpm.last_1d, events.raw.last_5s,
        events.raw.last_1m, events.raw.last_10m, events.raw.last_1h,
        events.raw.last_1d);
  }

  post_data();
}

void check_events() {
  static const uint32_t counter_interval = 5000; // ms to accumulate irq events

  uint32_t now = millis();
  uint32_t elapsed = now - last_counter_reset;
  if (elapsed >= counter_interval) {
    uint32_t events = counter_events;

    counter_events = 0;
    last_counter_reset += counter_interval;

    on_interval_elapsed(counter_interval, events);
  }
}

void breathe() {
  static uint32_t start = 0;
  static uint32_t max_duty = PWMRANGE / 2; // max brightness
  static uint32_t prev_duty = 0;

  uint32_t now = millis();
  uint32_t elapsed = now - start;
  if (elapsed > breathe_interval) {
    start = now;
    elapsed -= breathe_interval;
  }

  uint32_t duty = max_duty * elapsed * 2 / breathe_interval;
  if (duty > max_duty) {
    duty = 2 * max_duty - duty;
  }

  duty = duty * duty / max_duty;

  if (duty != prev_duty) {
    prev_duty = duty;
    analogWrite(LED_BUILTIN, PWMRANGE - duty);
  }
}

void loop() {
  ntp.update();
  if (check_ntptime()) {
    breathe();
  }
  check_events();
  web_server.handleClient();
  delay(5);
}
