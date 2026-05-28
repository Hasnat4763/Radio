#include <Wire.h>
#include <EEPROM.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SI4735.h>
#include <Encoder.h>
#include <patch_init.h>

#define sda_ic            26
#define scl_ic            27
#define rst_ic            14
#define ant_switch        11
#define sda_oled          12
#define scl_oled          13
#define batt_read         15
#define headphone_enable  28
#define speaker_enable    29
#define encoder_sw         5
#define encoder_1          9
#define encoder_2         10

#define COL1               3
#define COL2               4
#define ROW1               6
#define ROW2               7
#define ROW3               8

#define KEY_VOL       0
#define KEY_AUDIO     1
#define KEY_AGC       2
#define KEY_BW        3
#define KEY_BFO       4
#define KEY_SEEK      5
#define KEY_COUNT     6

#define FM_BAND_TYPE      0
#define MW_BAND_TYPE      1
#define SW_BAND_TYPE      2
#define LW_BAND_TYPE      3
#define FM                0
#define LSB               1
#define USB               2
#define AM                3
#define DEFAULT_VOL      35
#define LONG_PRESS_MS   600
#define DEBOUNCE_MS      50
#define RSSI_INTERVAL  1800
#define OLED_ADDR      0x3C

float   battVoltage    = 0.0;
uint8_t battPercent    = 0;
unsigned long lastBattTime = 0;
#define BATT_INTERVAL  10000
#define BATT_R1        100000
#define BATT_R2        100000
#define BATT_MAX       4.2
#define BATT_MIN       3.0
#define ADC_RES        4095.0
#define ADC_VREF       3.3

const uint16_t size_content = sizeof(ssb_patch_content);

const char *bandwidthSSB[] = {"1.2", "2.2", "3.0", "4.0", "0.5", "1.0"};
const char *bandwidthAM[]  = {"6",   "4",   "3",   "2",   "1",   "1.8", "2.5"};
uint8_t bwIdxSSB = 2;
uint8_t bwIdxAM  = 1;

typedef struct {
  uint8_t  bandType;
  uint16_t minimumFreq;
  uint16_t maximumFreq;
  uint16_t currentFreq;
  uint16_t currentStep;
} Band;

Band band[] = {
  {FM_BAND_TYPE,  8400, 10800, 10390, 10},
  {LW_BAND_TYPE,   100,   510,   300,  1},
  {MW_BAND_TYPE,   520,  1720,   810, 10},
  {SW_BAND_TYPE,  1800,  3500,  1900,  1},
  {SW_BAND_TYPE,  3500,  4500,  3700,  1},
  {SW_BAND_TYPE,  4500,  5500,  4850,  5},
  {SW_BAND_TYPE,  5600,  6300,  6000,  5},
  {SW_BAND_TYPE,  6800,  7800,  7200,  5},
  {SW_BAND_TYPE,  9200, 10000,  9600,  5},
  {SW_BAND_TYPE, 10000, 11000, 10100,  1},
  {SW_BAND_TYPE, 11200, 12500, 11940,  5},
  {SW_BAND_TYPE, 13400, 13900, 13600,  5},
  {SW_BAND_TYPE, 14000, 14500, 14200,  1},
  {SW_BAND_TYPE, 15000, 15900, 15300,  5},
  {SW_BAND_TYPE, 17200, 17900, 17600,  5},
  {SW_BAND_TYPE, 18000, 18300, 18100,  1},
  {SW_BAND_TYPE, 21000, 21900, 21200,  1},
  {SW_BAND_TYPE, 24890, 26200, 24940,  1},
  {SW_BAND_TYPE, 26200, 27900, 27500,  1},
  {SW_BAND_TYPE, 28000, 30000, 28400,  1}
};
const int lastBand = (sizeof(band) / sizeof(Band)) - 1;

const char *bandModeDesc[] = {"FM", "LSB", "USB", "AM"};
uint8_t  currentMode  = FM;
uint8_t  volume       = DEFAULT_VOL;
uint8_t  rssi         = 0;
int      bandIdx      = 0;
int      currentBFO   = 0;
int      previousBFO  = 0;
uint16_t currentFreq;
uint16_t previousFreq;
uint16_t currentStep  = 10;
bool     ssbLoaded    = false;
bool     disableAgc   = true;
bool     isMuted      = false;

enum AudioOutput { OUTPUT_SPEAKER, OUTPUT_HEADPHONE };
AudioOutput currentOutput = OUTPUT_SPEAKER;

