#include <Arduino.h>

// /EN auf D5, D5 = 14
#define ENABLE 14
// STEP auf D4 = 2
#define STEP 2
// DIR auf D3 = 0
#define DIR 0

void setup()
{
    pinMode(ENABLE, OUTPUT);
    pinMode(STEP, OUTPUT);
    pinMode(DIR, OUTPUT);
    Serial.begin(115200);
}

void advance(int8_t amount)
{
    Serial.printf("begin\n");
    digitalWrite(ENABLE, LOW);
    int base = 0;
    for (int x = 0; x < 50; x++) {
        base += analogRead(0);
        delay(1);
    }
    base /= 50;
    digitalWrite(DIR, amount > 0 ? HIGH : LOW);
    for (int x = 0; x < abs(amount); x++) {

        for (int rep = 0; rep < 46; rep++) {
            int min = 1023;
            int max = 0;
            int cnt = 0;
            int avg = 0;

            digitalWrite(STEP, LOW);

            for (int y = 0; y < 16; y++) {
                digitalWrite(STEP, HIGH);
                delay(1);
                //delayMicroseconds(300);
                int x = base - analogRead(0);
                // if (x > 200) {
                //     Serial.print("!");
                // } else if (x > 150) {
                //     Serial.print("*");
                // } else if (x > 100) {
                //     Serial.print("-");
                // } else {
                //     Serial.print(".");
                // }
                if (x != 0) {
                    avg += x;
                    cnt++;
                }
                if (x < min) {
                    min = x;
                }
                if (x > max) {
                    max = x;
                }
            }
            if (max-min > 10) {
                Serial.printf("[rep=%d,min=%d,delta=%d,avg=%d], ", rep, min, max - min, (avg / cnt) - min);
            }
            if (rep > 20 && (((avg/cnt) - min) > 40 || (max - min) > 80)) {
                Serial.printf("\ndone after %d\n", rep);
                goto exit;
            }
        }
        Serial.printf("\ndone after all\n");
    }

    exit:
    delay(50);
    digitalWrite(ENABLE, HIGH);
}

void loop()
{
    static uint16_t min = 0;
    delay(1000);
    advance(1);
    min++;
    Serial.printf("%02d:%02d:\n\n", ((min / 60) % 24), min % 60);
}
