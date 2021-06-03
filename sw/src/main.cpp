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
    delay(50);
    digitalWrite(DIR, amount > 0 ? HIGH : LOW);
    for (int x = 0; x < abs(amount); x++) {
        digitalWrite(STEP, LOW);

        for (int y = 0; y < 10; y++) {
            digitalWrite(STEP, HIGH);
            int x = analogRead(0);
            if (x < 800)
                Serial.printf("got %d\n", x);
            delay(1);
        }
    }

    delay(50);
    digitalWrite(ENABLE, HIGH);
    Serial.printf("end\n");
}

void loop()
{
    delay(1000);
    advance(20);
}