bool     keyState[KEY_COUNT]     = {false};
bool     keyHeld[KEY_COUNT]      = {false};
bool     keyWasHeld[KEY_COUNT]   = {false};
unsigned long keyPressTime[KEY_COUNT] = {0};
unsigned long lastKeyEvent[KEY_COUNT] = {0};

const uint8_t rowPins[] = {ROW1, ROW2, ROW3};
const uint8_t colPins[] = {COL1, COL2};

long encoderOldPos = -999;
int  encoderDelta  = 0;
unsigned long lastEncClick = 0;

unsigned long lastRSSITime = 0;

Adafruit_SSD1306 display(128, 64, &Wire, -1);
SI4735           si4735;
Encoder          knob(encoder_1, encoder_2);

void scanMatrix() {
  unsigned long now = millis();

  for (int r = 0; r < 3; r++) {
    for (int i = 0; i < 3; i++)
      pinMode(rowPins[i], (i == r) ? OUTPUT : INPUT);
    digitalWrite(rowPins[r], LOW);
    delayMicroseconds(10);

    for (int c = 0; c < 2; c++) {
      int keyIdx = r * 2 + c;
      bool pressed = (digitalRead(colPins[c]) == LOW);

      if (pressed && !keyState[keyIdx]) {
        if (now - lastKeyEvent[keyIdx] > DEBOUNCE_MS) {
          keyState[keyIdx]    = true;
          keyPressTime[keyIdx] = now;
          lastKeyEvent[keyIdx] = now;
        }
      } else if (!pressed && keyState[keyIdx]) {
        keyState[keyIdx] = false;
        lastKeyEvent[keyIdx] = now;

        if (!keyWasHeld[keyIdx]) {
          onKeyShortPress(keyIdx);
        }
        keyHeld[keyIdx]    = false;
        keyWasHeld[keyIdx] = false;
        keyPressTime[keyIdx] = 0;
      }

      if (keyState[keyIdx] && !keyHeld[keyIdx]) {
        if (now - keyPressTime[keyIdx] > LONG_PRESS_MS) {
          keyHeld[keyIdx] = true;
        }
      }
    }
  }

  for (int i = 0; i < 3; i++)
    pinMode(rowPins[i], INPUT_PULLUP);
}

void onKeyShortPress(int key) {
  switch (key) {
    case KEY_VOL:
      isMuted = !isMuted;
      si4735.setVolume(isMuted ? 0 : volume);
      showStatus();
      break;

    case KEY_AUDIO:
      toggleAudio();
      showStatus();
      break;

    case KEY_AGC:
      disableAgc = !disableAgc;
      si4735.setAutomaticGainControl(disableAgc, 1);
      showStatus();
      break;

    case KEY_BW:
      applyNextBandwidth(1);
      showStatus();
      break;

    case KEY_BFO:
      currentBFO = 0;
      si4735.setSSBBfo(0);
      showStatus();
      break;

    case KEY_SEEK:
      si4735.seekStationUp();
      delay(300);
      currentFreq = si4735.getCurrentFrequency();
      showStatus();
      break;
  }
}

void applyNextBandwidth(int delta) {
  if (currentMode == LSB || currentMode == USB) {
    bwIdxSSB = (bwIdxSSB + delta + 6) % 6;
    si4735.setSSBAudioBandwidth(bwIdxSSB);
    si4735.setSSBSidebandCutoffFilter(
      (bwIdxSSB == 0 || bwIdxSSB == 4 || bwIdxSSB == 5) ? 0 : 1);
  } else if (currentMode == AM) {
    bwIdxAM = (bwIdxAM + delta + 7) % 7;
    si4735.setBandwidth(bwIdxAM, 1);
  }
}

void setAntenna() {
  if (band[bandIdx].bandType == FM_BAND_TYPE ||
      band[bandIdx].bandType == SW_BAND_TYPE) {
    digitalWrite(ant_switch, HIGH);
  } else {
    digitalWrite(ant_switch, LOW);
  }
}

void setSpeaker() {
  digitalWrite(headphone_enable, LOW);
  delay(20);
  digitalWrite(speaker_enable, HIGH);
  currentOutput = OUTPUT_SPEAKER;
}

void setHeadphone() {
  digitalWrite(speaker_enable, LOW);
  delay(20);
  digitalWrite(headphone_enable, HIGH);
  currentOutput = OUTPUT_HEADPHONE;
}

void toggleAudio() {
  (currentOutput == OUTPUT_SPEAKER) ? setHeadphone() : setSpeaker();
}

