#include <Arduino.h>
#include <vector>

#if !defined(CONFIG_IDF_TARGET_ESP32C6)
  #error "This sketch targets ESP32-C6. Select: ESP32C6 Dev Module."
#endif
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEAdvertising.h>
#include <BLEServer.h>
#include <BLEService.h>
#include <BLECharacteristic.h>
#include <BLE2902.h>

extern "C" {
  #include "esp_wifi.h"
}

// ============================================================
// WirelessLab32-C6 v0.4.2-c6.1
// Target: ESP32-C6 / ESP32-C6 Dev Module
//
// Wi-Fi:
// - AP scan
// - Beacon lab limitado
// - Portal cautivo educativo
//
// BLE:
// - Control únicamente por Serial
// - Escaneo
// - Listado
// - Advertising BLE estable
//
// El portal es exclusivamente de laboratorio y no persiste datos.
// No almacena información en flash ni SD.
// ============================================================

static const char *FW_VERSION = "0.4.2-c6.1";

// Credenciales exclusivas del laboratorio.
static const char *DEMO_USERNAME = "student";

// Nombre BLE fijo para la prueba estable.
static const char *BLE_DEVICE_NAME = "WirelessLab32-C6";

// Servicio BLE de demostración.
static const char *BLE_SERVICE_UUID =
  "12345678-1234-1234-1234-1234567890ab";

// Datos exclusivamente ficticios para la simulación BlueSnarf.
static const char *BLE_DEMO_CONTACT_1_UUID =
  "12345678-1234-1234-1234-1234567890ac";

static const char *BLE_DEMO_CONTACT_2_UUID =
  "12345678-1234-1234-1234-1234567890ad";

static const char *BLE_DEMO_CONTACT_3_UUID =
  "12345678-1234-1234-1234-1234567890ae";

static const char *BLE_BLUEJACK_MESSAGE_UUID =
  "12345678-1234-1234-1234-1234567890af";

static const char *BLE_DEMO_CONTACTS[] = {
  "BEGIN:VCARD\nFN:Alex Example\nTEL:+1-555-0100\nEMAIL:alex@example.invalid\nEND:VCARD",
  "BEGIN:VCARD\nFN:Sam Student\nTEL:+1-555-0101\nEMAIL:sam@example.invalid\nEND:VCARD",
  "BEGIN:VCARD\nFN:Taylor Teacher\nTEL:+1-555-0102\nEMAIL:taylor@example.invalid\nEND:VCARD"
};

static const uint8_t MAX_BEACON_SSIDS = 5;

// Límite duro editable. Siempre se puede detener antes con: stop
static const uint8_t MAX_WIFI_SPAM_DURATION_SECONDS = 60;

// Advertising BLE genérico; se detiene antes con: ble spam stop
static const uint8_t MAX_BLE_SPAM_DURATION_SECONDS = 60;

struct BleSpamPreset {
  const char *name;
  const char *message;
};

// Presets ficticios. No usar nombres, IDs ni payloads de marcas reales.
static const BleSpamPreset BLE_SPAM_PRESETS[] = {
  {"Lab Beacon Alpha", "WirelessLab32 classroom payload A"},
  {"Lab Beacon Bravo", "WirelessLab32 classroom payload B"},
  {"Lab Beacon Charlie", "WirelessLab32 classroom payload C"}
};

static const size_t BLE_SPAM_PRESET_COUNT =
  sizeof(BLE_SPAM_PRESETS) / sizeof(BLE_SPAM_PRESETS[0]);

static const uint32_t BLE_SPAM_ROTATION_INTERVAL_MS = 3000;

// ============================================================
// Wi-Fi spam lab presets
// Edita estos nombres para preparar la práctica (máximo 5).
// Cada SSID debe contener entre 1 y 32 caracteres.
// ============================================================

static const char *WIFI_SPAM_SSIDS[] = {
  "Pizza Gratis!",
  "Dennys Ultra Mega Free Wifi",
  "Red Corporativa InGen"
};

static const size_t WIFI_SPAM_SSID_COUNT =
  sizeof(WIFI_SPAM_SSIDS) / sizeof(WIFI_SPAM_SSIDS[0]);

static_assert(
  WIFI_SPAM_SSID_COUNT <= MAX_BEACON_SSIDS,
  "WIFI_SPAM_SSIDS cannot contain more than 5 entries"
);

// ============================================================
// Modos
// ============================================================

enum class Mode {
  IDLE,
  WIFI_SCAN,
  BEACON_LAB,
  PORTAL,
  BLE_SERIAL
};

Mode currentMode = Mode::IDLE;

// ============================================================
// Servicios generales
// ============================================================

WebServer webServer(80);
DNSServer dnsServer;

String serialBuffer;

bool portalActive = false;

// ============================================================
// Wi-Fi scan
// ============================================================

struct AccessPointInfo {
  String ssid;
  String bssid;
  int32_t rssi;
  int32_t channel;
};

std::vector<AccessPointInfo> accessPoints;

// ============================================================
// Beacon lab
// ============================================================

std::vector<String> beaconSsids;

uint8_t beaconChannel = 6;
uint32_t beaconStopAt = 0;
uint16_t beaconSequence = 0;

// ============================================================
// BLE
// ============================================================

