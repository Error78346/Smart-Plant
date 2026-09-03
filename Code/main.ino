#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <EEPROM.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define DHTPIN 5
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Hardware Pins
const int SENSOR_PIN  = A0;
const int RELAY_PIN   = 7;
const int ENCODER_CLK = 2;
const int ENCODER_DT  = 3;
const int ENCODER_SW  = 4;

// Plant Profiles
enum PlantType { FICUS, SUCCULENT, FERN };
PlantType currentPlant = FICUS;

// Struct to store per-plant settings in EEPROM
struct PlantSettings {
  int dryLimit;       // Raw ADC value to start watering (Higher = Drier)
  int wetLimit;       // Raw ADC value to stop watering (Lower = Wetter)
  int pulseTimeSec;   // How long the pump runs per burst (1 - 10 sec)
  int intervalSec;    // Rest time between pulses for soil absorption (5 - 60 sec)
};

// Default profiles if EEPROM is uninitialized
// Recalibrated to this sensor's real range: ~570 raw out of the soil/water (dry), ~220 raw fully submerged (wet)
PlantSettings plantProfiles[3] = {
  { 480, 300, 2, 10 }, // Ficus
  { 540, 350, 2, 15 }, // Succulent (tolerates driest, stops earliest)
  { 400, 260, 3, 8  }  // Fern (wants it wettest)
};

bool isWateringActive = false;
unsigned long lastPulseTime = 0;
bool pumpOn = false;             // Is the relay currently mid-pulse?
unsigned long pumpStartTime = 0; // When the current pulse started
unsigned long wateringStartTime = 0; // When the current watering CYCLE began
bool wateringFault = false;      // Set if a cycle runs too long without reaching wetLimit
const unsigned long MAX_WATERING_DURATION_MS = 10UL * 60UL * 1000UL; // 10 min safety cutoff

// System States
enum SystemState { MAIN_SCREENS, MENU_MODE, EDIT_PARAM };
SystemState currentState = MAIN_SCREENS;

int screenIndex = 0;
const int TOTAL_SCREENS = 5;

int menuIndex = 0;
const int TOTAL_MENU_ITEMS = 6; // Profile, Dry Limit, Wet Limit, Pulse Time, Rest Interval, Exit

volatile int encoderDelta = 0; // accumulated steps since last consumed by handleEncoder()
bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;

void encoderISR();
void handleEncoder();
void handleButton();
void updateDisplay();
void handleWateringLogic(int rawMoisture);
void drawPlantGraphic(int moisturePercent, float tempC);
void loadSettingsFromEEPROM();
void saveSettingsToEEPROM();

void setup() {
  Serial.begin(9600);
  dht.begin();
  //UNCOMMENT FOLLOWING LINE TO CLEAR EEPROM MEMORY
  EEPROM.write(0, 0);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // Relay OFF

  pinMode(ENCODER_CLK, INPUT_PULLUP);
  pinMode(ENCODER_DT, INPUT_PULLUP);
  pinMode(ENCODER_SW, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK), encoderISR, FALLING);

  loadSettingsFromEEPROM();

  Wire.begin();
  Wire.setClock(50000); // slower I2C = more tolerant of a noisy/long OLED wire run
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;);
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(15, 25);
  display.print(F("Smart Care System"));
  display.display();
  delay(1200);
}

void loop() {
  handleEncoder();
  handleButton();
  Serial.println("LOOP RAN!");
  int rawMoisture = analogRead(SENSOR_PIN);
  handleWateringLogic(rawMoisture);

  updateDisplay();
  delay(10);
}

// --- EEPROM Functions ---

void loadSettingsFromEEPROM() {
  // Signature byte at address 0 checks if EEPROM has been formatted
  if (EEPROM.read(0) == 42) {
    byte storedPlant = EEPROM.read(1);
    currentPlant = (storedPlant <= FERN) ? (PlantType)storedPlant : FICUS; // guard against corrupted/garbage EEPROM values
    int addr = 2;
    for (int i = 0; i < 3; i++) {
      EEPROM.get(addr, plantProfiles[i]);
      addr += sizeof(PlantSettings);
    }
  } else {
    // First setup: Write default settings to EEPROM
    saveSettingsToEEPROM();
  }
}

