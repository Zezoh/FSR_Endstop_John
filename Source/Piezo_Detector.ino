// Pin layout copied from pins.arduino.h for convenience, and then added how the pins are
// connected on the circuit board.
//
// ATMEL ATTINY861
//
//                             +-\/-+
// MOSI           (D  9) PB0  1|    |20  PA0 (D  0)         PIEZO1
// MISO          *(D  8) PB1  2|    |19  PA1 (D  1)         PIEZO2
// SCK            (D  7) PB2  3|    |18  PA2 (D  2) INT1    PIEZO3
// NC/IO3        *(D  6) PB3  4|    |17  PA3 (D 14)
//                       VCC  5|    |16  AGND
//                       GND  6|    |15  AVCC
// SN1/IO2        (D  5) PB4  7|    |14  PA4 (D 10)         LED4
// SN2/IO1       *(D  4) PB5  8|    |13  PA5 (D 11)         LED3
// SIG       INT0 (D  3) PB6  9|    |12  PA6 (D 12)         LED2
// RST            (D 15) PB7 10|    |11  PA7 (D 13)         LED1
//                             +----+
//

#define VERSION     2

// Define the pins that have LEDs on them. We have one LED for each of the three piezo sensors to
// indicate when each sensor is triggered. And one power/end stop LED that is on until any sensor
// is triggered.
#define LED1        13
#define LED2        12
#define LED3        11

#define LEDTRIGGER  10
#define ENDSTOP     3

// Define the pins used for the analog inputs that have the piezo sensors attached.
#define PIEZO1      A0
#define PIEZO2      A1
#define PIEZO3      A2

// Jumper pins, which are labels IO3, IO2, and IO1 on the revision 1.1 boards. On the
// revision 1.2 boards, these are labeled NC, SN1, and SN2
#define NC_PIN      6
#define SEN1        5
#define SEN2        4

// The end stop output
#define TRIGGERED   LOW
#define UNTRIGGERED HIGH

//  SEN1    SEN2    Threshold multiplier
//  ----    ----    --------------------
//   0       0          1.20
//   0       1          1.15
//   1       0          1.05
//   1       1          1.08
// 0 = LOW (jumper installed), 1 = HIGH (jumper open) with INPUT_PULLUP.
// Values are listed by jumper-state table order above (not by numeric magnitude).
const float THRESHOLD_BOTH_INSTALLED  = 1.20;
const float THRESHOLD_SEN1_ONLY       = 1.15;
const float THRESHOLD_SEN2_ONLY       = 1.05;
const float THRESHOLD_BOTH_OPEN       = 1.08;

uint8_t piezoLeds[] = { LED1, LED2, LED3 };      // Pins for each of the LEDs next to the sensor inputs
uint8_t piezoPins[] = { PIEZO1, PIEZO2, PIEZO3 };// Pins for each sensor analog input

#define SHORT_SIZE 8
#define LONG_SIZE 16
#define LONG_INTERVAL (2000 / LONG_SIZE)
#define ADC_MAX_VALUE 1023

unsigned long lastLongSampleTime[3];        // Last time in millis that we captured a long-term sample
uint16_t longSamples[3][LONG_SIZE];         // Used to keep a long-term average
uint8_t longIndex[3] = {0, 0, 0};           // Index of the last long-term sample
uint16_t longAverage[3] = {0, 0, 0};

uint16_t shortSamples[3][SHORT_SIZE];       // Used to create an average of the most recent samples
uint8_t averageIndex[3] = {0, 0, 0};

//
// Set the triggered state based on the state of one piezo sensor
//
void SetOutput(uint8_t piezo, bool state)
{
    static bool triggered[3] = {false};     // Keeps track of current sensor trigger state, initially not triggered

    // Turns on the sensor LED when that sensor is triggered
    triggered[piezo] = state;
    digitalWrite(piezoLeds[piezo], state ? HIGH : LOW);

    // See if any of the sensors are currently triggered
    bool any = false;
    for (uint8_t i = 0; i < 3; i++)
    {
        any |= triggered[i];
    }

    digitalWrite(LEDTRIGGER, any ? LOW : HIGH);

    // For the end stop, we need to check the NC jumper to see if we need to invert
    // the output.
    uint8_t ncPinState = digitalRead(NC_PIN);
    if (ncPinState == HIGH)
    {
        // No jumper installed, so use Normally Closed
        digitalWrite(ENDSTOP, any ? LOW : HIGH);
    }
    else
    {
        // Jumper installed, so use Normally Open
        digitalWrite(ENDSTOP, any ? HIGH : LOW);
    }
}

void InitValues()
{
    for (uint8_t piezo = 0; piezo < 3; piezo++)
    {
        for (uint8_t i = 0; i < SHORT_SIZE; i++)
            shortSamples[piezo][i] = 0;

        for (uint8_t i = 0; i < LONG_SIZE; i++)
            longSamples[piezo][i] = 0;
    }

    for (uint8_t piezo = 0; piezo < 3; piezo++)
        lastLongSampleTime[piezo] = millis();
}