struct BleDeviceInfo {
  String address;
  String name;
  String serviceUuid;
  String manufacturerData;
  int rssi;
};

std::vector<BleDeviceInfo> bleDevices;

BLEScan *bleScan = nullptr;
BLEAdvertising *bleAdvertising = nullptr;
BLEServer *bleServer = nullptr;
BLEService *bleService = nullptr;
BLECharacteristic *bleDemoContacts[3] = {nullptr, nullptr, nullptr};
BLECharacteristic *bleBluejackMessage = nullptr;

bool bleInitialized = false;
bool bleAdvertisingActive = false;
uint32_t bleAdvertisingStopAt = 0;
uint32_t bleSpamNextRotationAt = 0;
size_t bleSpamPresetIndex = 0;
bool bleSpamActive = false;
bool portalRoutesConfigured = false;

// ============================================================
// Forward declarations
// ============================================================

void stopBleOperations();
void stopAll();

// ============================================================
// Helpers
// ============================================================

String getModeName() {
  switch (currentMode) {
    case Mode::IDLE:
      return "IDLE";

    case Mode::WIFI_SCAN:
      return "WIFI_SCAN";

    case Mode::BEACON_LAB:
      return "BEACON_LAB";

    case Mode::PORTAL:
      return "PORTAL";

    case Mode::BLE_SERIAL:
      return "BLE_SERIAL";
  }

  return "UNKNOWN";
}

void printPrompt() {
  Serial.print("> ");
}

String bytesToHex(
  const uint8_t *data,
  size_t length
) {
  static const char *HEX_DIGITS = "0123456789ABCDEF";

  String output;
  output.reserve(length * 2);

  for (size_t i = 0; i < length; i++) {
    output += HEX_DIGITS[(data[i] >> 4) & 0x0F];
    output += HEX_DIGITS[data[i] & 0x0F];
  }

  return output;
}

// ============================================================
// BLE callbacks
// ============================================================

class WirelessLabBleCallbacks
  : public BLEAdvertisedDeviceCallbacks {

public:

  void onResult(
    BLEAdvertisedDevice device
  ) override {
    BleDeviceInfo result;

    result.address =
      device.getAddress().toString().c_str();

    result.rssi =
      device.getRSSI();

    if (device.haveName()) {
      result.name =
        device.getName().c_str();
    }

    if (device.haveServiceUUID()) {
      result.serviceUuid =
        device.getServiceUUID().toString().c_str();
    }

    if (device.haveManufacturerData()) {
      String manufacturerData =
        device.getManufacturerData();

      result.manufacturerData =
        bytesToHex(
          reinterpret_cast<const uint8_t *>(
            manufacturerData.c_str()
          ),
          manufacturerData.length()
        );
    }

    // Actualizar un dispositivo existente.
    for (BleDeviceInfo &existing : bleDevices) {
      if (existing.address == result.address) {
        existing = result;
        return;
      }
    }

    // Evitar crecimiento ilimitado.
    if (bleDevices.size() < 100) {
      bleDevices.push_back(result);
    }
  }
};

WirelessLabBleCallbacks bleCallbacks;

// ============================================================
// BLE initialization
// ============================================================

void initializeBle() {
  if (bleInitialized) {
    return;
  }

  Serial.println("Initializing BLE stack...");

  BLEDevice::init(BLE_DEVICE_NAME);

  // Scanner BLE.
  bleScan =
    BLEDevice::getScan();

  bleScan->setAdvertisedDeviceCallbacks(
    &bleCallbacks,
    true
  );

  bleScan->setActiveScan(true);
  bleScan->setInterval(100);
  bleScan->setWindow(80);

  // Servidor y servicio mínimos para advertising estable.
  bleServer =
    BLEDevice::createServer();

  bleService =
    bleServer->createService(
      BLE_SERVICE_UUID
    );

  bleDemoContacts[0] =
    bleService->createCharacteristic(
      BLE_DEMO_CONTACT_1_UUID,
      BLECharacteristic::PROPERTY_READ
    );

  bleDemoContacts[1] =
    bleService->createCharacteristic(
      BLE_DEMO_CONTACT_2_UUID,
      BLECharacteristic::PROPERTY_READ
    );

  bleDemoContacts[2] =
    bleService->createCharacteristic(
      BLE_DEMO_CONTACT_3_UUID,
      BLECharacteristic::PROPERTY_READ
    );

  for (size_t i = 0; i < 3; i++) {
    bleDemoContacts[i]->setValue(BLE_DEMO_CONTACTS[i]);
  }

  bleBluejackMessage =
    bleService->createCharacteristic(
      BLE_BLUEJACK_MESSAGE_UUID,
      BLECharacteristic::PROPERTY_READ |
      BLECharacteristic::PROPERTY_NOTIFY
    );

  bleBluejackMessage->addDescriptor(new BLE2902());
  bleBluejackMessage->setValue("WirelessLab32 classroom message");

  bleService->start();

  bleAdvertising =
    BLEDevice::getAdvertising();

  /*
    Primera fase de estabilidad: anunciar solo el nombre que
    BLEDevice configura en el objeto global. El servicio GATT
    existe y puede descubrirse al conectar, pero su UUID de
    128 bits aún no se agrega al paquete legacy de 31 bytes.

    Nombre + UUID de 128 bits + flags + TX power no caben juntos
    en esos 31 bytes. El UUID se incorporará en una fase separada.
  */
  bleAdvertising->setScanResponse(false);

  bleInitialized = true;

  Serial.println("BLE stack initialized.");
}