void saveSettingsToEEPROM() {
  EEPROM.update(0, 42); // Signature byte
  EEPROM.update(1, (byte)currentPlant);
  int addr = 2;
  for (int i = 0; i < 3; i++) {
    EEPROM.put(addr, plantProfiles[i]);
    addr += sizeof(PlantSettings);
  }
}

// --- Dynamic Watering Logic ---

void handleWateringLogic(int rawMoisture) {
  PlantSettings &active = plantProfiles[currentPlant];
  unsigned long now = millis();
  float tempC = dht.readTemperature(); // cheap: library returns a cached reading if <2s old
  Serial.println("WATERING LOGIC RAN!");
  // Start watering cycle when dry limit is breached
  if (rawMoisture >= active.dryLimit && !isWateringActive) {
    isWateringActive = true;
    lastPulseTime = 0;
    wateringStartTime = now;
    wateringFault = false; // give a fresh cycle the benefit of the doubt
  }

  // Stop watering cycle once wet limit is reached
  if (rawMoisture <= active.wetLimit && isWateringActive) {
    isWateringActive = false;
    pumpOn = false;
    wateringFault = false;
    digitalWrite(RELAY_PIN, HIGH); // Relay OFF
  }

  // Dispense pulsed bursts while active — non-blocking, so the encoder,
  // button, and display all keep updating while the pump is running.
  if (isWateringActive) {
    // Safety cutoff: if we've been pulsing for MAX_WATERING_DURATION_MS
    // without reaching wetLimit (empty reservoir, dislodged probe, clogged
    // line, etc.), stop trying and flag it instead of pumping indefinitely.
    if (now - wateringStartTime >= MAX_WATERING_DURATION_MS) {
      isWateringActive = false;
      pumpOn = false;
      wateringFault = true;
      digitalWrite(RELAY_PIN, HIGH); // Relay OFF
      return;
    }

    // Don't start new pulses in cold conditions — a Ficus takes up water
    // more slowly when cold, so keep pumping on the same schedule would
    // waterlog the soil rather than actually rehydrate the roots.
    bool tooCold = !isnan(tempC) && tempC < 15.0;

    unsigned long intervalMs = (unsigned long)active.intervalSec * 1000;
    unsigned long pulseMs    = (unsigned long)active.pulseTimeSec * 1000;

    if (!pumpOn && !tooCold && (lastPulseTime == 0 || now - lastPulseTime >= intervalMs)) {
      // Time to start the next pulse
      pumpOn = true;
      pumpStartTime = now;
      digitalWrite(RELAY_PIN, LOW); // Relay ON
    }

    if (pumpOn && now - pumpStartTime >= pulseMs) {
      // Pulse duration elapsed — stop it
      pumpOn = false;
      digitalWrite(RELAY_PIN, HIGH); // Relay OFF
      lastPulseTime = now;
    }
  }
}

// --- Rotary Encoder Navigation ---

// Fires the instant CLK goes LOW — kept intentionally tiny (no Serial, no
// EEPROM, no display calls) so it can't stall an in-progress I2C transfer
// to the OLED or block the main loop. Runs regardless of what loop() is
// doing, which is what actually fixes the "laggy"/missed-step feel: the
// old version only checked the pins once per loop iteration, so fast
// turns between checks were silently dropped.
void encoderISR() {
  // CLK is guaranteed LOW here (that's why FALLING fired), so just compare DT.
  int direction = (digitalRead(ENCODER_DT) == HIGH) ? -1 : 1; // reversed from before
  encoderDelta += direction;
  Serial.println("ENCODER ISR RAN!");
}

