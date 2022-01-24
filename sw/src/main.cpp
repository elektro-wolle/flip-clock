#include <Arduino.h>

#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>

#include <AceTime.h>

#if !defined(NTP_SERVER)
#define NTP_SERVER "pool.ntp.org"
#endif

static const time_t EPOCH_2000_01_01 = 946684800;
static const unsigned long REBOOT_TIMEOUT_MILLIS = 15000;

using namespace ace_time;
static const int CACHE_SIZE = 3;
static ExtendedZoneProcessor localZoneProcessor;
static TimeZone localZone = TimeZone::forZoneInfo(
    &zonedbx::kZoneEurope_Berlin,
    &localZoneProcessor);
;

ESP8266WebServer server(80);

// /EN auf D5, D5 = 14
#define ENABLE 14
// STEP auf D4 = 2
#define STEP 2
// DIR auf D3 = 0
#define DIR 0

static uint16_t baseline = 0;

void calculateBaseline()
{
    int base = 0;
    for (int x = 0; x < 50; x++) {
        base += analogRead(A0);
        delay(1);
    }
    baseline = base / 50;
}

void setupSntp()
{
    Serial.print(F("Configuring SNTP"));
    configTime(0 /*timezone*/, 0 /*dst_sec*/, NTP_SERVER);

    // Wait until SNTP stabilizes by ignoring values before year 2000.
    unsigned long startMillis = millis();
    while (true) {
        Serial.print('.'); // Each '.' represents one attempt.
        time_t now = time(nullptr);
        if (now >= EPOCH_2000_01_01) {
            Serial.println(F(" Done."));
            break;
        }

        // Detect timeout and reboot.
        unsigned long nowMillis = millis();
        if ((unsigned long)(nowMillis - startMillis) >= REBOOT_TIMEOUT_MILLIS) {
            Serial.println(F(" FAILED! Rebooting..."));
            delay(1000);
            ESP.reset();
        }

        delay(500);
    }
}

int16_t currentDisplayedTime = 9 * 60 + 44;
int16_t currentTime = 07 * 60 + 55;
bool fastMode = true;
boolean setUpDone = false;

void setCurrentTime()
{
    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds(
        localTime, localZone);
    currentTime = zonedDateTime.minute() + zonedDateTime.hour() * 60;
    zonedDateTime.printTo(Serial);
    Serial.printf(" - hour=%d, minute=%d\n", zonedDateTime.hour(), zonedDateTime.minute());
}

void handleRoot()
{
    int hour = (((1440 + currentDisplayedTime) / 60) % 24);
    int minute = (1440 + currentDisplayedTime) % 60;

    String webpage = "<!DOCTYPE html><html><head>";
    webpage += "<title>FlipClock</title><style>";
    webpage += "body {width:100%;margin:0 auto;font-family:arial;font-size:14px;text-align:center;color:blue;background-color:#F7F2Fd;}";
    webpage += "</style></head><body><h1>FlipClock</h1>";
    webpage += "Aktuell angezeigte Zeit:</br>";
    webpage += "<form action=\"/set\" method=\"POST\">";
    webpage += "Stunde: <input type=\"number\" name=\"hour\" value=\"" + String(hour) + "\" min=\"0\" max=\"23\"></br>";
    webpage += "Minute: <input type=\"number\" name=\"minute\" value=\"" + String(minute) + "\" min=\"0\" max=\"59\"></br>";
    webpage += "<input type=\"submit\" value=\"Speichern\"></form>";
    webpage += "</body></html>";
    server.send(200, "text/html", webpage);
}

void handleSet()
{
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    currentDisplayedTime = (hour * 60 + minute) % 1440;
    setUpDone = true;
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

void setup()
{
    pinMode(ENABLE, OUTPUT);
    pinMode(STEP, OUTPUT);
    pinMode(DIR, OUTPUT);
    digitalWrite(ENABLE, HIGH);
    digitalWrite(DIR, HIGH);
    Serial.begin(115200);
    calculateBaseline();
    WiFiManager wifiManager;
    wifiManager.autoConnect("FlipClock");

    if (MDNS.begin("flipclock")) { // Start the mDNS responder for esp8266.local
        Serial.println("mDNS responder started");
    } else {
        Serial.println("Error setting up MDNS responder!");
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/set", HTTP_POST, handleSet);
    server.onNotFound([]() {
        server.send(404, "text/plain", "404: Not found");
    });
    server.begin();

    setupSntp();
    setCurrentTime();

    // assume short outage
    currentDisplayedTime = currentTime;

#ifdef OTA
    // Port defaults to 8266
    ArduinoOTA.setPort(8266);

    // Hostname defaults to esp8266-[ChipID]
    ArduinoOTA.setHostname("flipclock");

    // No authentication by default
    // ArduinoOTA.setPassword("admin");

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else { // U_FS
            type = "filesystem";
        }
        // NOTE: if updating FS this would be the place to unmount FS using FS.end()
        Serial.println("Start updating " + type);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("\nEnd");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    });
    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            Serial.println("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            Serial.println("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            Serial.println("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            Serial.println("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            Serial.println("End Failed");
        }
    });
    ArduinoOTA.begin();
#endif
}

bool advance()
{
    digitalWrite(ENABLE, LOW);
    digitalWrite(STEP, HIGH);
    delayMicroseconds(1);
    digitalWrite(STEP, LOW);

    static uint16_t advances = 0;
    advances++;
    for (int count = 0; count < 50; count++) {
        int val = analogRead(A0);
        if (advances > 20) {
            if (val < 0.9 * baseline || advances > 100) {
                Serial.printf("val=%d, base=%d, advance=%d\n", val, baseline, advances);
                advances = 0;
                currentDisplayedTime++;
                if (currentDisplayedTime >= 1440) {
                    currentDisplayedTime -= 1440;
                }
                Serial.printf("%02d:%02d  - %d/%d\n\n", (((1440 + currentDisplayedTime) / 60) % 24), (1440 + currentDisplayedTime) % 60, currentDisplayedTime, currentTime);
                if (fastMode == false) {
                    digitalWrite(ENABLE, HIGH);
                }
                return true;
            }
        }
        delayMicroseconds(1);
    }
    return false;
}

void loop()
{
    server.handleClient();
#ifdef OTA
    ArduinoOTA.handle();
#endif
    if (setUpDone) {
        if (currentDisplayedTime < currentTime) {
            bool advanced = advance();
            if (advanced) {
                delay(500);
            }
        } else {
            fastMode = false;
        }
        //
        if (currentDisplayedTime > currentTime && currentTime >= 1 * 60 && currentTime <= 6 * 60) {
            // just wait for time to arrive in the night
        } else {
            // if more than 10 minutes late: advance the whole day
            if (currentDisplayedTime > currentTime + 10) {
                fastMode = true;
                currentDisplayedTime -= 1440;
            }
        }
    }

    static unsigned long lastMillis = millis();
    unsigned long now = millis();
    if (now >= lastMillis + 1000) {
        static int seconds = 0;

        lastMillis = now;
        seconds++;
        setCurrentTime();
        calculateBaseline();
    }
}