void InitializeJumpers()
{
    pinMode(NC_PIN, INPUT_PULLUP);
    pinMode(SEN1, INPUT_PULLUP);
    pinMode(SEN2, INPUT_PULLUP);
}

//
// Briefly turns on sensor LEDs during startup to indicate the version number of the
// firmware.
//
void BlinkVersion(uint8_t version)
{
    for (uint8_t i = 0; i < 3; i++)
    {
        digitalWrite(piezoLeds[i], (version & (1 << i)) ? HIGH : LOW);
    }
    delay(250);
    for (uint8_t i = 0; i < 3; i++)
    {
        digitalWrite(piezoLeds[i], LOW);
    }
}

//
// One-time setup for the various I/O ports
//
void setup()
{
    InitValues();

    for (uint8_t piezo = 0; piezo < 3; piezo++)
    {
        // Set the sensor LEDs for output and turn them off
        uint8_t pin = piezoLeds[piezo];
        pinMode(pin, OUTPUT);
        digitalWrite(pin, LOW);

        // Set the sensor lines for input
        pin = piezoPins[piezo];
        pinMode(pin, INPUT);
    }

    // Set the green combined LED to on so it acts as a power-on indicator. We'll turn it
    // off whenever we trigger the end stop.
    pinMode(LEDTRIGGER, OUTPUT);
    digitalWrite(LEDTRIGGER, HIGH);

    // Set the endstop pin to be an output that is set for NC
    pinMode(ENDSTOP, OUTPUT);

    // Set the jumpers to use the internal pull-up resistor and be for input
    InitializeJumpers();

    BlinkVersion(VERSION);
}

//
// Captures a new value once LONG_INTERVAL ms have passed since the last sample.
//
// Returns: The current long-range average
uint16_t UpdateLongSamples(uint8_t piezo, uint16_t avg)
{
    //
    // If enough time hasn't passed, just return the last value
    //
    unsigned long current = millis();
    if (current - lastLongSampleTime[piezo] <= LONG_INTERVAL)
    {
        return longAverage[piezo];
    }

    //
    // Update the long sample with the new value, and then update the long average
    //
    longSamples[piezo][longIndex[piezo]++] = avg;
    if (longIndex[piezo] >= LONG_SIZE)
    {
        longIndex[piezo] = 0;
    }

    uint32_t total = 0;
    for (uint8_t i = 0; i < LONG_SIZE; i++)
    {
        total += longSamples[piezo][i];
    }

    longAverage[piezo] = total / LONG_SIZE;

    lastLongSampleTime[piezo] += LONG_INTERVAL;
    return longAverage[piezo];
}

//
// Returns the current threshold to use, based on jumpers installed
//
inline float GetThreshold()
{
    // With INPUT_PULLUP, LOW means jumper installed and HIGH means no jumper installed.
    // Read jumpers each cycle so sensitivity can be adjusted without reflashing.
    uint8_t sen1PinState = digitalRead(SEN1);
    uint8_t sen2PinState = digitalRead(SEN2);

    if (sen1PinState == LOW)
    {
        return (sen2PinState == LOW) ? THRESHOLD_BOTH_INSTALLED : THRESHOLD_SEN1_ONLY;
    }

    return (sen2PinState == LOW) ? THRESHOLD_SEN2_ONLY : THRESHOLD_BOTH_OPEN;
}

//
// This method is called after every sample to see if the output trigger status should be changed.
// It will also update the short sample buffer, and it may update the long-term samples.
//
void CheckIfTriggered(uint8_t piezo, float thresholdMultiplier)
{
    //
    // Calculate the average of the most recent short-term samples
    //
    uint32_t total = 0;
    for (uint8_t i = 0; i < SHORT_SIZE; i++)
    {
        total += shortSamples[piezo][i];
    }
    uint16_t avg = total / SHORT_SIZE;

    uint16_t baseline = UpdateLongSamples(piezo, avg);
    float thresholdCandidate = thresholdMultiplier * baseline;
    if (thresholdCandidate > ADC_MAX_VALUE)
    {
        thresholdCandidate = ADC_MAX_VALUE;
    }
    uint16_t threshold = (uint16_t)thresholdCandidate;

    bool triggered = avg > threshold;
    SetOutput(piezo, triggered);
}

void loop()
{
    float thresholdMultiplier = GetThreshold();

    for (uint8_t piezo = 0; piezo < 3; piezo++)
    {
        uint16_t value = analogRead(piezoPins[piezo]);

        shortSamples[piezo][averageIndex[piezo]++] = value;
        if (averageIndex[piezo] >= SHORT_SIZE)
        {
            averageIndex[piezo] = 0;
        }
        CheckIfTriggered(piezo, thresholdMultiplier);
    }
}