// ============================================================
// BLE operations
// ============================================================

void stopBleScan() {
  if (bleScan != nullptr) {
    bleScan->stop();
  }
}

void stopBleAdvertising() {
  if (
    bleAdvertising != nullptr &&
    bleAdvertisingActive
  ) {
    bleAdvertising->stop();
    delay(100);

    Serial.println(
      "BLE advertising stopped."
    );
  }

  bleAdvertisingActive = false;
  bleAdvertisingStopAt = 0;
  bleSpamNextRotationAt = 0;
  bleSpamPresetIndex = 0;
  bleSpamActive = false;
}

void stopBleOperations() {
  stopBleScan();
  stopBleAdvertising();
}

void enterBleSerialMode() {
  /*
    No llamamos stopAll() aquí después de inicializar BLE,
    porque stopAll() también apaga Wi-Fi y puede interferir
    con operaciones previamente inicializadas.

    Primero detenemos los servicios Wi-Fi.
  */

  dnsServer.stop();
  webServer.stop();

  WiFi.softAPdisconnect(true);
  // Apagar la radio sin borrar las credenciales guardadas en NVS.
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);

  portalActive = false;
  beaconStopAt = 0;

  currentMode = Mode::BLE_SERIAL;

  initializeBle();

  Serial.println();
  Serial.println("BLE serial mode ready.");
  Serial.println("Commands:");
  Serial.println("  ble scan");
  Serial.println("  ble list");
  Serial.println("  ble advertise start");
  Serial.println("  ble advertise stop");
  Serial.println("  ble exit");
}

void runBleScan() {
  if (currentMode != Mode::BLE_SERIAL) {
    enterBleSerialMode();
  }

  initializeBle();

  stopBleAdvertising();
  stopBleScan();

  bleDevices.clear();
  bleScan->clearResults();

  Serial.println(
    "Scanning BLE devices for 8 seconds..."
  );

  BLEScanResults *results =
    bleScan->start(
      8,
      false
    );

  if (results != nullptr) {
    Serial.printf(
      "Raw BLE advertisements found: %d\n",
      results->getCount()
    );
  } else {
    Serial.println(
      "BLE scan finished without a results object."
    );
  }

  Serial.printf(
    "Unique devices stored: %u\n",
    static_cast<unsigned>(
      bleDevices.size()
    )
  );

  bleScan->clearResults();
}

void printBleDevices() {
  if (bleDevices.empty()) {
    Serial.println(
      "No stored BLE scan results."
    );

    return;
  }

  Serial.println();
  Serial.println(
    "IDX  RSSI  ADDRESS            NAME"
  );

  Serial.println(
    "--------------------------------------------------------------------------"
  );

  for (size_t i = 0; i < bleDevices.size(); i++) {
    const BleDeviceInfo &device =
      bleDevices[i];

    Serial.printf(
      "%3u  %4d  %-17s  %s\n",
      static_cast<unsigned>(i),
      device.rssi,
      device.address.c_str(),
      device.name.length()
        ? device.name.c_str()
        : "<unnamed>"
    );

    if (device.serviceUuid.length()) {
      Serial.printf(
        "                         UUID: %s\n",
        device.serviceUuid.c_str()
      );
    }

    if (device.manufacturerData.length()) {
      Serial.printf(
        "                         MFG : %s\n",
        device.manufacturerData.c_str()
      );
    }
  }
}

void startBleAdvertising() {
  if (currentMode != Mode::BLE_SERIAL) {
    enterBleSerialMode();
  }

  initializeBle();

  stopBleScan();
  stopBleAdvertising();

  Serial.println(
    "Starting stable BLE advertising..."
  );

  /*
    Esta implementación utiliza el objeto global de advertising
    creado durante initializeBle().

    No crea objetos BLEAdvertisementData locales.
    No usa manufacturer data.
    No usa scan response personalizado.
    No cambia dinámicamente el nombre.
  */

  bleAdvertising->start();

  bleAdvertisingActive = true;
  bleAdvertisingStopAt = 0;

  Serial.println(
    "BLE advertising started."
  );

  Serial.printf(
    "Name: %s\n",
    BLE_DEVICE_NAME
  );

  Serial.printf(
    "GATT Service UUID: %s (not in advertising packet)\n",
    BLE_SERVICE_UUID
  );
}

void publishBleSpamPreset() {
  if (BLE_SPAM_PRESET_COUNT == 0 || bleBluejackMessage == nullptr) {
    return;
  }

  const BleSpamPreset &preset =
    BLE_SPAM_PRESETS[bleSpamPresetIndex];

  String payload = String(preset.name) + ": " + preset.message;
  bleBluejackMessage->setValue(payload.c_str());
  bleBluejackMessage->notify();

  Serial.printf(
    "[BLE SPAM LAB] Preset %u/%u: %s\n",
    static_cast<unsigned>(bleSpamPresetIndex + 1),
    static_cast<unsigned>(BLE_SPAM_PRESET_COUNT),
    preset.name
  );

  bleSpamPresetIndex =
    (bleSpamPresetIndex + 1) % BLE_SPAM_PRESET_COUNT;
}

