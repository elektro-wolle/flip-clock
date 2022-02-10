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
#define DRD_ADDRESS 4
#define EEPROM_MAGIC_NUMBER 0xdeadbeed
#define MAX_STEPS_PER_FLIP 40

typedef struct {
    uint32_t magicNumber;
    uint32_t uptimeSeconds;
    uint32_t uptimeSecondsTotal;
    uint32_t previousSecondsTotal;
    uint16_t reboots;
    uint16_t skipped;
    uint16_t skippedTotal;
    uint32_t previousSkippedTotal;
    uint32_t zoneId;
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

std::vector<int16_t> irValues;

Logger logger;

static const time_t EPOCH_2000_01_01 = 946684800;
static const unsigned long REBOOT_TIMEOUT_MILLIS = 5000;

using namespace ace_time;
using namespace ace_time::zonedbx;
static const int CACHE_SIZE = 3;
ExtendedZoneProcessorCache<1> zoneProcessorCache;
static ExtendedZoneProcessor localZoneProcessor;

ExtendedZoneManager zoneManager(
    zonedbx::kZoneRegistrySize,
    zonedbx::kZoneRegistry,
    zoneProcessorCache);

static TimeZone localZone = zoneManager.createForZoneInfo(&zonedbx::kZoneEurope_Berlin);

ESP8266WebServer server(80);

// /EN auf D5, D5 = 14
#define ENABLE 14
// STEP auf D4 = 2
#define STEP 2
// DIR auf D3 = 0
#define DIR 0

static uint16_t irPhotoDiodeBaseLine = analogRead(A0);
int16_t currentDisplayedTime = 9 * 60 + 44;
int16_t currentTime = 07 * 60 + 55;
bool fastMode = true;

int16_t upTriggeredAt = -1;
int16_t downTriggeredAt = -1;

void setCurrentTime();
void calculateBaseline();

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

String secondsToString(uint32_t seconds)
{
    String result = "";
    if (seconds > 86400) {
        result = String(seconds / 86400) + "d ";
    }
    result = result + String((seconds / 3600) % 24) + "h ";
    result = result + String((seconds / 60) % 60) + "m ";
    result = result + String(seconds % 60) + "s";
    return result;
}

void handleRoot()
{
    int hour = (((1440 + currentDisplayedTime) / 60) % 24);
    int minute = (1440 + currentDisplayedTime) % 60;

    server.chunkedResponseModeStart(200, "text/html; charset=utf-8");

    String webpage = "<!DOCTYPE html><html><head>\n";
    webpage += "<title>FlipClock</title><style>\n";
    webpage += "body{margin-left:5em;margin-right:5em;font-family:sans-serif;font-size:14px;color:darkslategray;background-color:#EEE}h1{text-align:center}.info{width:100%;text-align:left;font-size:18pt}input,main,option,select,th{font-size:24pt;text-align:left}input{width:100%}input[type='submit']{width:min-content;float:right;text-align:right}main{font-size:16pt;vertical-align:middle}.info{line-height:2em}.info br{margin-left:3em}.logs{margin-top:2em;padding-top:2em;overflow-x:auto;border-top:black 2px solid}ul li{text-align:left}\n";
    webpage += ".graph {background-color: #EEE; font-size:0; overflow-x: auto; padding-bottom: 40px;} .bar { background-color: blueviolet; width: 1px; display: inline-block; } .active { background-color: green; }";
    webpage += "</style></head><body><h1>FlipClock by Wolfgang Jung</h1><div class='main'>\n";
    webpage += "<h2>Aktuell angezeigte Zeit:</h2>\n";
    webpage += "<form action=\"/set\" method=\"POST\"><table>\n";
    webpage += "<tr><th>Stunde:</th><td><input type=\"number\" name=\"hour\" value=\"" + String(hour) + "\" min=\"0\" max=\"23\"></td></tr>";
    webpage += "<tr><th>Minute:</th><td><input type=\"number\" name=\"minute\" value=\"" + String(minute) + "\" min=\"0\" max=\"59\"></td></tr>";
    webpage += "<tr><th>Zeitzone:</th><td><select name='zone'>\n";
    server.sendContent(webpage);

    uint16_t indexes[zonedbx::kZoneRegistrySize];
    ace_time::ZoneSorterByName<ExtendedZoneManager> zoneSorter(zoneManager);
    zoneSorter.fillIndexes(indexes, zonedbx::kZoneRegistrySize);
    zoneSorter.sortIndexes(indexes, zonedbx::kZoneRegistrySize);
    for (int i = 0; i < zonedbx::kZoneRegistrySize; i++) {
        ace_common::PrintStr<32> printStr;
        ExtendedZone zone = zoneManager.getZoneForIndex(indexes[i]);
        zone.printNameTo(printStr);
        webpage = "<option value='" + String(indexes[i]) + "'";
        if (zone.zoneId() == globalStats.zoneId) {
            webpage += " selected='selected'";
        }
        webpage += ">" + String(printStr.getCstr()) + "</option>\n";
        server.sendContent(webpage);
    }

    webpage = "</select></td></tr>";
    webpage += "<tr><th></th><td><input id='save' type=\"submit\" value=\"Speichern\"></td></tr></table></form><br/></div>\n";
    webpage += "<div class='info'>";
    webpage += "<div class='time'><h2>Aktuelle Zeit</h2><tt>";

    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds(
        localTime, localZone);
    ace_common::PrintStr<60> currentTimeStr;
    zonedDateTime.printTo(currentTimeStr);

    webpage += String(currentTimeStr.getCstr()) + "</tt></div></br>\n";
    webpage += "<div class='stats'><h2>Stats</h2>\n";
    webpage += "Uptime:" + secondsToString(globalStats.uptimeSeconds) + "<br/>\n";
    webpage += "Uptime gesamt:" + secondsToString(globalStats.uptimeSecondsTotal) + "<br/>\n";
    webpage += "Skips :" + String(globalStats.skipped) + "<br/>\n";
    webpage += "Skips gesamt:" + String(globalStats.skippedTotal) + "<br/>\n";
    webpage += "Reboots:" + String(globalStats.reboots) + "<br/>\n";
    webpage += "Version: " + String(__TIMESTAMP__) + "<br/></div></div>\n";
    server.sendContent(webpage);

    if (logger.lastItems.size() > 0) {
        server.sendContent("<div class='logs'><h2>Logs</h2><ul>\n");

        for (std::list<String>::reverse_iterator line = logger.lastItems.rbegin();
             line != logger.lastItems.rend();
             line++) {
            server.sendContent("<li><pre>" + (*line) + "</pre></li>\n");
        }
        server.sendContent("</ul></div>");

        server.sendContent("<div class='graph'>\n");
        int idx = 0;
        for (uint16_t val : irValues) {
            bool visible = (downTriggeredAt < 0 && upTriggeredAt < 0) || (idx > max(downTriggeredAt, (int16_t)(upTriggeredAt - 100)) - 200 && idx < upTriggeredAt + 200);
            if (!visible) {
                idx++;
                continue;
            }
            if (idx == upTriggeredAt || idx == downTriggeredAt) {
                server.sendContent("<div style='height: " + String(val) + "px' class='bar active'></div>\n");
            } else {
                server.sendContent("<div style='height: " + String(val) + "px' class='bar'></div>\n");
            }
            idx++;
        }
        server.sendContent("</div>");
    }

    server.sendContent("</body></html>\n");

    server.chunkedResponseFinalize();
}

void handleSet()
{
    int hour = server.arg("hour").toInt();
    int minute = server.arg("minute").toInt();
    int zoneIdx = server.arg("zone").toInt();
    currentDisplayedTime = (hour * 60 + minute) % 1440;

    localZone = zoneManager.createForZoneIndex(zoneIdx);
    if (localZone.isError() == false) {
        globalStats.zoneId = localZone.getZoneId();
        EEPROM.put(STATS_ADDRESS, globalStats);
        EEPROM.commit();
    }

    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

void readFromEEProm()
{
    // EEPROM.begin(sizeof(statistics_t));
    // Already got begin() called by DRD constructor
    EEPROM.get(STATS_ADDRESS, globalStats);
    if (globalStats.magicNumber != EEPROM_MAGIC_NUMBER) {
        globalStats.magicNumber = EEPROM_MAGIC_NUMBER;
        globalStats.reboots = 0;
        globalStats.skipped = 0;
        globalStats.skippedTotal = 0;
        globalStats.previousSkippedTotal = 0;
        globalStats.uptimeSeconds = 0;
        globalStats.uptimeSecondsTotal = 0;
        globalStats.previousSecondsTotal = 0;
        globalStats.zoneId = zonedbx::kZoneIdEurope_Berlin;
        EEPROM.put(STATS_ADDRESS, globalStats);
    }
    globalStats.previousSecondsTotal = globalStats.uptimeSecondsTotal;
    globalStats.previousSkippedTotal = globalStats.skippedTotal;
    localZone = zoneManager.createForZoneId(globalStats.zoneId);
    if (localZone.isError()) {
        globalStats.zoneId = zonedbx::kZoneIdEurope_Berlin;
        localZone = zoneManager.createForZoneId(globalStats.zoneId);
        EEPROM.put(STATS_ADDRESS, globalStats);
    }
    globalStats.uptimeSeconds = 0;
    globalStats.skipped = 0;

    Serial.print("Using Timezone: ");
    localZone.printTo(Serial);
    Serial.println();
    EEPROM.commit();
}

void setup()
{
    Serial.begin(115200);
    readFromEEProm();
    globalStats.uptimeSeconds = 0;

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

void setCurrentTime()
{
    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds(
        localTime, localZone);
    currentTime = zonedDateTime.minute() + zonedDateTime.hour() * 60;
    if (zonedDateTime.second() > 58) {
        currentTime += 1;
    }
}

void calculateBaseline()
{
    int base = 0;
    for (int x = 0; x < 10; x++) {
        base = base + analogRead(A0);
        delayMicroseconds(10);
    }
    irPhotoDiodeBaseLine = base / 10;
}

void advance()
{
    int16_t minValue = 1024;
    int16_t maxValue = 0;
    bool minuteDisplayFlipped = false;
    uint16_t triggerCount = 0;
    uint16_t minimumAt = 0;
    uint16_t maximumAt = 0;
    upTriggeredAt = -1;
    downTriggeredAt = -1;
    int16_t currentIrReading = 0;

    digitalWrite(ENABLE, LOW);
    digitalWrite(DIR, HIGH);
    calculateBaseline();
    maxValue = 0;
    minValue = 1024;

    irValues.clear();
    irValues.resize(0);

    for (uint16_t stepperMotorSteps = 0; stepperMotorSteps < 60; stepperMotorSteps++) {
        if (!minuteDisplayFlipped) {
            digitalWrite(STEP, HIGH);
            delayMicroseconds(1);
            digitalWrite(STEP, LOW);
        }
        for (int count = 0; count < 30; count++) {
            currentIrReading = analogRead(A0);
            irValues.push_back(currentIrReading);
            if (currentIrReading < minValue) {
                minimumAt = stepperMotorSteps;
                minValue = currentIrReading;
            }
            if (currentIrReading > maxValue) {
                maximumAt = stepperMotorSteps;
                maxValue = currentIrReading;
            }
            int historyElements = irValues.size();
            if (historyElements > 300) {
                bool up = currentIrReading - irValues[historyElements - 100] > 50;
                // small rising edge if at least 60% of highest swing so far but also at least 10 ticks high
                bool smallUp = 10 * (currentIrReading - irValues[historyElements - 50]) > 6 * max(10, maxValue - minValue);
                bool down = irValues[historyElements - 100] - currentIrReading > 50;
                if (up || down || (!down && smallUp)) {
                    triggerCount++;
                    if (down && downTriggeredAt == -1) {
                        downTriggeredAt = historyElements;
                    }
                    if ((up || (!down && smallUp)) && upTriggeredAt == -1) {
                        upTriggeredAt = historyElements;
                    }
                    minuteDisplayFlipped = true;
                }
            }
            delayMicroseconds(100);
        }
    }

    if (triggerCount <= 5) {
        globalStats.skipped++;
    } else {
        currentDisplayedTime++;
    }
    
    for (std::vector<int16_t>::iterator it = irValues.begin(); it != irValues.end(); it++) {
        *it = *it - minValue;
    }

    if (currentDisplayedTime >= 1440) {
        currentDisplayedTime -= 1440;
    }
#ifdef DEBUG
    time_t localTime = time(nullptr);
    ZonedDateTime zonedDateTime = ZonedDateTime::forUnixSeconds(
        localTime, localZone);
    ace_common::PrintStr<64> currentTimeStr;
    zonedDateTime.printTo(currentTimeStr);

    logger.printf("%s - %02d:%02d - triggerCount=%d up=%d down=%d val=%d min=%d@%d max=%d@%d base=%d\n",
        currentTimeStr.getCstr(),
        (((1440 + currentDisplayedTime) / 60) % 24),
        (1440 + currentDisplayedTime) % 60,
        triggerCount, upTriggeredAt, downTriggeredAt, currentIrReading, minValue, minimumAt, maxValue, maximumAt, irPhotoDiodeBaseLine);
#endif
}

template <int T>
void runEvery(void (*f)())
{
    static unsigned long lastMillis = millis();
    unsigned long now = millis();
    if (now >= lastMillis + T) {
        // run every T milli-seconds
        f();
        lastMillis = millis();
    }
}

void loop()
{
    // WebServer should react..
    server.handleClient();
#ifdef OTA
    // OTA the same
    ArduinoOTA.handle();
#endif

    // The main time-handling code, wait at least 1 second between flips
    runEvery<1000>([]() {
        if (currentDisplayedTime < currentTime) {
            advance();
        } else if (currentDisplayedTime == currentTime) {
            digitalWrite(ENABLE, HIGH);
            fastMode = false;
        } else { // currentDisplayedTime > currentTime
            if (currentTime >= 1 * 60 && currentTime <= 6 * 60) {
                // do nothing in the night
                fastMode = false;
            } else if (currentDisplayedTime > currentTime + 10) {
                fastMode = true;
                currentDisplayedTime -= 1440;
            }
        }
    });

    // every half second -> housekeeping of time and name
    runEvery<500>([]() {
        globalStats.uptimeSeconds = (millis() / 1000);
        globalStats.uptimeSecondsTotal = globalStats.previousSecondsTotal + globalStats.uptimeSeconds;
        globalStats.skippedTotal = globalStats.previousSkippedTotal + globalStats.skipped;
        setCurrentTime();
        drd.loop();
        MDNS.update();
    });

    // every 15 minutes -> store stats
    runEvery<1000 * 15 * 60>([]() {
        EEPROM.put(STATS_ADDRESS, globalStats);
        EEPROM.commit();
    });
}