void loadSSB() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 28);
  display.print(" Loading SSB patch..");
  display.display();
  si4735.reset();
  si4735.queryLibraryId();
  si4735.patchPowerUp();
  delay(50);
  si4735.setI2CFastMode();
  si4735.downloadPatch(ssb_patch_content, size_content);
  si4735.setI2CStandardMode();
  si4735.setSSBConfig(bwIdxSSB, 1, 0, 0, 0, 1);
  delay(25);
  ssbLoaded = true;
}

void useBand() {
  if (band[bandIdx].bandType == FM_BAND_TYPE) {
    currentMode = FM;
    si4735.setTuneFrequencyAntennaCapacitor(0);
    si4735.setFM(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq,
                 band[bandIdx].currentFreq, band[bandIdx].currentStep);
    ssbLoaded = false;
  } else {
    si4735.setTuneFrequencyAntennaCapacitor(
      (band[bandIdx].bandType == MW_BAND_TYPE ||
       band[bandIdx].bandType == LW_BAND_TYPE) ? 0 : 1);

    if (ssbLoaded) {
      si4735.setSSB(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq,
                    band[bandIdx].currentFreq, band[bandIdx].currentStep, currentMode);
      si4735.setSSBAutomaticVolumeControl(1);
      si4735.setSsbSoftMuteMaxAttenuation(0);
    } else {
      currentMode = AM;
      si4735.setAM(band[bandIdx].minimumFreq, band[bandIdx].maximumFreq,
                   band[bandIdx].currentFreq, band[bandIdx].currentStep);
      si4735.setAutomaticGainControl(1, 0);
      si4735.setAmSoftMuteMaxAttenuation(0);
    }
  }
  delay(100);
  currentFreq = previousFreq = band[bandIdx].currentFreq;
  currentStep = band[bandIdx].currentStep;
  si4735.setVolume(volume);
  setAntenna();
  showStatus();
}

void nextBand() {
  band[bandIdx].currentFreq = currentFreq;
  band[bandIdx].currentStep = currentStep;
  bandIdx = (bandIdx < lastBand) ? bandIdx + 1 : 0;
  useBand();
}

void cycleMode() {
  if (band[bandIdx].bandType == FM_BAND_TYPE) return;
  if (currentMode == AM) {
    loadSSB();
    currentMode = LSB;
  } else if (currentMode == LSB) {
    currentMode = USB;
  } else if (currentMode == USB) {
    currentMode = AM;
    ssbLoaded = false;
    currentBFO = 0;
  }
  band[bandIdx].currentFreq = currentFreq;
  band[bandIdx].currentStep = currentStep;
  useBand();
}

void showFrequency() {
  display.fillRect(0, 14, 128, 30, BLACK);
  display.setTextColor(WHITE);
  display.setTextSize(3);
  display.setCursor(0, 16);

  if (band[bandIdx].bandType == FM_BAND_TYPE) {
    display.print(currentFreq / 100);
    display.print(".");
    display.print((currentFreq % 100) / 10);
  } else {
    display.print(currentFreq);
  }

  display.setTextSize(1);
  display.setCursor(100, 16);
  display.print(band[bandIdx].bandType == FM_BAND_TYPE ? "MHz" : "kHz");
}

void showStatus() {
  display.clearDisplay();

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(bandModeDesc[currentMode]);

  display.setCursor(25, 0);
  display.print("B:");
  display.print(bandIdx);

  if (currentMode == FM) {
    display.setCursor(50, 0);
    display.print(si4735.getCurrentPilot() ? "ST" : "MO");
  }

  display.setCursor(68, 0);
  display.print(disableAgc ? "   " : "AGC");

  display.setCursor(90, 0);
  display.print(currentOutput == OUTPUT_SPEAKER ? "SP" : "HP");

  display.setCursor(100, 0);
  display.print(battPercent);
  display.print("%");

  showFrequency();

  display.setTextSize(1);

  display.setCursor(0, 47);
  display.print("SIG:");
  int bars = constrain((rssi / 10) + 1, 0, 8);
  for (int i = 0; i < bars; i++) display.print("=");

  display.setCursor(0, 56);
  display.print("STP:");
  display.print(currentStep);
  display.print("k");

  display.setCursor(55, 56);
  if (isMuted) {
    display.print("MUTED");
  } else {
    display.print("VOL:");
    display.print(volume);
  }

  display.setCursor(55, 47);
  display.print("BW:");
  if (currentMode == LSB || currentMode == USB)
    display.print(bandwidthSSB[bwIdxSSB]);
  else if (currentMode == AM)
    display.print(bandwidthAM[bwIdxAM]);
  else
    display.print("--");

  if (currentMode == LSB || currentMode == USB) {
    display.setCursor(90, 56);
    display.print("B:");
    display.print(currentBFO);
  }

  display.display();
}

