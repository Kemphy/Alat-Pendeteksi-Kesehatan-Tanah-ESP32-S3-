/*
 * =========================================================================
 * SISTEM PEMANTAUAN KESEHATAN TANAMAN v3.6 (ESP32-S3)
 *
 * TARGET HARDWARE : ESP32-S3 N16R8 (16MB Flash, 8MB PSRAM)
 * PARTISI FLASH   : Custom (factory)
 *   nvs       data  nvs       0x9000   20KB
 *   coredump  data  coredump  0xE000    8KB
 *   app0      app   factory  0x10000    7MB
 *   spiffs    data  spiffs  0x710000  ~8.94MB  (LittleFS)
 *
 * FITUR v3.6:
 * - Display labels lengkap (Nitrogen, Konduktivitas, dll)
 * - Waktu WIB dari NTP/GPS
 * - Status GPS satelit
 * - Navigasi optimal dengan indikator
 * - Layout maksimal 128x64 (header+footer)
 * - WiFi Management (Scan, Last Used, Hapus)
 * - Auto-connect ke WiFi terakhir digunakan
 * - Input WiFi: Default + Serial + Scan+Password
 * - LittleFS ~8.94MB—mendukung hingga 50 pengukuran per ID tanaman
 * =========================================================================
 */

// ==============================================
// 1. KREDENSIAL DEFAULT
// ==============================================
#define DEFAULT_WIFI_SSID "Tokolontong_2.4G"
#define DEFAULT_WIFI_PASS "Tanyaebeb."
#define API_KEY "AIzaSyClLeqeC-In594OFRSD84wPO__gtGpd0Zo"
#define DATABASE_URL "https://test-capstone18-default-rtdb.asia-southeast1.firebasedatabase.app/"

// NTP Server untuk sync waktu
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 25200 // WIB = UTC+7
#define DAYLIGHT_OFFSET_SEC 0

// Batas pengukuran per ID tanaman (LittleFS ~8.94MB, aman hingga 50 entri per plant)
#define MAX_MEASUREMENTS 50

// ==============================================
// 2. LIBRARY
// ==============================================
#include <WiFi.h>
#include <HardwareSerial.h>
#include <LittleFS.h>
#include <time.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Keypad.h>
#include <TinyGPSPlus.h>
#include <ArduinoJson.h>

// ==============================================
// 3. DEFINISI PIN & OBJEK
// ==============================================

