#include <Arduino.h>

#define ESP_DRD_USE_EEPROM true

#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP_DoubleResetDetector.h>
#include <WiFiManager.h>
#include <WiFiUdp.h>

#include <AceTime.h>

#include <list>

#define OTA 1

#if !defined(NTP_SERVER)
#define NTP_SERVER "pool.ntp.org"
#endif

#define DEBUG 1

#define STATS_ADDRESS 10
#define DRD_ADDRESS   4
#define EEPROM_MAGIC_NUMBER 0xdeadbeef

typedef struct {
    uint32_t magicNumber;
    uint32_t previousSecondsTotal;
    uint32_t uptimeSecondsTotal;
    uint32_t uptimeSeconds;
    uint16_t reboots;
    uint16_t skipped;
} statistics_t;

statistics_t globalStats;

DoubleResetDetector drd(DRD_ADDRESS, 0);

class Logger : public Print {

public:
    size_t write(uint8_t c)
    {
        if (c == '\n') {
            lastItems.push_back(currentLine);
            if (lastItems.size() > 100) {
                lastItems.pop_front();
            }
            currentLine = String("");
        } else {
            currentLine += (char)c;
        }
        return 1;
    }
    std::list<String> lastItems;

private:
    String currentLine = String("");
};

Logger logger;

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

static uint16_t irPhotoDiodeBaseLine = 0;
int16_t currentDisplayedTime = 9 * 60 + 44;
int16_t currentTime = 07 * 60 + 55;
bool fastMode = true;
bool minuteDisplayFlipped = false;

void calculateBaseline()
{
    int base = 0;
    for (int x = 0; x < 10; x++) {
        base += analogRead(A0);
        delayMicroseconds(10);
    }
    irPhotoDiodeBaseLine = base / 10;
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
            drd.loop();
            delay(1000);
            ESP.reset();
        }

        delay(500);
    }
}

void setCurrentTime()
{
    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds(
        localTime, localZone);
    currentTime = zonedDateTime.minute() + zonedDateTime.hour() * 60;
    if (zonedDateTime.second() > 57) {
        currentTime += 1;
    }

#ifdef DEBUG
    zonedDateTime.printTo(logger);
    logger.printf(" - hour=%d, minute=%d\n", zonedDateTime.hour(), zonedDateTime.minute());
#endif
}

void handleRoot()
{
    int hour = (((1440 + currentDisplayedTime) / 60) % 24);
    int minute = (1440 + currentDisplayedTime) % 60;

    int a_hour = (((1440 + currentTime) / 60) % 24);
    int a_minute = (1440 + currentTime) % 60;

    String webpage = "<!DOCTYPE html><html><head>";
    webpage += "<title>FlipClock</title><style>";
    webpage += "body {margin:0 auto;font-family:arial;font-size:14px;text-align:center;color:blue;background-color:#F7F2Fd;} .info { margin-left: 25%; text-align: left; width: 300px; } ul li {text-align: left;max-width: 500px;}";
    webpage += "</style></head><body><h1>FlipClock by Wolfgang Jung</h1>";
    webpage += "Aktuell angezeigte Zeit:</br>";
    webpage += "<form action=\"/set\" method=\"POST\">";
    webpage += "Stunde: <input type=\"number\" name=\"hour\" value=\"" + String(hour) + "\" min=\"0\" max=\"23\"></br>";
    webpage += "Minute: <input type=\"number\" name=\"minute\" value=\"" + String(minute) + "\" min=\"0\" max=\"59\"></br>";
    webpage += "<input type=\"submit\" value=\"Speichern\"></form><br/>";
    
    webpage += "<div class='info'>";
    webpage += "Aktuelle Zeit: ";
    webpage += String(a_hour) + ":" + String(a_minute) + "</br>";
    webpage += "Stats:<br>";
    webpage += "Uptime seit letztem Reset:" + String(globalStats.uptimeSeconds) + "<br/>";
    webpage += "Uptime:" + String(globalStats.uptimeSecondsTotal) + "<br/>";
    webpage += "Skip error count:" + String(globalStats.skipped) + "<br/>";
    webpage += "Reboots:" + String(globalStats.reboots) + "<br/>";

    webpage += "Logs:<br/><ul>";
    for (std::list<String>::reverse_iterator line = logger.lastItems.rbegin();
         line != logger.lastItems.rend();
         line++) {
        webpage += "<li><pre>" + (*line) + "</pre></li>";
    }
    webpage += "</ul>";
    webpage +=" </div></body></html>";
    server.send(200, "text/html", webpage);
}