void printBleSpamPresets() {
  Serial.println("BLE spam lab presets:");

  for (size_t i = 0; i < BLE_SPAM_PRESET_COUNT; i++) {
    Serial.printf(
      "  %u: %s -> %s\n",
      static_cast<unsigned>(i + 1),
      BLE_SPAM_PRESETS[i].name,
      BLE_SPAM_PRESETS[i].message
    );
  }
}

void startBleSpamLab(uint8_t seconds) {
  if (seconds == 0 || seconds > MAX_BLE_SPAM_DURATION_SECONDS) {
    seconds = MAX_BLE_SPAM_DURATION_SECONDS;
  }

  startBleAdvertising();

  bleAdvertisingStopAt =
    millis() + static_cast<uint32_t>(seconds) * 1000UL;

  bleSpamActive = true;
  bleSpamPresetIndex = 0;
  publishBleSpamPreset();
  bleSpamNextRotationAt = millis() + BLE_SPAM_ROTATION_INTERVAL_MS;

  Serial.println();
  Serial.println("BLE advertising lab started.");
  Serial.println("Generic educational payload only; no branded pop-up payloads.");
  Serial.printf("Preset rotation: %lu ms\n", BLE_SPAM_ROTATION_INTERVAL_MS);
  Serial.printf("Duration: %u seconds\n", seconds);
}

void serviceBleAdvertisingLab() {
  if (!bleAdvertisingActive || bleAdvertisingStopAt == 0) {
    return;
  }

  if (
    bleSpamActive &&
    static_cast<int32_t>(millis() - bleSpamNextRotationAt) >= 0
  ) {
    publishBleSpamPreset();
    bleSpamNextRotationAt = millis() + BLE_SPAM_ROTATION_INTERVAL_MS;
  }

  if (
    static_cast<int32_t>(millis() - bleAdvertisingStopAt) >= 0
  ) {
    Serial.println();
    Serial.println("BLE advertising lab timeout reached.");
    stopBleAdvertising();
    printPrompt();
  }
}

void startBlueSnarfDemo() {
  startBleAdvertising();

  Serial.println();
  Serial.println("BlueSnarf safe simulation ready.");
  Serial.println("This ESP32 is the intentionally vulnerable device.");
  Serial.println("All exposed contact data is fictional.");
  Serial.println("Contact characteristics:");
  Serial.printf("  1: %s\n", BLE_DEMO_CONTACT_1_UUID);
  Serial.printf("  2: %s\n", BLE_DEMO_CONTACT_2_UUID);
  Serial.printf("  3: %s\n", BLE_DEMO_CONTACT_3_UUID);
  Serial.println("Connect with nRF Connect and read each vCard.");
}

void startBluejackingDemo() {
  startBleAdvertising();

  Serial.println();
  Serial.println("Bluejacking safe BLE simulation ready.");
  Serial.println("Connect with nRF Connect and enable notifications on:");
  Serial.println(BLE_BLUEJACK_MESSAGE_UUID);
  Serial.println("Then use: ble bluejack send <message>");
}

void sendBluejackDemoMessage(String message) {
  message.trim();

  if (message.length() == 0 || message.length() > 80) {
    Serial.println("Message must contain between 1 and 80 characters.");
    return;
  }

  initializeBle();
  bleBluejackMessage->setValue(message.c_str());
  bleBluejackMessage->notify();

  Serial.printf("[BLUEJACK LAB] Notification: %s\n", message.c_str());
  Serial.println("Delivered only to connected clients that opted into notifications.");
}

// ============================================================
// Wi-Fi stop helpers
// ============================================================

void stopPortal() {
  dnsServer.stop();
  webServer.stop();

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  portalActive = false;
}

void stopAll() {
  stopPortal();
  stopBleOperations();

  beaconStopAt = 0;

  WiFi.disconnect(true, false);
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);

  currentMode = Mode::IDLE;

  Serial.println(
    "Stopped. Mode: IDLE"
  );
}

// ============================================================
// Wi-Fi AP scan
// ============================================================

void runWifiScan() {
  stopAll();

  currentMode = Mode::WIFI_SCAN;

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);

  delay(150);

  Serial.println(
    "Scanning Wi-Fi access points..."
  );

  int networkCount =
    WiFi.scanNetworks(
      false,
      true
    );

  accessPoints.clear();

  if (networkCount <= 0) {
    Serial.println(
      "No access points found."
    );
  } else {
    accessPoints.reserve(networkCount);

    for (int i = 0; i < networkCount; i++) {
      AccessPointInfo accessPoint;

      accessPoint.ssid =
        WiFi.SSID(i);

      accessPoint.bssid =
        WiFi.BSSIDstr(i);

      accessPoint.rssi =
        WiFi.RSSI(i);

      accessPoint.channel =
        WiFi.channel(i);

      accessPoints.push_back(
        accessPoint
      );
    }

    Serial.printf(
      "Found %d access point(s).\n",
      networkCount
    );

    Serial.println(
      "Use: wifi list"
    );
  }

  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);

  currentMode = Mode::IDLE;
}