// --- OLED SH1106 ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define i2c_Address 0x3C
Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- KEYPAD 4x4 ---
const byte ROWS = 4;
const byte COLS = 4;
byte rowPins[ROWS] = {38, 37, 36, 35};
byte colPins[COLS] = {42, 41, 40, 39};
char keys[ROWS][COLS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'}};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- KEYPAD DEBOUNCING ---
#define DEBOUNCE_DELAY 400
unsigned long lastKeyTime = 0;
char lastKey = '\0';

// --- GPS (UART1: RX=17, TX=18) ---
TinyGPSPlus gps;
float currentLat = 0.0, currentLng = 0.0;
int currentSats = 0;

// --- SENSOR NPK (UART2: RX=16, TX=15) ---
#define RS485_DE_RE_PIN 7
#define DEVICE_ADDRESS 0x01
#define READ_HOLDING_REGISTERS 0x03

#define REG_MOISTURE 0x0000
#define REG_TEMPERATURE 0x0001
#define REG_CONDUCTIVITY 0x0002
#define REG_PH 0x0003
#define REG_NITROGEN 0x0004
#define REG_PHOSPHORUS 0x0005
#define REG_POTASSIUM 0x0006

// --- FIREBASE ---
FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool signupOK = false;
bool timeSync = false;
bool firebaseInitialized = false;

// --- WiFi Management ---
struct WiFiCredential
{
  String ssid;
  String password;
};
WiFiCredential wifiList[4];
int wifiCount = 0;
String lastUsedSSID = "";
String wifiScanResults[20];
int wifiScanCount = 0;

// --- STATE MACHINE ---
enum State
{
  STATE_SPLASH,
  STATE_MAIN_MENU,
  STATE_INPUT_ID,
  STATE_MEASURING,
  STATE_DISPLAY_DATA,
  STATE_SAVE_CONFIRM,
  STATE_VIEW_LIST,
  STATE_VIEW_MEASUREMENT,
  STATE_VIEW_DETAIL,
  STATE_DELETE_MENU,
  STATE_DELETE_CONFIRM,
  STATE_SYNC_MENU,
  STATE_WIFI_SELECT,
  STATE_WIFI_SCAN,
  STATE_WIFI_INPUT_PASS,
  STATE_SYNCING,
  STATE_SYNC_DONE,
  STATE_SYNC_FAILED
};

State currentState = STATE_SPLASH;

// --- DATA STORAGE ---
struct MeasurementData
{
  float nitrogen;
  float fosfor;
  float kalium;
  float ph;
  float suhu;
  float kelembaban;
  float konduktivitas;
  float latitude;
  float longitude;
  String waktuPengambilan;
};

String currentPlantID = "";
String inputBuffer = "";
MeasurementData currentMeasurement;
char displayMode = 'A';
int menuSelection = 0;
int listSelection = 0;
int measurementSelection = 0;
int deleteMode = 0;
int wifiSelection = 0;
String selectedSSID = "";

// --- TIMING ---
unsigned long lastGPSRead = 0;
bool needsDisplayUpdate = true;
unsigned long lastWiFiCheck = 0;
bool wifiConnecting = false;

// ==============================================
// 4. FUNGSI MODBUS (REAL SENSOR)
// ==============================================

uint16_t calculateCRC(uint8_t *data, uint8_t length)
{
  uint16_t crc = 0xFFFF;
  for (uint8_t pos = 0; pos < length; pos++)
  {
    crc ^= (uint16_t)data[pos];
    for (uint8_t i = 8; i != 0; i--)
    {
      if ((crc & 0x0001) != 0)
      {
        crc >>= 1;
        crc ^= 0xA001;
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void createRequestFrame(uint8_t *frame, uint8_t deviceAddress, uint8_t functionCode,
                        uint16_t registerAddress, uint16_t registerCount)
{
  frame[0] = deviceAddress;
  frame[1] = functionCode;
  frame[2] = (registerAddress >> 8) & 0xFF;
  frame[3] = registerAddress & 0xFF;
  frame[4] = (registerCount >> 8) & 0xFF;
  frame[5] = registerCount & 0xFF;

  uint16_t crc = calculateCRC(frame, 6);
  frame[6] = crc & 0xFF;
  frame[7] = (crc >> 8) & 0xFF;
}

uint16_t readRegister(uint16_t registerAddress)
{
  uint8_t requestFrame[8];
  uint8_t responseFrame[10];
  uint16_t value = 0xFFFF;

  createRequestFrame(requestFrame, DEVICE_ADDRESS, READ_HOLDING_REGISTERS, registerAddress, 1);

  digitalWrite(RS485_DE_RE_PIN, HIGH);
  delay(10);
  Serial2.write(requestFrame, sizeof(requestFrame));
  Serial2.flush();
  digitalWrite(RS485_DE_RE_PIN, LOW);
  delay(50);

  unsigned long startTime = millis();
  uint8_t index = 0;

  while (millis() - startTime < 1000 && index < sizeof(responseFrame))
  {
    if (Serial2.available())
    {
      responseFrame[index++] = Serial2.read();
    }
    else
    {
      delay(1); // yield CPU, hindari busy-wait 100%
    }
  }

  if (index >= 7)
  {
    uint16_t receivedCRC = (responseFrame[index - 1] << 8) | responseFrame[index - 2];
    uint16_t calculatedCRC = calculateCRC(responseFrame, index - 2);

    if (receivedCRC == calculatedCRC && responseFrame[1] == READ_HOLDING_REGISTERS)
    {
      value = (responseFrame[3] << 8) | responseFrame[4];
    }
  }

  return value;
}

// ==============================================
// 5. FUNGSI GPS & WAKTU
// ==============================================

void updateGPS()
{
  while (Serial1.available() > 0)
  {
    char c = Serial1.read();
    gps.encode(c);
  }

  if (millis() - lastGPSRead > 1000)
  {
    lastGPSRead = millis();

    if (gps.location.isValid())
    {
      currentLat = gps.location.lat();
      currentLng = gps.location.lng();
    }

    if (gps.satellites.isValid())
    {
      currentSats = gps.satellites.value();
    }
  }
}

String getWaktuPengambilan()
{
  if (gps.date.isValid() && gps.time.isValid())
  {
    struct tm timeinfo;
    timeinfo.tm_year = gps.date.year() - 1900;
    timeinfo.tm_mon = gps.date.month() - 1;
    timeinfo.tm_mday = gps.date.day();
    timeinfo.tm_hour = gps.time.hour();
    timeinfo.tm_min = gps.time.minute();
    timeinfo.tm_sec = gps.time.second();

    time_t utcTime = mktime(&timeinfo);
    time_t wibTime = utcTime + GMT_OFFSET_SEC;
    struct tm *wibTimeInfo = localtime(&wibTime);

    char buffer[20];
    sprintf(buffer, "%02d-%02d-%04dT%02d:%02d:%02d",
            wibTimeInfo->tm_mday,
            wibTimeInfo->tm_mon + 1,
            wibTimeInfo->tm_year + 1900,
            wibTimeInfo->tm_hour,
            wibTimeInfo->tm_min,
            wibTimeInfo->tm_sec);

    return String(buffer);
  }

  time_t now = time(nullptr);
  if (now > 1600000000)
  {
    struct tm *timeinfo = localtime(&now);

    char buffer[20];
    sprintf(buffer, "%02d-%02d-%04dT%02d:%02d:%02d",
            timeinfo->tm_mday,
            timeinfo->tm_mon + 1,
            timeinfo->tm_year + 1900,
            timeinfo->tm_hour,
            timeinfo->tm_min,
            timeinfo->tm_sec);

    return String(buffer);
  }

  return "01-01-2025T00:00:00";
}

void measureAllSensors()
{
  Serial.println("Membaca sensor... [MODE: REAL HARDWARE]");

  // Bersihkan serial buffer sebelum pembacaan
  while (Serial2.available())
  {
    Serial2.read();
  }

  // Baca semua register dari sensor fisik
  uint16_t raw_moisture = readRegister(REG_MOISTURE);
  delay(200);
  uint16_t raw_temp = readRegister(REG_TEMPERATURE);
  delay(200);
  uint16_t raw_cond = readRegister(REG_CONDUCTIVITY);
  delay(200);
  uint16_t raw_ph = readRegister(REG_PH);
  delay(200);
  uint16_t raw_n = readRegister(REG_NITROGEN);
  delay(200);
  uint16_t raw_p = readRegister(REG_PHOSPHORUS);
  delay(200);
  uint16_t raw_k = readRegister(REG_POTASSIUM);
  delay(200);

  // Konversi nilai raw ke format yang sesuai
  currentMeasurement.kelembaban = (raw_moisture != 0xFFFF && raw_moisture < 1000) ? raw_moisture / 10.0 : 0.0;
  currentMeasurement.suhu = (raw_temp != 0xFFFF && raw_temp < 1000) ? raw_temp / 10.0 : 0.0;
  currentMeasurement.konduktivitas = (raw_cond != 0xFFFF && raw_cond < 10000) ? raw_cond : 0.0;
  currentMeasurement.ph = (raw_ph != 0xFFFF && raw_ph < 200) ? raw_ph / 10.0 : 0.0;
  currentMeasurement.nitrogen = (raw_n != 0xFFFF && raw_n < 2000) ? raw_n : 0.0;
  currentMeasurement.fosfor = (raw_p != 0xFFFF && raw_p < 2000) ? raw_p : 0.0;
  currentMeasurement.kalium = (raw_k != 0xFFFF && raw_k < 2000) ? raw_k : 0.0;

  // Update GPS dan waktu
  updateGPS();
  currentMeasurement.latitude = currentLat;
  currentMeasurement.longitude = currentLng;
  currentMeasurement.waktuPengambilan = getWaktuPengambilan();

  // Tampilkan hasil ke Serial Monitor
  Serial.println("=== Hasil Pembacaan ===");
  Serial.printf("Kelembaban   : %.1f%%\n", currentMeasurement.kelembaban);
  Serial.printf("Suhu         : %.1f C\n", currentMeasurement.suhu);
  Serial.printf("Konduktivitas: %.0f uS/cm\n", currentMeasurement.konduktivitas);
  Serial.printf("pH           : %.1f\n", currentMeasurement.ph);
  Serial.printf("Nitrogen     : %.0f mg/kg\n", currentMeasurement.nitrogen);
  Serial.printf("Fosfor       : %.0f mg/kg\n", currentMeasurement.fosfor);
  Serial.printf("Kalium       : %.0f mg/kg\n", currentMeasurement.kalium);
  Serial.printf("Koordinat    : %.6f, %.6f\n", currentMeasurement.latitude, currentMeasurement.longitude);
  Serial.printf("Waktu        : %s\n", currentMeasurement.waktuPengambilan.c_str());
  Serial.println("========================");
}

// ==============================================
// 6. FUNGSI WiFi MANAGEMENT
// ==============================================

void loadWiFiConfig()
{
  wifiCount = 0;
  lastUsedSSID = "";

  if (!LittleFS.exists("/wifi_config.json"))
  {
    wifiList[0].ssid = DEFAULT_WIFI_SSID;
    wifiList[0].password = DEFAULT_WIFI_PASS;
    wifiCount = 1;
    lastUsedSSID = DEFAULT_WIFI_SSID;
    saveWiFiConfig();
    Serial.println("Created default WiFi config");
    return;
  }

  File file = LittleFS.open("/wifi_config.json", FILE_READ);
  DynamicJsonDocument doc(2048);
  deserializeJson(doc, file);
  file.close();

  lastUsedSSID = doc["last_used"].as<String>();
  JsonArray wifiArray = doc["wifi_list"];

  for (int i = 0; i < wifiArray.size() && i < 4; i++)
  {
    wifiList[i].ssid = wifiArray[i]["ssid"].as<String>();
    wifiList[i].password = wifiArray[i]["password"].as<String>();
    wifiCount++;
  }

  Serial.printf("Loaded %d WiFi configs, last used: %s\n", wifiCount, lastUsedSSID.c_str());
}

void saveWiFiConfig()
{
  File file = LittleFS.open("/wifi_config.json", FILE_WRITE);
  DynamicJsonDocument doc(2048);

  doc["last_used"] = lastUsedSSID;
  JsonArray wifiArray = doc.createNestedArray("wifi_list");

  for (int i = 0; i < wifiCount; i++)
  {
    JsonObject wifi = wifiArray.createNestedObject();
    wifi["ssid"] = wifiList[i].ssid;
    wifi["password"] = wifiList[i].password;
  }

  serializeJson(doc, file);
  file.close();
  Serial.println("WiFi config saved");
}

String getWiFiPassword(String ssid)
{
  for (int i = 0; i < wifiCount; i++)
  {
    if (wifiList[i].ssid == ssid)
      return wifiList[i].password;
  }
  return "";
}

void addWiFiCredential(String ssid, String password)
{
  for (int i = 0; i < wifiCount; i++)
  {
    if (wifiList[i].ssid == ssid)
    {
      wifiList[i].password = password;
      saveWiFiConfig();
      Serial.println("WiFi updated");
      return;
    }
  }

  if (wifiCount < 4)
  {
    wifiList[wifiCount].ssid = ssid;
    wifiList[wifiCount].password = password;
    wifiCount++;
    saveWiFiConfig();
    Serial.println("WiFi added");
  }
  else
  {
    Serial.println("WiFi list full!");
  }
}

void deleteWiFiCredential(int index)
{
  if (index < 0 || index >= wifiCount)
    return;

  String deletedSSID = wifiList[index].ssid; // simpan sebelum array di-shift

  for (int i = index; i < wifiCount - 1; i++)
  {
    wifiList[i] = wifiList[i + 1];
  }
  wifiCount--;

  if (lastUsedSSID == deletedSSID)
  {
    lastUsedSSID = wifiCount > 0 ? wifiList[0].ssid : "";
  }

  saveWiFiConfig();
  Serial.println("WiFi deleted");
}

void scanWiFi()
{
  Serial.println("Scanning WiFi...");
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("  Scanning WiFi...");
  display.display();

  wifiScanCount = WiFi.scanNetworks();
  if (wifiScanCount > 20)
    wifiScanCount = 20;

  for (int i = 0; i < wifiScanCount; i++)
  {
    wifiScanResults[i] = WiFi.SSID(i);
  }

  Serial.printf("Found %d networks\n", wifiScanCount);
}

// ==============================================
// 7. FUNGSI FILE MANAGEMENT
// ==============================================

String getPlantFileName(String plantID)
{
  return "/plant_" + plantID + ".json";
}

bool saveMeasurementToFile(String plantID, MeasurementData data)
{
  String filename = getPlantFileName(plantID);

  DynamicJsonDocument doc(8192);
  JsonArray measurements;

  if (LittleFS.exists(filename))
  {
    File file = LittleFS.open(filename, FILE_READ);
    if (!file)
      return false;

    DeserializationError error = deserializeJson(doc, file);
    file.close();
    if (error)
      return false;

    measurements = doc["pengukuran"].as<JsonArray>();
  }
  else
  {
    doc["datatanaman_id"] = plantID.toInt();
    measurements = doc.createNestedArray("pengukuran");
  }

  if (measurements.size() >= MAX_MEASUREMENTS)
  {
    Serial.printf("ERROR: Maks %d pengukuran per ID tercapai\n", MAX_MEASUREMENTS);
    return false;
  }

  JsonObject newMeasurement = measurements.createNestedObject();
  newMeasurement["datatanaman_id"] = plantID.toInt();
  newMeasurement["ph"] = data.ph;
  newMeasurement["nitrogen"] = data.nitrogen;
  newMeasurement["fosfor"] = data.fosfor;
  newMeasurement["kalium"] = data.kalium;
  newMeasurement["suhu"] = data.suhu;
  newMeasurement["kelembaban"] = data.kelembaban;
  newMeasurement["konduktivitas"] = data.konduktivitas;
  newMeasurement["koordinat"] = String(data.latitude, 6) + "," + String(data.longitude, 6);
  newMeasurement["waktu_pengambilan"] = data.waktuPengambilan;

  File file = LittleFS.open(filename, FILE_WRITE);
  if (!file)
    return false;

  serializeJson(doc, file);
  file.close();

  Serial.println("SUCCESS: Data saved!");
  return true;
}

void getPlantIDList(String *list, int *count)
{
  *count = 0;

  File root = LittleFS.open("/");
  if (!root || !root.isDirectory())
    return;

  File file = root.openNextFile();

  while (file && *count < 20)
  {
    String fullPath = String(file.path());
    String fileName = String(file.name());

    bool isPlantFile = false;
    String plantID = "";

    if (fullPath.startsWith("/plant_") && fullPath.endsWith(".json"))
    {
      isPlantFile = true;
      int startIdx = fullPath.indexOf("_") + 1;
      int endIdx = fullPath.lastIndexOf(".");
      plantID = fullPath.substring(startIdx, endIdx);
    }
    else if (fileName.startsWith("plant_") && fileName.endsWith(".json"))
    {
      isPlantFile = true;
      int startIdx = fileName.indexOf("_") + 1;
      int endIdx = fileName.lastIndexOf(".");
      plantID = fileName.substring(startIdx, endIdx);
    }

    if (isPlantFile && plantID.length() > 0)
    {
      bool isDuplicate = false;
      for (int i = 0; i < *count; i++)
      {
        if (list[i] == plantID)
        {
          isDuplicate = true;
          break;
        }
      }
      if (!isDuplicate)
      {
        list[*count] = plantID;
        (*count)++;
      }
    }

    file = root.openNextFile();
  }
  root.close();

  // Sort ascending berdasarkan ID numerik
  for (int i = 0; i < *count - 1; i++)
  {
    for (int j = i + 1; j < *count; j++)
    {
      if (list[i].toInt() > list[j].toInt())
      {
        String temp = list[i];
        list[i] = list[j];
        list[j] = temp;
      }
    }
  }
}

int getMeasurementCount(String plantID)
{
  String filename = getPlantFileName(plantID);
  if (!LittleFS.exists(filename))
    return 0;

  File file = LittleFS.open(filename, FILE_READ);
  DynamicJsonDocument doc(16384);
  deserializeJson(doc, file);
  file.close();

  JsonArray measurements = doc["pengukuran"];
  return measurements.size();
}

bool getMeasurementByIndex(String plantID, int index, MeasurementData *data)
{
  String filename = getPlantFileName(plantID);
  if (!LittleFS.exists(filename))
    return false;

  File file = LittleFS.open(filename, FILE_READ);
  DynamicJsonDocument doc(16384);
  deserializeJson(doc, file);
  file.close();

  JsonArray measurements = doc["pengukuran"];
  if (index >= measurements.size())
    return false;

  JsonObject m = measurements[index];
  data->nitrogen = m["nitrogen"];
  data->fosfor = m["fosfor"];
  data->kalium = m["kalium"];
  data->ph = m["ph"];
  data->suhu = m["suhu"];
  data->kelembaban = m["kelembaban"];
  data->konduktivitas = m["konduktivitas"];

  String koordinat = m["koordinat"];
  int commaIndex = koordinat.indexOf(',');
  if (commaIndex > 0)
  {
    data->latitude = koordinat.substring(0, commaIndex).toFloat();
    data->longitude = koordinat.substring(commaIndex + 1).toFloat();
  }

  data->waktuPengambilan = m["waktu_pengambilan"].as<String>();
  return true;
}

bool deletePlantData(String plantID)
{
  String filename = getPlantFileName(plantID);
  if (LittleFS.exists(filename))
    return LittleFS.remove(filename);
  return false;
}

bool deleteMeasurementByIndex(String plantID, int index)
{
  String filename = getPlantFileName(plantID);
  if (!LittleFS.exists(filename))
    return false;

  File file = LittleFS.open(filename, FILE_READ);
  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error)
    return false;

  JsonArray measurements = doc["pengukuran"];
  if (index >= measurements.size())
    return false;

  measurements.remove(index);

  if (measurements.size() == 0)
  {
    LittleFS.remove(filename);
    return true;
  }

  file = LittleFS.open(filename, FILE_WRITE);
  if (!file)
    return false;

  serializeJson(doc, file);
  file.close();
  return true;
}

// ==============================================
// 8. FUNGSI FIREBASE
// ==============================================

bool syncTimeWithNTP()
{
  Serial.println("Syncing time with NTP...");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  int retry = 0;
  time_t now = time(nullptr);

  while (now < 1600000000 && retry < 20)
  {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }

  if (now > 1600000000)
  {
    Serial.println();
    Serial.print("Time synced: ");
    Serial.println(ctime(&now));
    return true;
  }
  else
  {
    Serial.println("\nTime sync failed!");
    return false;
  }
}

void initFirebase()
{
  if (firebaseInitialized)
    return;

  Serial.println("Initializing Firebase...");

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  config.token_status_callback = tokenStatusCallback;

  if (Firebase.signUp(&config, &auth, "", ""))
  {
    Serial.println("Firebase signup OK");
    signupOK = true;
  }
  else
  {
    Serial.printf("Firebase signup error: %s\n", config.signer.signupError.message.c_str());
    signupOK = false;
  }

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  firebaseInitialized = true;
}

bool isFirebaseReady()
{
  return (WiFi.status() == WL_CONNECTED && signupOK && Firebase.ready() && timeSync);
}

int uploadAllDataToFirebase()
{
  if (!isFirebaseReady())
  {
    Serial.println("ERROR: Firebase not ready");
    return -1;
  }

  String plantList[20];
  int plantCount = 0;
  getPlantIDList(plantList, &plantCount);

  Serial.printf("Found %d plant IDs to upload\n", plantCount);

  if (plantCount == 0)
  {
    Serial.println("No data to upload");
    return 0;
  }

  int successCount = 0;
  int totalMeasurements = 0;

  for (int i = 0; i < plantCount; i++)
  {
    String plantID = plantList[i];
    String filename = getPlantFileName(plantID);

    Serial.printf("\n[%d/%d] Uploading Plant ID: %s\n", i + 1, plantCount, plantID.c_str());

    menuSelection = i + 1;
    needsDisplayUpdate = true;
    updateDisplay();

    File file = LittleFS.open(filename, FILE_READ);
    if (!file)
    {
      Serial.println("  Failed to open file");
      continue;
    }

    DynamicJsonDocument doc(16384);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    if (error)
    {
      Serial.printf("  JSON parse failed: %s\n", error.c_str());
      continue;
    }

    String plantPath = "/sensor/" + plantID;
    FirebaseJson plantJson;
    plantJson.set("datatanaman_id", plantID.toInt());

    if (!Firebase.RTDB.setJSON(&fbdo, plantPath.c_str(), &plantJson))
    {
      Serial.printf("  Failed to set datatanaman_id: %s\n", fbdo.errorReason().c_str());
    }

    JsonArray measurements = doc["pengukuran"];
    Serial.printf("  Uploading %d measurements...\n", measurements.size());

    bool allSuccess = true;

    for (int j = 0; j < measurements.size(); j++)
    {
      JsonObject m = measurements[j];

      String waktuKey = m["waktu_pengambilan"].as<String>();
      String path = "/sensor/" + plantID + "/pengukuran/" + waktuKey;

      FirebaseJson json;
      json.set("datatanaman_id", plantID.toInt());
      json.set("ph", (float)m["ph"]);
      json.set("nitrogen", (float)m["nitrogen"]);
      json.set("fosfor", (float)m["fosfor"]);
      json.set("kalium", (float)m["kalium"]);
      json.set("suhu", (float)m["suhu"]);
      json.set("kelembaban", (float)m["kelembaban"]);
      json.set("konduktivitas", (float)m["konduktivitas"]);
      json.set("koordinat", m["koordinat"].as<String>());

      Serial.printf("    [%d/%d] Uploading %s... ", j + 1, measurements.size(), waktuKey.c_str());

      bool uploaded = false;
      for (int retry = 0; retry < 3; retry++)
      {
        if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json))
        {
          Serial.println("OK");
          uploaded = true;
          totalMeasurements++;
          break;
        }
        else
        {
          Serial.printf("Retry %d: %s\n", retry + 1, fbdo.errorReason().c_str());
          delay(1000);
        }
      }

      if (!uploaded)
      {
        allSuccess = false;
        Serial.println("    Upload FAILED after 3 retries");
        break;
      }

      delay(200);
    }

    if (allSuccess)
    {
      if (LittleFS.remove(filename))
      {
        Serial.println("  Local file deleted");
        successCount++;
      }
    }
    else
    {
      Serial.println("  Skipped due to errors");
    }
  }

  Serial.printf("\n=== Upload Complete ===\n");
  Serial.printf("Plant IDs   : %d/%d successful\n", successCount, plantCount);
  Serial.printf("Measurements: %d uploaded\n", totalMeasurements);
  Serial.println("=======================\n");

  if (successCount > 0 && WiFi.status() == WL_CONNECTED)
  {
    lastUsedSSID = WiFi.SSID();
    saveWiFiConfig();
    Serial.printf("Updated last_used to: %s\n", lastUsedSSID.c_str());
  }

  return successCount;
}

// ==============================================
// 9. FUNGSI DISPLAY
// ==============================================

void drawHeader(String title)
{
  display.setCursor(0, 0);
  display.print(title);
  display.drawLine(0, 9, 127, 9, SH110X_WHITE);
}

void drawFooter(String text)
{
  display.drawLine(0, 54, 127, 54, SH110X_WHITE);
  display.setCursor(0, 56);
  display.print(text);
}

void updateDisplay()
{
  if (!needsDisplayUpdate)
    return;
  needsDisplayUpdate = false;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SH110X_WHITE);

  switch (currentState)
  {
  case STATE_SPLASH:
    display.setTextSize(2);
    display.setCursor(5, 5);
    display.println("Monitor");
    display.setCursor(5, 25);
    display.println("Kesehatan");
    display.setCursor(5, 45);
    display.println("Tanaman");
    break;

  case STATE_MAIN_MENU:
    drawHeader("MENU UTAMA");
    display.setCursor(0, 14);
    display.println("1. Periksa Tanaman");
    display.println("2. Data Tanaman");
    display.println("3. Kirim Data");
    drawFooter("Tekan 1/2/3");
    break;

  case STATE_INPUT_ID:
    drawHeader("INPUT ID TANAMAN");
    display.setTextSize(2);
    display.setCursor(20, 25);
    display.println(inputBuffer.length() > 0 ? inputBuffer : "_");
    display.setTextSize(1);
    drawFooter("#=OK  *=Batal  D=Del");
    break;

  case STATE_MEASURING:
    display.setTextSize(2);
    display.setCursor(10, 20);
    display.println("Mengukur");
    display.setCursor(15, 40);
    display.println("Sensor");
    break;

  case STATE_DISPLAY_DATA:
  {
    char header[20];
    char modeChar = displayMode;
    if (displayMode == 'C')
    {
      sprintf(header, "ID:%s [%c] GPS:%d", currentPlantID.c_str(), modeChar, currentSats);
    }
    else
    {
      sprintf(header, "ID:%s [%c]", currentPlantID.c_str(), modeChar);
    }
    drawHeader(header);
    display.setCursor(0, 14);

    if (displayMode == 'A')
    {
      display.println("pH: " + String(currentMeasurement.ph, 1));
      display.println("Nitrogen:" + String(currentMeasurement.nitrogen, 0) + " mg/kg");
      display.println("Fosfor: " + String(currentMeasurement.fosfor, 0) + " mg/kg");
      display.println("Kalium: " + String(currentMeasurement.kalium, 0) + " mg/kg");
    }
    else if (displayMode == 'B')
    {
      display.println("Suhu: " + String(currentMeasurement.suhu, 1) + " C");
      display.println("Kelembaban:" + String(currentMeasurement.kelembaban, 1) + " %");
      display.println("Konduktivitas:");
      display.println(" " + String(currentMeasurement.konduktivitas, 0) + " uS/cm");
    }
    else if (displayMode == 'C')
    {
      String koordinatStr = String(currentMeasurement.latitude, 6) + ", " + String(currentMeasurement.longitude, 6);
      display.println("Koordinat:");
      display.println(" " + koordinatStr);
      String waktu = currentMeasurement.waktuPengambilan;
      display.println("Tanggal: " + waktu.substring(0, 10));
      display.println("Waktu: " + waktu.substring(11));
    }

    drawFooter("A/B/C #=Save *=Batal");
  }
  break;

  case STATE_SAVE_CONFIRM:
    drawHeader("DATA TERSIMPAN");
    display.setTextSize(2);
    display.setCursor(35, 23);
    display.print("ID: ");
    display.println(currentPlantID);
    display.setTextSize(1);
    drawFooter("1=Cek Lagi  2=Home");
    break;

  case STATE_VIEW_LIST:
  {
    String plantList[20];
    int plantCount = 0;
    getPlantIDList(plantList, &plantCount);

    char header[20];
    sprintf(header, "DATA [%d/%d]", listSelection + 1, plantCount);
    drawHeader(plantCount > 0 ? header : "DATA TANAMAN");
    display.setCursor(0, 14);

    if (plantCount == 0)
    {
      display.println("Tidak ada data");
      display.println("tersimpan");
    }
    else
    {
      int start = max(0, listSelection - 2);
      int end = min(plantCount, start + 4);
      for (int i = start; i < end; i++)
      {
        if (i == listSelection)
          display.print(">");
        else
          display.print(" ");
        display.println(" ID: " + plantList[i]);
      }
    }

    drawFooter(plantCount > 0 ? "A/B #=OK D=Hapus *=Home" : "*=Kembali");
  }
  break;

  case STATE_VIEW_MEASUREMENT:
  {
    int count = getMeasurementCount(currentPlantID);

    char header[20];
    sprintf(header, "ID:%s [%d/%d]", currentPlantID.c_str(), measurementSelection + 1, count);
    drawHeader(count > 0 ? header : ("ID:" + currentPlantID).c_str());
    display.setCursor(0, 14);

    if (count == 0)
    {
      display.println("Tidak ada data");
    }
    else
    {
      int start = max(0, measurementSelection - 2);
      int end = min(count, start + 4);
      for (int i = start; i < end; i++)
      {
        if (i == measurementSelection)
          display.print(">");
        else
          display.print(" ");
        MeasurementData data;
        getMeasurementByIndex(currentPlantID, i, &data);
        String waktu = data.waktuPengambilan;
        display.println(" " + waktu.substring(0, 16));
      }
    }

    drawFooter(count > 0 ? "A/B #=OK D=Hapus *=Home" : "*=Kembali");
  }
  break;

  case STATE_VIEW_DETAIL:
  {
    MeasurementData data;
    getMeasurementByIndex(currentPlantID, measurementSelection, &data);

    char header[20];
    char modeChar = displayMode;
    sprintf(header, "ID:%s [%c]#%d", currentPlantID.c_str(), modeChar, measurementSelection + 1);
    drawHeader(header);
    display.setCursor(0, 14);

    if (displayMode == 'A')
    {
      display.println("pH: " + String(data.ph, 1));
      display.println("Nitrogen:" + String(data.nitrogen, 0) + " mg/kg");
      display.println("Fosfor: " + String(data.fosfor, 0) + " mg/kg");
      display.println("Kalium: " + String(data.kalium, 0) + " mg/kg");
    }
    else if (displayMode == 'B')
    {
      display.println("Suhu:" + String(data.suhu, 1) + " C");
      display.println("Kelembaban:" + String(data.kelembaban, 1) + " %");
      display.println("Konduktivitas:");
      display.println(" " + String(data.konduktivitas, 0) + " uS/cm");
    }
    else if (displayMode == 'C')
    {
      String koordinatStr = String(data.latitude, 6) + ", " + String(data.longitude, 6);
      display.println("Koordinat:");
      display.println(" " + koordinatStr);
      String waktu = data.waktuPengambilan;
      display.println("Tanggal: " + waktu.substring(0, 10));
      display.println("Waktu: " + waktu.substring(11));
    }

    drawFooter("A/B/C *=Kembali");
  }
  break;

  case STATE_DELETE_MENU:
    drawHeader("HAPUS DATA");
    display.setCursor(0, 14);
    if (deleteMode == 0)
    {
      display.println("Hapus semua data");
      display.println("ID: " + currentPlantID);
      int count = getMeasurementCount(currentPlantID);
      display.println("Total: " + String(count) + " data");
    }
    else
    {
      display.println("Hapus data #" + String(measurementSelection + 1));
      display.println("dari ID: " + currentPlantID);
    }
    drawFooter("1=Ya 2=Batal *=Kembali");
    break;

  case STATE_DELETE_CONFIRM:
    drawHeader("BERHASIL");
    display.setTextSize(2);
    display.setCursor(20, 25);
    display.println("Data");
    display.setCursor(15, 40);
    display.println("Dihapus");
    display.setTextSize(1);
    drawFooter("#=Kembali");
    break;

  case STATE_SYNC_MENU:
    drawHeader("KIRIM DATA");
    display.setCursor(0, 14);
    if (wifiConnecting || WiFi.status() != WL_CONNECTED)
    {
      display.println("WiFi: Connecting..");
      String ssid = lastUsedSSID.length() > 0 ? lastUsedSSID : DEFAULT_WIFI_SSID;
      if (ssid.length() > 18)
        ssid = ssid.substring(0, 18) + "..";
      display.println("SSID: " + ssid);
    }
    else
    {
      display.println("WiFi: Connected");
      String ssid = WiFi.SSID();
      if (ssid.length() > 18)
        ssid = ssid.substring(0, 18) + "..";
      display.println("SSID: " + ssid);
    }
    display.println("1. Pilih WiFi");
    if (isFirebaseReady())
    {
      display.print("2. Kirim Data");
    }
    else
    {
      display.print("2. Kirim (Tunggu..)");
    }
    drawFooter("*=Kembali");
    break;

  case STATE_WIFI_SELECT:
  {
    char header[20];
    sprintf(header, "PILIH WiFi [%d/%d]", wifiSelection + 1, wifiCount);
    drawHeader(header);
    display.setCursor(0, 14);

    int start = max(0, wifiSelection - 2);
    int end = min(wifiCount, start + 4);
    for (int i = start; i < end; i++)
    {
      if (i == wifiSelection)
        display.print(">");
      else
        display.print(" ");
      String ssid = wifiList[i].ssid;
      if (ssid.length() > 19)
        ssid = ssid.substring(0, 16) + "..";
      display.print(" " + ssid);
      if (wifiList[i].ssid == lastUsedSSID)
        display.print("*");
      display.println();
    }

    drawFooter("A/B #=OK C=Scan D=Del");
  }
  break;

  case STATE_WIFI_SCAN:
  {
    char header[20];
    sprintf(header, "SCAN [%d/%d]", wifiSelection + 1, wifiScanCount);
    drawHeader(header);
    display.setCursor(0, 14);

    if (wifiScanCount == 0)
    {
      display.println("Tidak ada WiFi");
    }
    else
    {
      int start = max(0, wifiSelection - 2);
      int end = min(wifiScanCount, start + 4);
      for (int i = start; i < end; i++)
      {
        if (i == wifiSelection)
          display.print(">");
        else
          display.print(" ");
        String ssid = wifiScanResults[i];
        if (ssid.length() > 19)
          ssid = ssid.substring(0, 19);
        display.println(" " + ssid);
      }
    }

    drawFooter("A/B #=Pilih *=Kembali");
  }
  break;

  case STATE_WIFI_INPUT_PASS:
  {
    drawHeader("INPUT PASSWORD");
    display.setCursor(0, 14);
    display.println("SSID:");
    String ssid = selectedSSID;
    if (ssid.length() > 20)
      ssid = ssid.substring(0, 17) + "..";
    display.println(" " + ssid);
    display.println();
    display.println("Pass: " + inputBuffer);
    drawFooter("#=OK  *=Batal  D=Del");
  }
  break;

  case STATE_SYNCING:
  {
    String plantList[20];
    int plantCount = 0;
    getPlantIDList(plantList, &plantCount);

    drawHeader("MENGIRIM...");
    display.setCursor(0, 14);

    int progress = (menuSelection * 100) / max(1, plantCount);
    display.print("[");
    for (int i = 0; i < 18; i++)
    {
      display.print(i < (progress * 18 / 100) ? "=" : " ");
    }
    display.println("]");
    display.println(String(progress) + "%");
    display.println();
    display.println("Plant ID " + String(menuSelection) + "/" + String(plantCount));
    drawFooter("Mohon tunggu...");
  }
  break;

  case STATE_SYNC_DONE:
    drawHeader("BERHASIL");
    display.setTextSize(2);
    display.setCursor(30, 25);
    display.print(menuSelection);
    display.println(" Data");
    display.setTextSize(1);
    display.setCursor(40, 42);
    display.println("Terkirim");
    drawFooter("#=Home");
    break;

  case STATE_SYNC_FAILED:
    drawHeader("GAGAL");
    display.setCursor(0, 20);
    display.println("Pengiriman gagal");
    display.println("Cek koneksi WiFi");
    drawFooter("1=Coba  2=Home");
    break;
  }

  display.display();
}

// ==============================================
// 10. FUNGSI STATE MACHINE
// ==============================================

void handleKeypress(char key)
{
  unsigned long currentTime = millis();

  if (currentTime - lastKeyTime < DEBOUNCE_DELAY)
  {
    if (key == lastKey)
    {
      Serial.printf("DEBOUNCED: %c (%dms / %dms)\n",
                    key, (int)(currentTime - lastKeyTime), DEBOUNCE_DELAY);
      return;
    }
  }

  lastKeyTime = currentTime;
  lastKey = key;

  Serial.printf("KEY: %c (State:%d Time:%lu)\n", key, currentState, currentTime);

  needsDisplayUpdate = true;

  switch (currentState)
  {
  case STATE_SPLASH:
    currentState = STATE_MAIN_MENU;
    break;

  case STATE_MAIN_MENU:
    if (key == '1')
    {
      currentState = STATE_INPUT_ID;
      inputBuffer = "";
    }
    else if (key == '2')
    {
      currentState = STATE_VIEW_LIST;
      listSelection = 0;
    }
    else if (key == '3')
    {
      currentState = STATE_SYNC_MENU;
      wifiConnecting = true;
      String ssid = lastUsedSSID.length() > 0 ? lastUsedSSID : DEFAULT_WIFI_SSID;
      String pass = getWiFiPassword(ssid);
      if (pass.length() == 0)
        pass = DEFAULT_WIFI_PASS;
      Serial.printf("Auto-connecting to: %s\n", ssid.c_str());
      WiFi.begin(ssid.c_str(), pass.c_str());
    }
    break;

  case STATE_INPUT_ID:
    if (key == '#')
    {
      if (inputBuffer.length() > 0 && inputBuffer.toInt() <= 999)
      {
        currentPlantID = inputBuffer;
        currentState = STATE_MEASURING;
        needsDisplayUpdate = true;
        updateDisplay();

        measureAllSensors();

        displayMode = 'A';
        currentState = STATE_DISPLAY_DATA;
        needsDisplayUpdate = true;
      }
    }
    else if (key == '*')
    {
      currentState = STATE_MAIN_MENU;
      inputBuffer = "";
    }
    else if (key == 'D')
    {
      if (inputBuffer.length() > 0)
        inputBuffer.remove(inputBuffer.length() - 1);
    }
    else if (key >= '0' && key <= '9')
    {
      if (inputBuffer.length() < 3)
        inputBuffer += key;
    }
    break;

  case STATE_DISPLAY_DATA:
    if (key == 'A')
    {
      displayMode = 'A';
    }
    else if (key == 'B')
    {
      displayMode = 'B';
    }
    else if (key == 'C')
    {
      displayMode = 'C';
    }
    else if (key == '#')
    {
      if (saveMeasurementToFile(currentPlantID, currentMeasurement))
      {
        currentState = STATE_SAVE_CONFIRM;
      }
    }
    else if (key == '*')
    {
      currentState = STATE_INPUT_ID;
      inputBuffer = "";
    }
    break;

  case STATE_SAVE_CONFIRM:
    if (key == '1')
    {
      currentState = STATE_INPUT_ID;
      inputBuffer = "";
    }
    else if (key == '2')
    {
      currentState = STATE_MAIN_MENU;
    }
    break;

  case STATE_VIEW_LIST:
  {
    String plantList[20];
    int plantCount = 0;
    getPlantIDList(plantList, &plantCount);

    if (key == 'A')
    {
      if (listSelection > 0)
      {
        listSelection--;
        delay(50);
      }
    }
    else if (key == 'B')
    {
      if (listSelection < plantCount - 1)
      {
        listSelection++;
        delay(50);
      }
    }
    else if (key == '#')
    {
      if (plantCount > 0)
      {
        currentPlantID = plantList[listSelection];
        measurementSelection = 0;
        currentState = STATE_VIEW_MEASUREMENT;
      }
    }
    else if (key == 'D')
    {
      if (plantCount > 0)
      {
        currentPlantID = plantList[listSelection];
        deleteMode = 0;
        currentState = STATE_DELETE_MENU;
      }
    }
    else if (key == '*')
    {
      currentState = STATE_MAIN_MENU;
    }
  }
  break;

  case STATE_VIEW_MEASUREMENT:
  {
    int count = getMeasurementCount(currentPlantID);

    if (key == 'A')
    {
      if (measurementSelection > 0)
      {
        measurementSelection--;
        delay(50);
      }
    }
    else if (key == 'B')
    {
      if (measurementSelection < count - 1)
      {
        measurementSelection++;
        delay(50);
      }
    }
    else if (key == '#')
    {
      if (count > 0)
      {
        displayMode = 'A';
        currentState = STATE_VIEW_DETAIL;
      }
    }
    else if (key == 'D')
    {
      if (count > 0)
      {
        deleteMode = 1;
        currentState = STATE_DELETE_MENU;
      }
    }
    else if (key == '*')
    {
      currentState = STATE_VIEW_LIST;
    }
  }
  break;

  case STATE_VIEW_DETAIL:
    if (key == 'A')
    {
      displayMode = 'A';
    }
    else if (key == 'B')
    {
      displayMode = 'B';
    }
    else if (key == 'C')
    {
      displayMode = 'C';
    }
    else if (key == '*')
    {
      currentState = STATE_VIEW_MEASUREMENT;
    }
    break;

  case STATE_DELETE_MENU:
    if (key == '1')
    {
      bool success = (deleteMode == 0)
                         ? deletePlantData(currentPlantID)
                         : deleteMeasurementByIndex(currentPlantID, measurementSelection);

      currentState = success
                         ? STATE_DELETE_CONFIRM
                         : (deleteMode == 0 ? STATE_VIEW_LIST : STATE_VIEW_MEASUREMENT);
    }
    else if (key == '2' || key == '*')
    {
      currentState = (deleteMode == 0) ? STATE_VIEW_LIST : STATE_VIEW_MEASUREMENT;
    }
    break;

  case STATE_DELETE_CONFIRM:
    if (key == '#')
    {
      listSelection = 0;
      measurementSelection = 0;
      currentState = STATE_VIEW_LIST;
    }
    break;

  case STATE_SYNC_MENU:
    if (key == '1')
    {
      wifiSelection = 0;
      currentState = STATE_WIFI_SELECT;
    }
    else if (key == '2')
    {
      if (isFirebaseReady())
      {
        Serial.println("Starting upload...");
        currentState = STATE_SYNCING;
        menuSelection = 0;
        needsDisplayUpdate = true;
        updateDisplay();

        int result = uploadAllDataToFirebase();

        if (result > 0)
        {
          currentState = STATE_SYNC_DONE;
          menuSelection = result;
        }
        else if (result == 0)
        {
          currentState = STATE_MAIN_MENU;
        }
        else
        {
          currentState = STATE_SYNC_FAILED;
        }
        needsDisplayUpdate = true;
      }
    }
    else if (key == '*')
    {
      WiFi.disconnect();
      wifiConnecting = false;
      currentState = STATE_MAIN_MENU;
    }
    break;

  case STATE_WIFI_SELECT:
    if (key == 'A')
    {
      if (wifiSelection > 0)
      {
        wifiSelection--;
        delay(50);
      }
    }
    else if (key == 'B')
    {
      if (wifiSelection < wifiCount - 1)
      {
        wifiSelection++;
        delay(50);
      }
    }
    else if (key == '#')
    {
      lastUsedSSID = wifiList[wifiSelection].ssid;
      saveWiFiConfig();
      WiFi.disconnect();
      WiFi.begin(wifiList[wifiSelection].ssid.c_str(), wifiList[wifiSelection].password.c_str());
      wifiConnecting = true;
      currentState = STATE_SYNC_MENU;
    }
    else if (key == 'C')
    {
      wifiSelection = 0;
      currentState = STATE_WIFI_SCAN;
      needsDisplayUpdate = true;
      updateDisplay();
      scanWiFi();
      needsDisplayUpdate = true;
    }
    else if (key == 'D')
    {
      if (wifiCount > 1)
      {
        deleteWiFiCredential(wifiSelection);
        if (wifiSelection >= wifiCount)
          wifiSelection = wifiCount - 1;
      }
    }
    else if (key == '*')
    {
      currentState = STATE_SYNC_MENU;
    }
    break;

  case STATE_WIFI_SCAN:
    if (key == 'A')
    {
      if (wifiSelection > 0)
      {
        wifiSelection--;
        delay(50);
      }
    }
    else if (key == 'B')
    {
      if (wifiSelection < wifiScanCount - 1)
      {
        wifiSelection++;
        delay(50);
      }
    }
    else if (key == '#')
    {
      selectedSSID = wifiScanResults[wifiSelection];
      inputBuffer = "";
      currentState = STATE_WIFI_INPUT_PASS;
    }
    else if (key == '*')
    {
      currentState = STATE_WIFI_SELECT;
    }
    break;

  case STATE_WIFI_INPUT_PASS:
    if (key == '#')
    {
      if (inputBuffer.length() >= 8)
      {
        addWiFiCredential(selectedSSID, inputBuffer);
        lastUsedSSID = selectedSSID;
        saveWiFiConfig();
        WiFi.disconnect();
        WiFi.begin(selectedSSID.c_str(), inputBuffer.c_str());
        wifiConnecting = true;
        currentState = STATE_SYNC_MENU;
      }
    }
    else if (key == '*')
    {
      currentState = STATE_WIFI_SCAN;
    }
    else if (key == 'D')
    {
      if (inputBuffer.length() > 0)
        inputBuffer.remove(inputBuffer.length() - 1);
    }
    else
    {
      inputBuffer += key;
    }
    break;

  case STATE_SYNC_DONE:
    if (key == '#')
      currentState = STATE_MAIN_MENU;
    break;

  case STATE_SYNC_FAILED:
    if (key == '1')
    {
      currentState = STATE_SYNC_MENU;
    }
    else if (key == '2')
    {
      currentState = STATE_MAIN_MENU;
    }
    break;
  }
}

// ==============================================
// 11. SETUP & LOOP
// ==============================================

void setup()
{
  Serial.begin(115200);
  delay(100);

  Serial.println("\n\n=================================");
  Serial.println("SISTEM MONITORING TANAMAN v3.6");
  Serial.println("MODE: REAL HARDWARE SENSOR");
  Serial.println("=================================\n");

  // Init LittleFS
  if (!LittleFS.begin(true))
  {
    Serial.println("ERROR: LittleFS mount failed");
    while (1)
      ;
  }
  Serial.println("LittleFS OK");

  // Load WiFi config
  loadWiFiConfig();

  // Init OLED
  if (!display.begin(i2c_Address, true))
  {
    Serial.println("ERROR: OLED init failed");
    while (1)
      ;
  }
  Serial.println("OLED OK");

  // Init GPS (UART1)
  Serial1.begin(9600, SERIAL_8N1, 17, 18);
  Serial.println("GPS UART OK");

  // Init Sensor RS485 (UART2)
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);
  Serial2.begin(4800, SERIAL_8N1, 16, 15);
  Serial.println("RS485 OK - Real Hardware Mode");

  // WiFi mode
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  Serial.println("WiFi Mode Set");

  // Tampilkan splash
  currentState = STATE_SPLASH;
  needsDisplayUpdate = true;
  updateDisplay();

  Serial.println("\nSystem Ready");
  Serial.println("Tekan tombol untuk memulai...\n");
  Serial.println("Commands via Serial Monitor:");
  Serial.println("  ADD_WIFI:SSID,PASSWORD");
}