void handleEncoder() {
  noInterrupts();
  int delta = encoderDelta;
  encoderDelta = 0;
  interrupts();
  Serial.println("HANDLE ENCODER RAN");
  if (delta == 0) return;

  if (currentState == MAIN_SCREENS) {
    screenIndex = ((screenIndex + delta) % TOTAL_SCREENS + TOTAL_SCREENS) % TOTAL_SCREENS;
  } 
  else if (currentState == MENU_MODE) {
    menuIndex = ((menuIndex + delta) % TOTAL_MENU_ITEMS + TOTAL_MENU_ITEMS) % TOTAL_MENU_ITEMS;
  } 
  else if (currentState == EDIT_PARAM) {
    PlantSettings &active = plantProfiles[currentPlant];

    switch (menuIndex) {
      case 0: // Select Plant
        currentPlant = (PlantType)(((currentPlant + delta) % 3 + 3) % 3);
        break;
      case 1: // Adjust Dry Start Limit
        active.dryLimit = constrain(active.dryLimit + (delta * 10), active.wetLimit + 20, 560); // 560: just under this sensor's measured dry-air max of 570
        break;
      case 2: // Adjust Wet Stop Limit
        active.wetLimit = constrain(active.wetLimit + (delta * 10), 230, active.dryLimit - 20); // 230: just above this sensor's measured submerged min of 220
        break;
      case 3: // Adjust Pulse Duration
        active.pulseTimeSec = constrain(active.pulseTimeSec + delta, 1, 10);
        break;
      case 4: // Adjust Rest Interval
        active.intervalSec = constrain(active.intervalSec + delta, 5, 60);
        break;
    }
    // NOTE: no EEPROM save here anymore — see handleButton(). Writing on
    // every single detent was the other big source of lag (each write is
    // ~3.3ms per changed byte, blocking right in the middle of a turn).
  }
}

void handleButton() {
  bool reading = digitalRead(ENCODER_SW);
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  Serial.println("HANDLE BUTTON RAN");
  if ((millis() - lastDebounceTime) > 50) {
    static bool buttonState = HIGH;
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        if (currentState == MAIN_SCREENS) {
          currentState = MENU_MODE;
          menuIndex = 0;
        } else if (currentState == MENU_MODE) {
          if (menuIndex == 5) { // Exit Menu
            currentState = MAIN_SCREENS;
          } else {
            currentState = EDIT_PARAM;
          }
        } else if (currentState == EDIT_PARAM) {
          currentState = MENU_MODE;
          saveSettingsToEEPROM(); // Save once, now that editing is done
        }
      }
    }
  }
  lastButtonState = reading;
}

// --- Screen Updates ---

