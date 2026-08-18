#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <AnimatedGIF.h>
#include "animation.h"

// ===== I2C OLED Configuration =====
#define I2C_SDA 8
#define I2C_SCL 9
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

// ===== MPU6050 Configuration =====
#define MPU6050_ADDR 0x68

// ===== Sound Configuration =====
#define SPEAKER_PIN 3

// ===== Emotion Timing Configuration =====
#define SHAKE_COOLDOWN 100
#define EMOTION_INTERVAL 10000
#define ANGRY_DURATION 3000

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
AnimatedGIF gif;

int16_t accX, accY, accZ;
float gForceX, gForceY, gForceZ;

enum EmotionState {
  NORMAL,
  ANGRY
};

EmotionState currentEmotion = NORMAL;
unsigned long lastShakeTime = 0;
unsigned long lastShakeCheckTime = 0;
unsigned long lastEmotionChange = 0;
unsigned long angryStartTime = 0;
bool isShaking = false;
bool gifPlaying = false;

struct SoundNote {
  int frequency;
  int duration;
};

const int ANGRY_MELODY_LENGTH = 10;
SoundNote angryMelody[ANGRY_MELODY_LENGTH] = {
  {1000, 200}, {1000, 200}, {800, 150}, {1200, 150},
  {600, 300}, {1400, 100}, {1400, 100}, {500, 400},
  {1000, 200}, {0, 500}
};

bool soundPlaying = false;
int currentNoteIndex = 0;
unsigned long noteStartTime = 0;
int currentFrequency = 0;
int currentDuration = 0;

void setupMPU6050() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission(true);
  
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x1C);
  Wire.write(0x00);
  Wire.endTransmission(true);
  delay(100);
}

void readMPU6050() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU6050_ADDR, 6, true);
  
  if (Wire.available() >= 6) {
    accX = Wire.read() << 8 | Wire.read();
    accY = Wire.read() << 8 | Wire.read();
    accZ = Wire.read() << 8 | Wire.read();
    
    gForceX = accX / 16384.0;
    gForceY = accY / 16384.0;
    gForceZ = accZ / 16384.0;
  }
}

bool isShakeDetected() {
  readMPU6050();
  float totalAcc = abs(gForceX) + abs(gForceY) + abs(gForceZ);
  return (totalAcc > 2.0);
}

void startSound() {
  if (soundPlaying) {
    ledcDetach(SPEAKER_PIN);
    pinMode(SPEAKER_PIN, OUTPUT);
    digitalWrite(SPEAKER_PIN, LOW);
  }
  
  soundPlaying = true;
  currentNoteIndex = 0;
  noteStartTime = millis();
  
  currentFrequency = angryMelody[0].frequency;
  currentDuration = angryMelody[0].duration;
  
  if (currentFrequency > 0) {
    ledcAttach(SPEAKER_PIN, currentFrequency, 8);
    ledcWrite(SPEAKER_PIN, 128);
  }
}

void updateSound() {
  if (!soundPlaying) return;
  
  unsigned long now = millis();
  
  if (now - noteStartTime >= currentDuration) {
    currentNoteIndex++;
    
    if (currentNoteIndex >= ANGRY_MELODY_LENGTH) {
      soundPlaying = false;
      ledcDetach(SPEAKER_PIN);
      pinMode(SPEAKER_PIN, OUTPUT);
      digitalWrite(SPEAKER_PIN, LOW);
      return;
    }
    
    currentFrequency = angryMelody[currentNoteIndex].frequency;
    currentDuration = angryMelody[currentNoteIndex].duration;
    noteStartTime = now;
    
    if (currentFrequency > 0) {
      ledcAttach(SPEAKER_PIN, currentFrequency, 8);
      ledcWrite(SPEAKER_PIN, 128);
    } else {
      ledcDetach(SPEAKER_PIN);
      pinMode(SPEAKER_PIN, OUTPUT);
      digitalWrite(SPEAKER_PIN, LOW);
    }
  }
}