void printWifiList() {
  if (accessPoints.empty()) {
    Serial.println(
      "No stored Wi-Fi scan results."
    );

    return;
  }

  Serial.println();
  Serial.println(
    "IDX  CH   RSSI  BSSID              SSID"
  );

  Serial.println(
    "----------------------------------------------------------"
  );

  for (size_t i = 0; i < accessPoints.size(); i++) {
    const AccessPointInfo &accessPoint =
      accessPoints[i];

    Serial.printf(
      "%3u  %2ld  %4ld  %-17s  %s\n",
      static_cast<unsigned>(i),
      static_cast<long>(
        accessPoint.channel
      ),
      static_cast<long>(
        accessPoint.rssi
      ),
      accessPoint.bssid.c_str(),
      accessPoint.ssid.length()
        ? accessPoint.ssid.c_str()
        : "<hidden>"
    );
  }
}

// ============================================================
// Beacon lab
// ============================================================

void sendBeaconFrame(
  const String &ssid
) {
  if (
    ssid.length() == 0 ||
    ssid.length() > 32
  ) {
    return;
  }

  uint8_t frame[128] = {0};
  size_t position = 0;

  const uint8_t header[] = {
    0x80, 0x00,
    0x00, 0x00,

    0xff, 0xff, 0xff,
    0xff, 0xff, 0xff,

    0x02, 0x00, 0x00,
    0x00, 0x00, 0x01,

    0x02, 0x00, 0x00,
    0x00, 0x00, 0x01,

    0x00, 0x00
  };

  memcpy(
    frame + position,
    header,
    sizeof(header)
  );

  position += sizeof(header);

  frame[22] =
    static_cast<uint8_t>(
      (beaconSequence & 0x0F) << 4
    );

  frame[23] =
    static_cast<uint8_t>(
      (beaconSequence >> 4) & 0xFF
    );

  beaconSequence++;

  // Timestamp.
  memset(
    frame + position,
    0,
    8
  );

  position += 8;

  // Beacon interval.
  frame[position++] = 0x64;
  frame[position++] = 0x00;

  // Capability information.
  frame[position++] = 0x01;
  frame[position++] = 0x04;

  // SSID element.
  frame[position++] = 0x00;

  frame[position++] =
    static_cast<uint8_t>(
      ssid.length()
    );

  memcpy(
    frame + position,
    ssid.c_str(),
    ssid.length()
  );

  position += ssid.length();

  // Supported rates.
  const uint8_t rates[] = {
    0x01, 0x08,
    0x82, 0x84,
    0x8b, 0x96,
    0x0c, 0x12,
    0x18, 0x24
  };

  memcpy(
    frame + position,
    rates,
    sizeof(rates)
  );

  position += sizeof(rates);

  // DS parameter set.
  frame[position++] = 0x03;
  frame[position++] = 0x01;
  frame[position++] = beaconChannel;

  esp_wifi_80211_tx(
    WIFI_IF_AP,
    frame,
    position,
    false
  );
}

void loadWifiSpamPresets() {
  beaconSsids.clear();

  for (size_t i = 0; i < WIFI_SPAM_SSID_COUNT; i++) {
    String ssid = WIFI_SPAM_SSIDS[i];

    if (ssid.length() > 0 && ssid.length() <= 32) {
      beaconSsids.push_back(ssid);
    }
  }

  Serial.printf(
    "Loaded %u Wi-Fi spam lab preset(s).\n",
    static_cast<unsigned>(beaconSsids.size())
  );
}

void startBeaconLab(
  uint8_t seconds
) {
  if (beaconSsids.empty()) {
    Serial.println(
      "Add SSIDs first:"
    );

    Serial.println(
      "wifi beacon add <ssid>"
    );

    return;
  }

  if (
    seconds == 0 ||
    seconds > MAX_WIFI_SPAM_DURATION_SECONDS
  ) {
    seconds = MAX_WIFI_SPAM_DURATION_SECONDS;
  }

  stopAll();

  currentMode =
    Mode::BEACON_LAB;

  WiFi.mode(WIFI_AP);

  bool started =
    WiFi.softAP(
      "WirelessLab32-C6-Beacon",
      "WirelessLab32!",
      beaconChannel,
      true,
      1
    );

  if (!started) {
    Serial.println(
      "Could not initialize Wi-Fi beacon lab."
    );

    currentMode = Mode::IDLE;
    return;
  }

  esp_wifi_set_channel(
    beaconChannel,
    WIFI_SECOND_CHAN_NONE
  );

  beaconStopAt =
    millis() +
    static_cast<uint32_t>(
      seconds
    ) * 1000UL;

  Serial.println(
    "Beacon lab started."
  );

  Serial.printf(
    "Channel: %u\n",
    beaconChannel
  );

  Serial.printf(
    "Duration: %u seconds\n",
    seconds
  );

  Serial.printf(
    "SSID count: %u\n",
    static_cast<unsigned>(
      beaconSsids.size()
    )
  );
}

void serviceBeaconLab() {
  if (
    currentMode !=
    Mode::BEACON_LAB
  ) {
    return;
  }

  if (
    beaconStopAt != 0 &&
    static_cast<int32_t>(
      millis() - beaconStopAt
    ) >= 0
  ) {
    Serial.println();
    Serial.println(
      "Beacon lab timeout reached."
    );

    stopAll();
    printPrompt();

    return;
  }

  static uint32_t lastTransmission = 0;

  if (
    millis() -
    lastTransmission <
    120
  ) {
    return;
  }

  lastTransmission =
    millis();

  for (
    const String &ssid :
    beaconSsids
  ) {
    sendBeaconFrame(ssid);
  }
}