void updateDisplay() {
  display.clearDisplay();
  Serial.println("UPDATE DISPLAY RAN");
  PlantSettings &active = plantProfiles[currentPlant];

  int rawMoisture = analogRead(SENSOR_PIN);
  // dryLimit (higher raw) = 0%, wetLimit (lower raw) = 100%, so the
  // percentage always reflects this plant's own configured thresholds.
  int moisturePercent = map(rawMoisture, active.dryLimit, active.wetLimit, 0, 100);
  moisturePercent = constrain(moisturePercent, 0, 100);

  float tempC = dht.readTemperature();
  float humidity = dht.readHumidity();

  if (currentState == MAIN_SCREENS) {
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("< Page "));
    display.print(screenIndex + 1);
    display.print(F("/5 >"));

    switch (screenIndex) {
      case 0: // Environmental & Moisture Plant Graphic
        drawPlantGraphic(moisturePercent, tempC);
        break;

      case 1: // Moisture %
        display.setCursor(0, 20);
        display.print(F("Soil Moisture:"));
        display.setTextSize(3);
        display.setCursor(20, 38);
        display.print(moisturePercent);
        display.print(F("%"));
        break;

      case 2: // DHT11 Environment Data Page
        display.setCursor(0, 18);
        display.print(F("Environment Metrics"));
        display.setCursor(0, 32);
        display.print(F("Temp: "));
        if (isnan(tempC)) {
          display.print(F("Err"));
        } else {
          display.print((tempC * 9.0 / 5.0) + 32.0, 1);
          display.print(F(" F"));
        }
        display.setCursor(0, 48);
        display.print(F("Humidity: "));
        if (isnan(humidity)) {
          display.print(F("Err"));
        } else {
          display.print(humidity, 1);
          display.print(F("%"));
        }
        break;

      case 3: // Raw Sensor Readout
        display.setCursor(0, 20);
        display.print(F("Raw ADC Value:"));
        display.setTextSize(2);
        display.setCursor(20, 40);
        display.print(rawMoisture);
        break;

      case 4: // Active Plant Settings Overview
        display.setCursor(0, 12);
        if (currentPlant == FICUS) display.print(F("Mode: Ficus"));
        else if (currentPlant == SUCCULENT) display.print(F("Mode: Succulent"));
        else display.print(F("Mode: Fern"));

        display.setCursor(0, 25);
        display.print(F("Start/Stop: ")); display.print(active.dryLimit); display.print(F("/")); display.print(active.wetLimit);
        display.setCursor(0, 38);
        display.print(F("Pulse Time: ")); display.print(active.pulseTimeSec); display.print(F("s"));
        display.setCursor(0, 51);
        display.print(F("Rest Pause: ")); display.print(active.intervalSec); display.print(F("s"));
        break;
    }
  } 
  else if (currentState == MENU_MODE || currentState == EDIT_PARAM) {
    display.setTextSize(1);
    display.setCursor(15, 0);
    display.print(F("=== SETTINGS ==="));

    const char* options[] = {"Profile", "Dry Limit", "Wet Limit", "Pulse Time", "Rest Pause", "Save & Exit"};
    
    // Display up to 4 items at a time with scrolling offset
    int topItem = (menuIndex > 3) ? menuIndex - 3 : 0;

    for (int i = topItem; i < min(topItem + 4, TOTAL_MENU_ITEMS); i++) {
      int y = 16 + ((i - topItem) * 12);
      if (i == menuIndex) {
        display.setCursor(0, y);
        display.print(currentState == EDIT_PARAM ? F(">") : F("->"));
      }
      display.setCursor(12, y);
      display.print(options[i]);

      // Show values next to menu options
      display.setCursor(95, y);
      switch (i) {
        case 0:
          if (currentPlant == FICUS) display.print(F("Ficus"));
          else if (currentPlant == SUCCULENT) display.print(F("Succ."));
          else display.print(F("Fern"));
          break;
        case 1: display.print(active.dryLimit); break;
        case 2: display.print(active.wetLimit); break;
        case 3: display.print(active.pulseTimeSec); display.print(F("s")); break;
        case 4: display.print(active.intervalSec); display.print(F("s")); break;
      }
    }
  }

  display.display();
}