void playToneBlocking(int frequency, int duration) {
  if (frequency == 0) {
    delay(duration);
    return;
  }
  
  ledcAttach(SPEAKER_PIN, frequency, 8);
  ledcWrite(SPEAKER_PIN, 128);
  delay(duration);
  ledcDetach(SPEAKER_PIN);
  pinMode(SPEAKER_PIN, OUTPUT);
  digitalWrite(SPEAKER_PIN, LOW);
  delay(50);
}

void playStartupMelody() {
  int melody[] = {262, 330, 392, 523};
  int durations[] = {300, 300, 300, 600};
  
  for (int i = 0; i < 4; i++) {
    playToneBlocking(melody[i], durations[i]);
    delay(100);
  }
}

void playAngrySound() {
  startSound();
}

void transformCoordinates(int &x, int &y, int rotation) {
  int oldX = x, oldY = y;
  switch(rotation) {
    case 2:
      x = SCREEN_WIDTH - 1 - oldX;
      y = SCREEN_HEIGHT - 1 - oldY;
      break;
  }
}

void GIFDraw(GIFDRAW* pDraw) {
  uint8_t* s;
  int x, y, iWidth;
  static uint8_t ucPalette[256];
  
  uint8_t* buffer = display.getBuffer();
  
  if (pDraw->y == 0) {
    for (x = 0; x < 256; x++) {
      uint16_t usColor = pDraw->pPalette[x];
      if (usColor == 0) {
        ucPalette[x] = 0;
        continue;
      }
      int r = (usColor & 0xF800) >> 11;
      int g = (usColor & 0x07E0) >> 5;
      int b = usColor & 0x001F;
      int brightness = (r * 299 + g * 587 + b * 114) / 1000;
      ucPalette[x] = (brightness > 15) ? 1 : 0;
    }
  }
  
  y = pDraw->iY + pDraw->y;
  iWidth = pDraw->iWidth;
  if (iWidth > SCREEN_WIDTH) iWidth = SCREEN_WIDTH;
  
  s = pDraw->pPixels;
  
  for (x = 0; x < iWidth; x++) {
    uint8_t c;
    if (pDraw->ucHasTransparency) {
      c = *s++;
      if (c == pDraw->ucTransparent) continue;
    } else {
      c = pDraw->pPixels[x];
    }
    
    int origX = pDraw->iX + x;
    int origY = y;
    
    int rotX = origX, rotY = origY;
    transformCoordinates(rotX, rotY, 2);
    
    if (rotX >= 0 && rotX < SCREEN_WIDTH && rotY >= 0 && rotY < SCREEN_HEIGHT) {
      int byteIdx = rotX + (rotY / 8) * SCREEN_WIDTH;
      int bitIdx = rotY % 8;
      
      if (ucPalette[c]) {
        buffer[byteIdx] |= (1 << bitIdx);
      } else {
        buffer[byteIdx] &= ~(1 << bitIdx);
      }
    }
  }
  
  if (pDraw->y == pDraw->iHeight - 1) {
    display.display();
  }
}

void startGIF(uint8_t* gifData, int size) {
  if (gifPlaying) {
    gif.close();
  }
  display.clearDisplay();
  display.display();
  delay(10);
  
  if (gif.open(gifData, size, GIFDraw)) {
    gifPlaying = true;
  }
}

void updateGIF() {
  if (gifPlaying) {
    if (!gif.playFrame(true, NULL)) {
      gif.close();
      gifPlaying = false;
    }
  }
}