// ============================================================
// Captive portal lab
// ============================================================

String portalPage() {
  return R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">

  <meta
    name="viewport"
    content="width=device-width,initial-scale=1"
  >

  <title>WirelessLab32 Portal Lab</title>

  <style>
    body {
      font-family: system-ui, sans-serif;
      max-width: 680px;
      margin: 3rem auto;
      padding: 0 1rem;
      line-height: 1.5;
    }

    .card {
      border: 1px solid #bbb;
      border-radius: 14px;
      padding: 1.4rem;
    }

    .brand {
      display: flex;
      align-items: center;
      gap: 0.8rem;
      margin-bottom: 1rem;
    }

    .brand svg {
      width: 64px;
      height: 64px;
      flex: 0 0 auto;
    }

    .brand h1 {
      margin: 0;
    }

    .warning {
      background: #fff4cc;
      border: 1px solid #d6aa00;
      border-radius: 10px;
      padding: 0.8rem;
    }

    input,
    button {
      width: 100%;
      box-sizing: border-box;
      padding: 0.8rem;
      margin: 0.35rem 0;
      border: 1px solid #bbb;
      border-radius: 10px;
    }

    button {
      font-weight: 700;
      cursor: pointer;
    }

    code {
      background: #eee;
      padding: 0.15rem 0.35rem;
      border-radius: 5px;
    }
  </style>
</head>

<body>
  <div class="card">
    <div class="brand">
      <svg viewBox="0 0 64 64" role="img" aria-label="WirelessLab32 logo">
        <rect width="64" height="64" rx="14" fill="#123a63"/>
        <circle cx="32" cy="43" r="4" fill="#56d6ff"/>
        <path d="M22 34a14 14 0 0 1 20 0" fill="none" stroke="#56d6ff" stroke-width="4" stroke-linecap="round"/>
        <path d="M14 25a25 25 0 0 1 36 0" fill="none" stroke="#fff" stroke-width="4" stroke-linecap="round"/>
        <path d="M32 12v6M29 15h6" stroke="#fff" stroke-width="3" stroke-linecap="round"/>
      </svg>
      <h1>WirelessLab32 Portal Lab</h1>
    </div>

    <p class="warning">
      <strong>Training environment.</strong>
      Do not enter real credentials.
    </p>

    <p>
      Use the classroom username and any fictional password:
    </p>

    <p>
      Username:
      <code>student</code>
      <br>

      Password:
      <code>choose any training password</code>
    </p>

    <form
      method="post"
      action="/login"
      autocomplete="off"
    >
      <label for="username">
        Username
      </label>

      <input
        id="username"
        name="username"
        maxlength="32"
        required
      >

      <label for="password">
        Password
      </label>

      <input
        id="password"
        name="password"
        type="password"
        maxlength="64"
        required
      >

      <button type="submit">
        Sign in to training portal
      </button>
    </form>
  </div>
</body>
</html>
)HTML";
}

String portalSuccessPage() {
  return R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">

  <meta
    name="viewport"
    content="width=device-width,initial-scale=1"
  >

  <title>Thank you</title>
</head>

<body style="font-family:system-ui;max-width:650px;margin:3rem auto;padding:1rem">
  <h1>Thank you!</h1>

  <p>
    Logging you in...
  </p>
</body>
</html>
)HTML";
}

void configurePortalRoutes() {
  if (portalRoutesConfigured) {
    return;
  }

  webServer.on(
    "/",
    HTTP_ANY,
    []() {
      webServer.send(
        200,
        "text/html",
        portalPage()
      );
    }
  );

  webServer.on(
    "/login",
    HTTP_POST,
    []() {
      String username =
        webServer.arg("username");

      String password =
        webServer.arg("password");

      Serial.println();
      Serial.println(
        "========== PORTAL LAB SUBMISSION =========="
      );

      Serial.printf(
        "[PORTAL] Client: %s\n",
        webServer.client()
          .remoteIP()
          .toString()
          .c_str()
      );

      Serial.println(
        "[PORTAL] POST /login"
      );

      Serial.printf(
        "[PORTAL] Username: %s\n",
        username.c_str()
      );

      Serial.printf(
        "[PORTAL] Password: %s\n",
        password.c_str()
      );

      Serial.println(
        "[PORTAL] Thank you! Logging you in..."
      );

      Serial.println(
        "==========================================="
      );

      printPrompt();

      // Limpiar copias temporales.
      username = "";
      password = "";

      webServer.send(
        200,
        "text/html",
        portalSuccessPage()
      );
    }
  );

  webServer.onNotFound(
    []() {
      webServer.sendHeader(
        "Location",
        "http://192.168.4.1/",
        true
      );

      webServer.send(
        302,
        "text/plain",
        ""
      );
    }
  );

  portalRoutesConfigured = true;
}

