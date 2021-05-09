#include <Arduino.h>

void setup()
{
    pinMode(GPIO_NUM_12, OUTPUT);
    pinMode(GPIO_NUM_14, OUTPUT);
    pinMode(GPIO_NUM_27, OUTPUT);
    pinMode(GPIO_NUM_26, OUTPUT);
}

typedef struct { 
    bool a1;
    bool a2;
    bool b1;
    bool b2;
} outputs_t;

outputs_t steps[] = {
    { true, true, true, false},
    { true, false, true, false},
    { true, false, true, true},
    { true, false, false, true},
    { true, true, false, true},
    { false, true, false, true},
    { false, true, true, true},
    { false, true, true, false }
};


int8_t phase = 1;

void advance(int8_t amount) {
    digitalWrite(GPIO_NUM_12, steps[phase].a1);
    digitalWrite(GPIO_NUM_14, steps[phase].a2);
    digitalWrite(GPIO_NUM_27, steps[phase].b1);
    digitalWrite(GPIO_NUM_26, steps[phase].b2);

    delay(100);
    digitalWrite(GPIO_NUM_12, LOW);
    digitalWrite(GPIO_NUM_14, LOW);
    digitalWrite(GPIO_NUM_27, LOW);
    digitalWrite(GPIO_NUM_26, LOW);

    delay(100);

    phase+=amount;
    while (phase < 0) { phase += 8;}
    if (phase >= 8) { phase = phase % 8; }
}
 

void loop()
{
    delay(1500);
    advance(-2);
    advance(-2);
}