void handleEncoder() {
  long newPos = knob.read() / 4;
  if (newPos == encoderOldPos) return;
  encoderDelta  = (newPos > encoderOldPos) ? 1 : -1;
  encoderOldPos = newPos;

  if (keyHeld[KEY_VOL]) {
    keyWasHeld[KEY_VOL] = true;
    volume = constrain(volume + encoderDelta, 0, 63);
    if (isMuted) { isMuted = false; }
    si4735.setVolume(volume);
    showStatus();
    return;
  }

  if (keyHeld[KEY_BFO]) {
    keyWasHeld[KEY_BFO] = true;
    if (currentMode == LSB || currentMode == USB) {
      currentBFO += encoderDelta * 25;
      si4735.setSSBBfo(currentBFO);
      showStatus();
    }
    return;
  }

  if (keyHeld[KEY_BW]) {
    keyWasHeld[KEY_BW] = true;
    applyNextBandwidth(encoderDelta);
    showStatus();
    return;
  }

  if (encoderDelta == 1) si4735.frequencyUp();
  else                   si4735.frequencyDown();
  currentFreq = si4735.getCurrentFrequency();
  showFrequency();
  display.display();
}

void handleEncoderClick() {
  unsigned long now = millis();
  if (digitalRead(encoder_sw) == LOW && (now - lastEncClick) > 300) {
    lastEncClick = now;
    if (keyHeld[KEY_AGC]) {
      keyWasHeld[KEY_AGC] = true;
      cycleMode();
    } else {
      nextBand();
    }
  }
}

void readBattery() {
  int raw = analogRead(batt_read);
  float adcVoltage = (raw / ADC_RES) * ADC_VREF;
  battVoltage = adcVoltage * ((BATT_R1 + BATT_R2) / (float)BATT_R2);
  battVoltage = constrain(battVoltage, BATT_MIN, BATT_MAX);
  battPercent = (uint8_t)(((battVoltage - BATT_MIN) / (BATT_MAX - BATT_MIN)) * 100.0);
}

void setup() {
  Serial.begin(115200);

  Wire.setSDA(sda_oled);
  Wire.setSCL(scl_oled);
  Wire.begin();

  Wire1.setSDA(sda_ic);
  Wire1.setSCL(scl_ic);
  Wire1.begin();
  Wire1.setClock(100000);

  pinMode(batt_read, INPUT);

  analogReadResolution(12);
  readBattery();
  pinMode(COL1, INPUT_PULLUP);
  pinMode(COL2, INPUT_PULLUP);
  for (int i = 0; i < 3; i++)
    pinMode(rowPins[i], INPUT_PULLUP);
  pinMode(encoder_sw,       INPUT_PULLUP);
  pinMode(speaker_enable,   OUTPUT);
  pinMode(headphone_enable, OUTPUT);
  pinMode(ant_switch,       OUTPUT);

  setSpeaker();
  setAntenna();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("SSD1306 not found"));
    for (;;);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.println("Akash");
  display.setTextSize(1);
  display.setCursor(10, 35);
  display.println("Chowa DX Receiver");
  display.display();
  delay(2000);

  si4735.setDeviceI2CAddress(0x11);
  si4735.setup(rst_ic, -1, SI473X_ANALOG_AUDIO, Wire1);
  si4735.setFM(band[0].minimumFreq, band[0].maximumFreq,
               band[0].currentFreq, band[0].currentStep);
  currentFreq = previousFreq = si4735.getCurrentFrequency();
  si4735.setVolume(volume);
  showStatus();
}

void loop() {
  scanMatrix();
  handleEncoder();
  handleEncoderClick();

  if (currentFreq != previousFreq) {
    previousFreq = currentFreq;
    band[bandIdx].currentFreq = currentFreq;
  }

  if (currentBFO != previousBFO) {
    previousBFO = currentBFO;
    si4735.setSSBBfo(currentBFO);
  }

  if (millis() - lastRSSITime > RSSI_INTERVAL) {
    si4735.getCurrentReceivedSignalQuality();
    rssi = si4735.getCurrentRSSI();
    lastRSSITime = millis();
    
    showStatus();
  }
  if (millis() - lastBattTime > BATT_INTERVAL) {
    readBattery();
    lastBattTime = millis();
  }

  delay(10);
}