void startPortal(
  String ssid
) {
  stopAll();

  currentMode = Mode::PORTAL;

  if (ssid.length() == 0) {
    ssid =
      "WirelessLab32-C6-Demo";
  }

  WiFi.mode(WIFI_AP);

  bool started =
    WiFi.softAP(
      ssid.c_str(),
      nullptr,
      6,
      false,
      4
    );

  if (!started) {
    Serial.println(
      "Could not start captive portal AP."
    );

    currentMode = Mode::IDLE;
    return;
  }

  dnsServer.start(
    53,
    "*",
    WiFi.softAPIP()
  );

  configurePortalRoutes();

  webServer.begin();
  portalActive = true;

  Serial.println(
    "Portal started."
  );

  Serial.printf(
    "SSID: %s\n",
    ssid.c_str()
  );

  Serial.printf(
    "URL: http://%s/\n",
    WiFi.softAPIP()
      .toString()
      .c_str()
  );

  Serial.println(
    "Training username: student"
  );

  Serial.println(
    "Password: any fictional training value"
  );
}

// ============================================================
// Help
// ============================================================

void printHelp() {
  Serial.println();
  Serial.println("System:");
  Serial.println("  help");
  Serial.println("  info");
  Serial.println("  status");
  Serial.println("  stop");
  Serial.println("  reboot");

  Serial.println();
  Serial.println("Wi-Fi:");
  Serial.println("  wifi scan");
  Serial.println("  wifi list");

  Serial.println(
    "  wifi beacon add <ssid>"
  );

  Serial.println(
    "  wifi beacon clear"
  );

  Serial.println(
    "  wifi beacon channel <1-13>"
  );

  Serial.println(
    "  wifi beacon start [seconds]"
  );

  Serial.println(
    "  wifi spam start [seconds]"
  );

  Serial.println(
    "  wifi spam load"
  );

  Serial.println(
    "  portal start [ssid]"
  );

  Serial.println(
    "  portal stop"
  );

  Serial.println();
  Serial.println("BLE:");
  Serial.println("  ble mode");
  Serial.println("  ble scan");
  Serial.println("  ble list");

  Serial.println(
    "  ble advertise start"
  );

  Serial.println(
    "  ble advertise stop"
  );

  Serial.println(
    "  ble spam start [seconds]"
  );

  Serial.println(
    "  ble spam stop"
  );

  Serial.println(
    "  ble spam load"
  );

  Serial.println(
    "  ble spam status"
  );

  Serial.println(
    "  ble bluesnarf demo"
  );

  Serial.println(
    "  ble bluejack demo"
  );

  Serial.println(
    "  ble bluejack send <message>"
  );

  Serial.println(
    "  ble exit"
  );
}

// ============================================================
// Command parser
// ============================================================