void setRandomNormalEmotion() {
  int r = random(1, 33);
  
  switch(r) {
    case 1: startGIF((uint8_t*)_1, sizeof(_1)); break;
    case 2: startGIF((uint8_t*)_2, sizeof(_2)); break;
    case 3: startGIF((uint8_t*)_3, sizeof(_3)); break;
    case 4: startGIF((uint8_t*)_4, sizeof(_4)); break;
    case 5: startGIF((uint8_t*)_5, sizeof(_5)); break;
    case 6: startGIF((uint8_t*)_6, sizeof(_6)); break;
    case 7: startGIF((uint8_t*)_7, sizeof(_7)); break;
    case 8: startGIF((uint8_t*)_8, sizeof(_8)); break;
    case 9: startGIF((uint8_t*)_9, sizeof(_9)); break;
    case 10: startGIF((uint8_t*)_10, sizeof(_10)); break;
    case 11: startGIF((uint8_t*)_11, sizeof(_11)); break;
    case 12: startGIF((uint8_t*)_12, sizeof(_12)); break;
    case 13: startGIF((uint8_t*)_13, sizeof(_13)); break;
    case 14: startGIF((uint8_t*)_14, sizeof(_14)); break;
    case 15: startGIF((uint8_t*)_15, sizeof(_15)); break;
    case 16: startGIF((uint8_t*)_16, sizeof(_16)); break;
    case 17: startGIF((uint8_t*)_17, sizeof(_17)); break;
    case 18: startGIF((uint8_t*)_18, sizeof(_18)); break;
    case 19: startGIF((uint8_t*)_19, sizeof(_19)); break;
    case 20: startGIF((uint8_t*)_20, sizeof(_20)); break;
    case 21: startGIF((uint8_t*)_21, sizeof(_21)); break;
    case 22: startGIF((uint8_t*)_22, sizeof(_22)); break;
    case 23: startGIF((uint8_t*)_23, sizeof(_23)); break;
    case 24: startGIF((uint8_t*)_24, sizeof(_24)); break;
    case 25: startGIF((uint8_t*)_25, sizeof(_25)); break;
    case 26: startGIF((uint8_t*)_26, sizeof(_26)); break;
    case 27: startGIF((uint8_t*)_27, sizeof(_27)); break;
    case 28: startGIF((uint8_t*)_28, sizeof(_28)); break;
    case 29: startGIF((uint8_t*)_29, sizeof(_29)); break;
    case 30: startGIF((uint8_t*)_30, sizeof(_30)); break;
    case 31: startGIF((uint8_t*)_31, sizeof(_31)); break;
    case 32: startGIF((uint8_t*)_32, sizeof(_32)); break;
    default: startGIF((uint8_t*)_1, sizeof(_1)); break;
  }
}

void setup() {
  pinMode(SPEAKER_PIN, OUTPUT);
  
  Wire.begin(I2C_SDA, I2C_SCL);
  setupMPU6050();
  
  display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  display.setRotation(2);
  
  gif.begin(LITTLE_ENDIAN_PIXELS);
  
  startGIF((uint8_t*)intro, sizeof(intro));
  
  while (gifPlaying) {
    updateGIF();
    delay(10);
  }
  
  delay(500);
  
  currentEmotion = NORMAL;
  lastEmotionChange = millis();
  setRandomNormalEmotion();
  playStartupMelody();
}

void loop() {
  unsigned long now = millis();
  
  if (now - lastShakeCheckTime > SHAKE_COOLDOWN) {
    isShaking = isShakeDetected();
    lastShakeCheckTime = now;
    if (isShaking) lastShakeTime = now;
  }
  
  updateGIF();
  updateSound();
  
  switch(currentEmotion) {
    case NORMAL:
      if (isShaking) {
        currentEmotion = ANGRY;
        playAngrySound();
        startGIF((uint8_t*)_9, sizeof(_9));
        angryStartTime = now;
      }
      else if (!gifPlaying && (now - lastEmotionChange > EMOTION_INTERVAL)) {
        lastEmotionChange = now;
        setRandomNormalEmotion();
      }
      break;
      
    case ANGRY:
      if (!gifPlaying || (now - angryStartTime > ANGRY_DURATION)) {
        currentEmotion = NORMAL;
        lastEmotionChange = now;
        setRandomNormalEmotion();
      }
      break;
  }
  
  delay(10);
}