// Dynamic Graphic Avatar Renderer (Weather Effects Drawn ON the Plant)
void drawPlantGraphic(int moisturePercent, float tempC) {
  display.setTextSize(1);

  // 1. Determine Status Line
  display.setCursor(0, 14);
  if (wateringFault) {
    display.print(F("Status: Check H2O!"));
  } else if (!isnan(tempC) && tempC < 15.0) {
    display.print(F("Status: Too Cold!"));
  } else if (!isnan(tempC) && tempC > 29.0) {
    display.print(F("Status: Too Hot!"));
  } else if (isWateringActive) {
    display.print(F("Status: Pumping H2O!"));
  } else {
    display.print(F("Status: Feeling Good!"));
  }

  // 2. Base Character Rendering
  if (currentPlant == FICUS) {
    // Ficus Bonsai
    display.fillRoundRect(50, 52, 28, 10, 2, SSD1306_WHITE); // Pot
    display.fillRect(62, 40, 5, 12, SSD1306_WHITE);          // Trunk
    display.fillCircle(64, 32, 14, SSD1306_WHITE);          // Canopy
    
    // Face Features
    if (!isnan(tempC) && tempC < 15.0) { // Cold
      display.fillCircle(59, 30, 2, SSD1306_BLACK);
      display.fillCircle(69, 30, 2, SSD1306_BLACK);
      display.drawLine(60, 36, 68, 36, SSD1306_BLACK); // Shiver line
    } else if (!isnan(tempC) && tempC > 29.0) { // Hot
      display.fillCircle(59, 30, 2, SSD1306_BLACK);
      display.fillCircle(69, 30, 2, SSD1306_BLACK);
      display.drawCircle(64, 37, 2, SSD1306_BLACK);   // Open mouth
    } else { // Normal
      display.fillCircle(59, 30, 2, SSD1306_BLACK);
      display.fillCircle(69, 30, 2, SSD1306_BLACK);
      display.drawLine(61, 36, 67, 36, SSD1306_BLACK); // Smile
    }
  } 
  else if (currentPlant == SUCCULENT) {
    // Cactus
    display.fillTriangle(52, 52, 76, 52, 70, 63, SSD1306_WHITE); // Pot
    display.fillRect(50, 49, 28, 4, SSD1306_WHITE);
    display.fillRoundRect(60, 28, 9, 21, 4, SSD1306_WHITE);       // Main Stem
    display.fillRect(52, 36, 8, 3, SSD1306_WHITE);                // Left Arm
    display.fillRect(52, 30, 3, 7, SSD1306_WHITE);
    display.fillRect(69, 39, 8, 3, SSD1306_WHITE);                // Right Arm
    display.fillRect(74, 33, 3, 7, SSD1306_WHITE);
    
    // Face Features
    display.fillCircle(62, 34, 1, SSD1306_BLACK);
    display.fillCircle(67, 34, 1, SSD1306_BLACK);
    if (!isnan(tempC) && tempC > 29.0) {
      display.drawPixel(64, 37, SSD1306_BLACK); // O-mouth
    } else {
      display.drawPixel(64, 37, SSD1306_BLACK);
    }
  } 
  else if (currentPlant == FERN) {
    // Hanging Fern
    display.fillTriangle(54, 50, 74, 50, 64, 60, SSD1306_WHITE); // Pot
    display.drawLine(54, 50, 48, 30, SSD1306_WHITE);
    display.drawLine(74, 50, 80, 30, SSD1306_WHITE);
    display.drawLine(64, 50, 40, 25, SSD1306_WHITE);            // Fronds
    display.drawLine(64, 50, 88, 25, SSD1306_WHITE);
    display.drawLine(64, 50, 32, 38, SSD1306_WHITE);
    display.drawLine(64, 50, 96, 38, SSD1306_WHITE);
    display.drawLine(64, 50, 64, 20, SSD1306_WHITE);
    
    // Face Features
    display.fillCircle(60, 53, 1, SSD1306_BLACK);
    display.fillCircle(68, 53, 1, SSD1306_BLACK);
    display.drawLine(62, 56, 66, 56, SSD1306_BLACK);
  }

  // 3. Overlay Weather Graphics ON TOP of the Plant Character

  // Cold Weather: Icicles dripping directly from the plant leaves/branches
  if (!isnan(tempC) && tempC < 15.0) {
    if (currentPlant == FICUS) {
      display.fillTriangle(52, 38, 56, 38, 54, 46, SSD1306_WHITE); // Left Canopy Icicle
      display.fillTriangle(72, 38, 76, 38, 74, 46, SSD1306_WHITE); // Right Canopy Icicle
    } else if (currentPlant == SUCCULENT) {
      display.fillTriangle(50, 30, 54, 30, 52, 36, SSD1306_WHITE); // Arm Icicle
      display.fillTriangle(72, 33, 76, 33, 74, 39, SSD1306_WHITE);
    } else if (currentPlant == FERN) {
      display.fillTriangle(38, 26, 42, 26, 40, 33, SSD1306_WHITE); // Frond Icicle
      display.fillTriangle(86, 26, 90, 26, 88, 33, SSD1306_WHITE);
    }
  } 

  // Hot Weather: Sun rays beating down & sweat drops on the plant
  if (!isnan(tempC) && tempC > 29.0) {
    // Blazing Sun graphic in the upper corner shining toward the plant
    display.drawCircle(115, 12, 5, SSD1306_WHITE);
    display.drawLine(115, 4, 115, 1, SSD1306_WHITE);
    display.drawLine(108, 12, 105, 12, SSD1306_WHITE);
    display.drawLine(110, 17, 106, 21, SSD1306_WHITE); // Ray pointing directly down at plant

    // Sweat Drop on the Plant
    if (currentPlant == FICUS) {
      display.drawPixel(75, 26, SSD1306_BLACK); // Sweat drop on edge of canopy
      display.drawPixel(76, 27, SSD1306_BLACK);
    } else if (currentPlant == SUCCULENT) {
      display.drawPixel(68, 30, SSD1306_BLACK); // Sweat drop on cactus stem
    } else if (currentPlant == FERN) {
      display.drawPixel(71, 52, SSD1306_BLACK); // Sweat drop on pot
    }
  }
}