void handleCommand(
  String command
) {
  command.trim();

  if (command.length() == 0) {
    return;
  }

  if (command == "help") {
    printHelp();
  }

  else if (command == "info") {
    Serial.printf(
      "Firmware: WirelessLab32-C6\n"
      "Version: %s\n"
      "Chip: %s rev %u\n"
      "CPU: %u MHz\n"
      "Flash: %u bytes\n"
      "Free heap: %u bytes\n"
      "Mode: %s\n",
      FW_VERSION,
      ESP.getChipModel(),
      ESP.getChipRevision(),
      ESP.getCpuFreqMHz(),
      ESP.getFlashChipSize(),
      ESP.getFreeHeap(),
      getModeName().c_str()
    );
  }

  else if (command == "status") {
    Serial.printf(
      "Mode: %s\n"
      "BLE initialized: %s\n"
      "BLE advertising: %s\n"
      "Portal: %s\n",
      getModeName().c_str(),
      bleInitialized
        ? "yes"
        : "no",
      bleAdvertisingActive
        ? "active"
        : "inactive",
      portalActive
        ? "active"
        : "inactive"
    );
  }

  else if (command == "stop") {
    stopAll();
  }

  else if (command == "reboot") {
    Serial.println(
      "Restarting..."
    );

    delay(150);
    ESP.restart();
  }

  else if (command == "wifi scan") {
    runWifiScan();
  }

  else if (command == "wifi list") {
    printWifiList();
  }

  else if (
    command.startsWith(
      "wifi beacon add "
    )
  ) {
    String ssid =
      command.substring(
        String(
          "wifi beacon add "
        ).length()
      );

    ssid.trim();

    if (
      ssid.length() == 0 ||
      ssid.length() > 32
    ) {
      Serial.println(
        "SSID must contain between 1 and 32 characters."
      );
    }

    else if (
      beaconSsids.size() >=
      MAX_BEACON_SSIDS
    ) {
      Serial.println(
        "Maximum of 5 SSIDs reached."
      );
    }

    else {
      beaconSsids.push_back(
        ssid
      );

      Serial.printf(
        "Added beacon SSID: %s\n",
        ssid.c_str()
      );
    }
  }

  else if (
    command ==
    "wifi beacon clear"
  ) {
    beaconSsids.clear();

    Serial.println(
      "Beacon SSID list cleared."
    );
  }

  else if (
    command.startsWith(
      "wifi beacon channel "
    )
  ) {
    int requestedChannel =
      command.substring(
        String(
          "wifi beacon channel "
        ).length()
      ).toInt();

    if (
      requestedChannel < 1 ||
      requestedChannel > 13
    ) {
      Serial.println(
        "Channel must be between 1 and 13."
      );
    } else {
      beaconChannel =
        static_cast<uint8_t>(
          requestedChannel
        );

      Serial.printf(
        "Beacon channel set to %u.\n",
        beaconChannel
      );
    }
  }

  else if (
    command.startsWith(
      "wifi beacon start"
    )
  ) {
    String argument =
      command.substring(
        String(
          "wifi beacon start"
        ).length()
      );

    argument.trim();

    uint8_t seconds =
      argument.length()
        ? static_cast<uint8_t>(
            argument.toInt()
          )
        : 10;

    startBeaconLab(
      seconds
    );
  }

  else if (
    command ==
    "wifi spam load"
  ) {
    loadWifiSpamPresets();
  }

  else if (
    command.startsWith(
      "wifi spam start"
    )
  ) {
    String argument =
      command.substring(
        String("wifi spam start").length()
      );

    argument.trim();

    int requestedSeconds =
      argument.length() ? argument.toInt() : 10;

    if (beaconSsids.empty()) {
      loadWifiSpamPresets();
    }

    startBeaconLab(
      static_cast<uint8_t>(requestedSeconds)
    );
  }

  else if (
    command.startsWith(
      "portal start"
    )
  ) {
    String ssid =
      command.substring(
        String(
          "portal start"
        ).length()
      );

    ssid.trim();

    startPortal(ssid);
  }

  else if (
    command ==
    "portal stop"
  ) {
    stopAll();
  }

  else if (
    command ==
    "ble mode"
  ) {
    enterBleSerialMode();
  }

  else if (
    command ==
    "ble scan"
  ) {
    runBleScan();
  }

  else if (
    command ==
    "ble list"
  ) {
    printBleDevices();
  }

  else if (
    command ==
    "ble advertise start"
  ) {
    startBleAdvertising();
  }

  else if (
    command.startsWith(
      "ble advertise start "
    )
  ) {
    /*
      Por estabilidad, el nombre indicado por el usuario
      se ignora temporalmente. El nombre anunciado será
      siempre WirelessLab32.
    */

    String requestedName =
      command.substring(
        String(
          "ble advertise start "
        ).length()
      );

    requestedName.trim();

    Serial.printf(
      "Requested name '%s' ignored during stability testing.\n",
      requestedName.c_str()
    );

    startBleAdvertising();
  }

  else if (
    command ==
    "ble advertise stop"
  ) {
    stopBleAdvertising();
  }

  else if (
    command.startsWith(
      "ble spam start"
    )
  ) {
    String argument =
      command.substring(
        String("ble spam start").length()
      );

    argument.trim();

    int requestedSeconds =
      argument.length() ? argument.toInt() : 10;

    startBleSpamLab(
      static_cast<uint8_t>(requestedSeconds)
    );
  }

  else if (
    command ==
    "ble spam stop"
  ) {
    stopBleAdvertising();
  }

  else if (
    command ==
    "ble spam load"
  ) {
    printBleSpamPresets();
  }

  else if (
    command ==
    "ble spam status"
  ) {
    Serial.printf(
      "BLE spam lab: %s\nPresets: %u\n",
      bleSpamActive ? "active" : "inactive",
      static_cast<unsigned>(BLE_SPAM_PRESET_COUNT)
    );
  }

  else if (
    command ==
    "ble bluesnarf demo"
  ) {
    startBlueSnarfDemo();
  }

  else if (
    command ==
    "ble bluejack demo"
  ) {
    startBluejackingDemo();
  }

  else if (
    command.startsWith(
      "ble bluejack send "
    )
  ) {
    String message =
      command.substring(
        String("ble bluejack send ").length()
      );

    sendBluejackDemoMessage(message);
  }

  else if (
    command ==
    "ble exit"
  ) {
    stopBleOperations();

    currentMode =
      Mode::IDLE;

    Serial.println(
      "Exited BLE mode."
    );
  }

  else {
    Serial.println(
      "Unknown command. Type: help"
    );
  }
}

// ============================================================
// Serial handling
// ============================================================

void serviceSerial() {
  while (Serial.available()) {
    char character =
      static_cast<char>(
        Serial.read()
      );

    if (character == '\r') {
      continue;
    }

    if (character == '\n') {
      Serial.println();

      String command =
        serialBuffer;

      serialBuffer = "";

      handleCommand(
        command
      );

      printPrompt();
    }

    else if (
      character == 8 ||
      character == 127
    ) {
      if (serialBuffer.length()) {
        serialBuffer.remove(
          serialBuffer.length() - 1
        );

        Serial.print(
          "\b \b"
        );
      }
    }

    else if (
      isPrintable(character)
    ) {
      serialBuffer +=
        character;

      Serial.write(
        character
      );
    }
  }
}

// ============================================================
// Arduino lifecycle
// ============================================================

void setup() {
  Serial.begin(115200);

  delay(500);

  WiFi.mode(WIFI_OFF);

  Serial.println();
  Serial.println("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
  Serial.println("X                                  X");
  Serial.println("X  ESP32-C6 - Wireless Hacking Lab  X");
  Serial.println("X                                  X");
  Serial.println("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
  Serial.printf("Firmware: WirelessLab32-C6 v%s\n", FW_VERSION);

  Serial.println(
    "Type: help"
  );

  printPrompt();
}

void loop() {
  serviceSerial();
  serviceBeaconLab();
  serviceBleAdvertisingLab();

  if (
    currentMode ==
    Mode::PORTAL
  ) {
    dnsServer.processNextRequest();
    webServer.handleClient();
  }

  delay(2);
}