void handleSet()
{
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    currentDisplayedTime = (hour * 60 + minute) % 1440;
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

void readFromEEProm() {
    // EEPROM.begin(sizeof(statistics_t));
    // Already got begin() called by DRD constructor
    EEPROM.get(STATS_ADDRESS, globalStats);
    if (globalStats.magicNumber != EEPROM_MAGIC_NUMBER) {
        globalStats.magicNumber = EEPROM_MAGIC_NUMBER;
        globalStats.reboots = 0;
        globalStats.skipped = 0;
        globalStats.uptimeSeconds = 0;
        globalStats.uptimeSecondsTotal = 0;
        globalStats.previousSecondsTotal = 0;
        EEPROM.put(STATS_ADDRESS, globalStats);
        EEPROM.commit();
    }
    globalStats.previousSecondsTotal = globalStats.uptimeSecondsTotal;
}

void setup()
{
    readFromEEProm();
    globalStats.uptimeSeconds = 0;

    Serial.begin(115200);
    Serial.println(F("\nStarting FlipClock 2022 - Wolfgang Jung / Ideas In Logic\n"));

    pinMode(ENABLE, OUTPUT);
    pinMode(STEP, OUTPUT);
    pinMode(DIR, OUTPUT);
    pinMode(D0, OUTPUT);
    digitalWrite(D0, HIGH);

    digitalWrite(ENABLE, HIGH);
    calculateBaseline();

    WiFiManager wiFiManager;

    if (drd.detectDoubleReset()) {
        digitalWrite(ENABLE, LOW);
        Serial.println(F("Reset WiFi configuration"));
        wiFiManager.resetSettings();
        wiFiManager.startConfigPortal("FlipClock", "");
    }

#ifdef DEBUG
    logger.println(F("Trying to connect to known WiFi"));
#endif
    if (!wiFiManager.autoConnect("FlipClock")) {
        digitalWrite(ENABLE, LOW);
        Serial.println("failed to connect and hit timeout");
        delay(3000);
        digitalWrite(ENABLE, HIGH);
        // reset and try again, or maybe put it to deep sleep
        ESP.reset();
    }

    if (MDNS.begin("flipclock")) { // Start the mDNS responder for esp8266.local
#ifdef DEBUG
        logger.println("mDNS responder started");
#endif
    } else {
        logger.println("Error setting up MDNS responder!");
    }

    digitalWrite(ENABLE, LOW);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/set", HTTP_POST, handleSet);
    server.onNotFound([]() {
        server.send(404, "text/plain", "404: Not found");
    });
    server.begin();

    globalStats.reboots++;
    EEPROM.put(STATS_ADDRESS, globalStats);

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
#ifdef DEBUG
        logger.printf("Progress: %u%%\r", (progress / (total / 100)));
#endif
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
    digitalWrite(ENABLE, HIGH);
}

bool advance()
{
    digitalWrite(ENABLE, LOW);
    digitalWrite(STEP, HIGH);
    digitalWrite(DIR, HIGH);
    delayMicroseconds(1);
    digitalWrite(STEP, LOW);

    static uint16_t stepperMotorSteps = 0;
    stepperMotorSteps++;
    uint16_t triggerCount = 0;
    int minValue = 1024;
    int maxValue = 0;
    for (int count = 0; count < 3000; count++) {
        if (count % 10 == 0) {
            // digitalWrite(D0, HIGH);
            int val = analogRead(A0);
            minValue = min(minValue, val);
            maxValue = max(maxValue, val);
            // digitalWrite(D0, LOW);

            if (stepperMotorSteps > 5) {
                if (val < 0.95 * irPhotoDiodeBaseLine || stepperMotorSteps > 50) {
#ifdef DEBUG
                    logger.printf("%d: triggers=%d, val=%d, base=%d, advance=%d -> %d/%d\n",
                        count,
                        triggerCount,
                        val, irPhotoDiodeBaseLine,
                        stepperMotorSteps,
                        minValue,
                        maxValue);
#endif
                    triggerCount++;
                    if (triggerCount > 3 || stepperMotorSteps > 50) {
                        if (stepperMotorSteps > 50) {
                            globalStats.skipped++;
                        }
                        minuteDisplayFlipped = true;
                    }
                }

            } else if (stepperMotorSteps == 5) {
                calculateBaseline();
            }

            if (minuteDisplayFlipped) {
                currentDisplayedTime++;
                if (currentDisplayedTime >= 1440) {
                    currentDisplayedTime -= 1440;
                }
#ifdef DEBUG
                logger.printf("%02d:%02d  - %d/%d steps=%d\n\n",
                    (((1440 + currentDisplayedTime) / 60) % 24),
                    (1440 + currentDisplayedTime) % 60,
                    currentDisplayedTime,
                    currentTime,
                    stepperMotorSteps);
#endif
                stepperMotorSteps = 0;
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

uint32_t lastEepromWrite = 0;

void loop()
{
    server.handleClient();
#ifdef OTA
    ArduinoOTA.handle();
#endif
    if (currentDisplayedTime < currentTime) {
        fastMode = true;
        static unsigned long waitTime = millis();
        if (millis() > waitTime) {
            bool advanced = advance();
            if (advanced) {
                minuteDisplayFlipped = false;
                waitTime = millis() + 1000;
            }
        }
    } else {
        fastMode = false;
    }
    //
    if (currentDisplayedTime > currentTime && currentTime >= 1 * 60 && currentTime <= 6 * 60) {
        fastMode = false;
        // just wait for time to arrive in the night
    } else {
        // if more than 10 minutes late: advance the whole day
        if (currentDisplayedTime > currentTime + 10) {
            fastMode = true;
            currentDisplayedTime -= 1440;
        }
    }

    static unsigned long lastMillis = millis();
    unsigned long now = millis();
    if (now >= lastMillis + 1000) {
        // run every second
        lastMillis = now;
        globalStats.uptimeSeconds = (now / 1000);
        globalStats.uptimeSecondsTotal = globalStats.previousSecondsTotal + globalStats.uptimeSeconds;
        EEPROM.put(STATS_ADDRESS, globalStats);

        setCurrentTime();
        drd.loop();
        MDNS.update();
    }
    if (now > lastEepromWrite + (1000 * 60 * 15)) { // every 15 minutes        
        EEPROM.commit();
        lastEepromWrite = now;
    }
}