void loop()
{
  // Baca keypad
  char key = keypad.getKey();
  if (key)
    handleKeypress(key);

  // Update GPS
  updateGPS();

  // Auto-connect & Firebase init di SYNC_MENU
  if (currentState == STATE_SYNC_MENU)
  {
    if (millis() - lastWiFiCheck > 1000)
    {
      lastWiFiCheck = millis();

      static int lastWiFiStatus = -1;
      int currentWiFiStatus = WiFi.status();

      if (currentWiFiStatus != lastWiFiStatus)
      {
        lastWiFiStatus = currentWiFiStatus;
        needsDisplayUpdate = true;

        if (currentWiFiStatus == WL_CONNECTED)
        {
          wifiConnecting = false;
          Serial.println("WiFi Connected!");
          Serial.print("  SSID: ");
          Serial.println(WiFi.SSID());
          Serial.print("  IP:   ");
          Serial.println(WiFi.localIP());

          if (!timeSync)
            timeSync = syncTimeWithNTP();
          if (!firebaseInitialized)
            initFirebase();

          needsDisplayUpdate = true;
        }
        else if (currentWiFiStatus == WL_CONNECT_FAILED ||
                 currentWiFiStatus == WL_NO_SSID_AVAIL)
        {
          wifiConnecting = false;
          needsDisplayUpdate = true;
          Serial.println("WiFi Connection Failed");
        }
      }
    }
  }

  // Serial command handler
  if (Serial.available())
  {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("ADD_WIFI:"))
    {
      String data = cmd.substring(9);
      int commaIdx = data.indexOf(',');
      if (commaIdx > 0)
      {
        String ssid = data.substring(0, commaIdx);
        String pass = data.substring(commaIdx + 1);
        addWiFiCredential(ssid, pass);
        Serial.println("WiFi added via Serial: " + ssid);
      }
    }
  }

  // Update display
  updateDisplay();
}
