#include <Arduino.h>
#include <cerrno>
#include <cmath>
#include <new>
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
#include <Zigbee.h>
#include <ep/ZigbeeLight.h>
#include <ep/ZigbeeTempSensor.h>

/*
  El ESP32-C6 puede exponer más de una consola USB. WirelessLab32 usa
  explícitamente UART0 para que el banner, los comandos y el log de arranque
  aparezcan en el mismo puerto que la consola ROM.
*/
#define Serial Serial0

extern "C" {
  #include "esp_partition.h"
  #include "esp_wifi.h"
  #include "esp_rom_sys.h"
  #include "bdb/esp_zigbee_bdb_commissioning.h"
}

#if !defined(ZIGBEE_MODE_ED)
  #error "Select Zigbee Mode: Zigbee ED (end device)."
#endif

// Arduino.h defines SERIAL as a numeric macro; free the name for the enum below.
#ifdef SERIAL
  #undef SERIAL
#endif

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

static const char *FW_VERSION = "0.5.0-c6-ed";

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

// Zigbee End Device lab for Home Assistant ZHA.
static const uint8_t ZIGBEE_ENDPOINT = 10;
static const char *ZIGBEE_MANUFACTURER = "WirelessLab32";
static const char *ZIGBEE_LIGHT_MODEL = "ESP32-C6 Lab Light";
static const char *ZIGBEE_SENSOR_MODEL = "ESP32-C6 Lab Sensor";
static const uint8_t MAX_ZIGBEE_REPORTING_DURATION_SECONDS = 60;
static const uint32_t ZIGBEE_SENSOR_DEFAULT_INTERVAL_MS = 5000;
static const uint32_t ZIGBEE_SENSOR_MIN_INTERVAL_MS = 500;
static const uint32_t ZIGBEE_SENSOR_MAX_INTERVAL_MS = 60000;
static const uint32_t ZIGBEE_FACTORY_RESET_CONFIRMATION_MS = 15000;

#ifndef LED_BUILTIN
static const uint8_t ZIGBEE_LIGHT_LED_PIN = 8;
#else
static const uint8_t ZIGBEE_LIGHT_LED_PIN = LED_BUILTIN;
#endif

// Cambia a true si el LED de tu placa se enciende con nivel LOW.
static const bool ZIGBEE_LIGHT_LED_ACTIVE_LOW = false;

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
  BLE_SERIAL,
  ZIGBEE_DEVICE
};

enum class ZigbeeProfile {
  SENSOR,
  LIGHT
};

enum class ZigbeeLightStateSource {
  STARTUP,
  SERIAL,
  REMOTE
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
// Zigbee
// ============================================================

ZigbeeProfile zigbeeProfile = ZigbeeProfile::SENSOR;
ZigbeeLight *zigbeeLightEndpoint = nullptr;
ZigbeeTempSensor *zigbeeSensorEndpoint = nullptr;
bool zigbeeStackStarted = false;
bool zigbeeJoined = false;
bool zigbeeJoining = false;
bool zigbeeEndpointReady = false;
bool zigbeeRebootRequired = false;
bool zigbeeFactoryResetPending = false;
uint32_t zigbeeFactoryResetDeadline = 0;
String zigbeeLastOperation = "none";
esp_err_t zigbeeLastResult = ESP_OK;
bool zigbeeHasLastResult = false;

bool zigbeeLightState = false;
bool zigbeeLocalLightUpdate = false;
ZigbeeLightStateSource zigbeeLightStateSource =
  ZigbeeLightStateSource::STARTUP;

int16_t zigbeeSensorCentiCelsius = 2350;
uint32_t zigbeeSensorIntervalMs =
  ZIGBEE_SENSOR_DEFAULT_INTERVAL_MS;
bool zigbeeSensorReportingActive = false;
uint32_t zigbeeSensorReportingStopAt = 0;
uint32_t zigbeeSensorNextReportAt = 0;
uint32_t zigbeeSensorReportsSent = 0;

// ============================================================
// Forward declarations
// ============================================================

void stopBleOperations();
void stopAll();
void stopSensorReporting(bool announce = true);
void printZigbeeStatus();
void serviceZigbee();

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

    case Mode::ZIGBEE_DEVICE:
      return "ZIGBEE_DEVICE";
  }

  return "UNKNOWN";
}

// ============================================================
// Zigbee End Device lab
// ============================================================

const char *getZigbeeProfileName() {
  return zigbeeProfile == ZigbeeProfile::LIGHT
    ? "LIGHT"
    : "SENSOR";
}

const char *getZigbeeModelName() {
  return zigbeeProfile == ZigbeeProfile::LIGHT
    ? ZIGBEE_LIGHT_MODEL
    : ZIGBEE_SENSOR_MODEL;
}

const char *getLightStateSourceName() {
  switch (zigbeeLightStateSource) {
    case ZigbeeLightStateSource::SERIAL:
      return "serial";
    case ZigbeeLightStateSource::REMOTE:
      return "remote";
    case ZigbeeLightStateSource::STARTUP:
    default:
      return "startup";
  }
}

void setZigbeeResult(
  const String &operation,
  esp_err_t result
) {
  zigbeeLastOperation = operation;
  zigbeeLastResult = result;
  zigbeeHasLastResult = true;
}

bool parseStrictLong(
  String value,
  long &parsed
) {
  value.trim();

  if (value.length() == 0) {
    return false;
  }

  errno = 0;
  char *end = nullptr;
  const long result =
    strtol(value.c_str(), &end, 10);

  if (
    errno == ERANGE ||
    end == value.c_str() ||
    *end != '\0'
  ) {
    return false;
  }

  parsed = result;
  return true;
}

bool parseStrictFloat(
  String value,
  float &parsed
) {
  value.trim();

  if (value.length() == 0) {
    return false;
  }

  errno = 0;
  char *end = nullptr;
  const float result =
    strtof(value.c_str(), &end);

  if (
    errno == ERANGE ||
    end == value.c_str() ||
    *end != '\0' ||
    !isfinite(result)
  ) {
    return false;
  }

  parsed = result;
  return true;
}

String normalizeCommandWhitespace(String value) {
  value.trim();
  String normalized;
  normalized.reserve(value.length());
  bool previousWasSpace = false;

  for (size_t i = 0; i < value.length(); i++) {
    const char character = value.charAt(i);
    const bool isSpace =
      character == ' ' || character == '\t';

    if (isSpace) {
      if (!previousWasSpace) {
        normalized += ' ';
      }
    } else {
      normalized += character;
    }

    previousWasSpace = isSpace;
  }

  normalized.trim();
  return normalized;
}

void applyLightLedState(bool on) {
  const uint8_t level =
    (on != ZIGBEE_LIGHT_LED_ACTIVE_LOW)
      ? HIGH
      : LOW;

  digitalWrite(ZIGBEE_LIGHT_LED_PIN, level);
}

void printLightState(
  bool state,
  ZigbeeLightStateSource source
) {
  Serial.println();
  Serial.println("[ZIGBEE LIGHT]");
  Serial.printf("state=%s\n", state ? "ON" : "OFF");

  switch (source) {
    case ZigbeeLightStateSource::SERIAL:
      Serial.println("source=serial");
      break;
    case ZigbeeLightStateSource::REMOTE:
      Serial.println("source=remote");
      break;
    case ZigbeeLightStateSource::STARTUP:
    default:
      Serial.println("source=startup");
      break;
  }
}

void handleRemoteLightChange(bool state) {
  zigbeeLightState = state;
  zigbeeLightStateSource =
    zigbeeLocalLightUpdate
      ? ZigbeeLightStateSource::SERIAL
      : ZigbeeLightStateSource::REMOTE;

  applyLightLedState(state);
  printLightState(state, zigbeeLightStateSource);
}

bool initializeZigbeeLightEndpoint() {
  if (zigbeeLightEndpoint != nullptr) {
    return false;
  }

  zigbeeLightEndpoint =
    new (std::nothrow) ZigbeeLight(ZIGBEE_ENDPOINT);

  if (zigbeeLightEndpoint == nullptr) {
    setZigbeeResult("create light endpoint", ESP_ERR_NO_MEM);
    return false;
  }

  if (
    !zigbeeLightEndpoint->setManufacturerAndModel(
      ZIGBEE_MANUFACTURER,
      ZIGBEE_LIGHT_MODEL
    )
  ) {
    setZigbeeResult("configure light endpoint", ESP_FAIL);
    return false;
  }

  zigbeeLightEndpoint->onLightChange(
    handleRemoteLightChange
  );

  if (!Zigbee.addEndpoint(zigbeeLightEndpoint)) {
    setZigbeeResult("register light endpoint", ESP_FAIL);
    return false;
  }

  return true;
}

bool initializeZigbeeSensorEndpoint() {
  if (zigbeeSensorEndpoint != nullptr) {
    return false;
  }

  zigbeeSensorEndpoint =
    new (std::nothrow) ZigbeeTempSensor(ZIGBEE_ENDPOINT);

  if (zigbeeSensorEndpoint == nullptr) {
    setZigbeeResult("create sensor endpoint", ESP_ERR_NO_MEM);
    return false;
  }

  if (
    !zigbeeSensorEndpoint->setManufacturerAndModel(
      ZIGBEE_MANUFACTURER,
      ZIGBEE_SENSOR_MODEL
    ) ||
    !zigbeeSensorEndpoint->setMinMaxValue(-40.0f, 125.0f) ||
    !zigbeeSensorEndpoint->setDefaultValue(
      zigbeeSensorCentiCelsius / 100.0f
    ) ||
    !zigbeeSensorEndpoint->setTolerance(0.01f)
  ) {
    setZigbeeResult("configure sensor endpoint", ESP_FAIL);
    return false;
  }

  if (!Zigbee.addEndpoint(zigbeeSensorEndpoint)) {
    setZigbeeResult("register sensor endpoint", ESP_FAIL);
    return false;
  }

  return true;
}

bool initializeSelectedZigbeeEndpoint() {
  if (zigbeeEndpointReady) {
    return true;
  }

  const bool ready =
    zigbeeProfile == ZigbeeProfile::LIGHT
      ? initializeZigbeeLightEndpoint()
      : initializeZigbeeSensorEndpoint();

  zigbeeEndpointReady = ready;
  return ready;
}

bool startZigbeeEndDevice() {
  if (zigbeeStackStarted) {
    Serial.println(
      zigbeeJoined
        ? "Zigbee device is already joined."
        : "Zigbee network steering is already active."
    );
    return true;
  }

  stopAll();
  WiFi.mode(WIFI_OFF);

  if (bleInitialized) {
    BLEDevice::deinit(true);
    bleScan = nullptr;
    bleAdvertising = nullptr;
    bleServer = nullptr;
    bleService = nullptr;
    bleBluejackMessage = nullptr;

    for (size_t i = 0; i < 3; i++) {
      bleDemoContacts[i] = nullptr;
    }

    bleInitialized = false;
    Serial.println("BLE stack released for Zigbee.");
  }

  if (!initializeSelectedZigbeeEndpoint()) {
    Serial.printf(
      "Could not create the %s endpoint.\n",
      getZigbeeProfileName()
    );
    return false;
  }

  Zigbee.setTimeout(30000);

  Serial.println("Starting Zigbee End Device...");
  Serial.printf(
    "Selected profile: %s\n",
    getZigbeeProfileName()
  );
  Serial.println("Searching for an open Zigbee network...");
  Serial.println(
    "Use Home Assistant ZHA > Add device to open permit-join."
  );

  zigbeeJoining = true;
  zigbeeLastOperation = "network steering";

  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    zigbeeJoining = false;
    setZigbeeResult("start Zigbee End Device", ESP_FAIL);
    Serial.println("Could not start the Zigbee End Device stack.");
    return false;
  }

  zigbeeStackStarted = true;
  currentMode = Mode::ZIGBEE_DEVICE;
  setZigbeeResult("start Zigbee End Device", ESP_OK);

  Serial.println(
    "Zigbee stack started; waiting for a confirmed network join."
  );
  Serial.println(
    "The radio remains assigned to Zigbee until reboot."
  );
  return true;
}

void announceZigbeeJoined() {
  esp_zb_ieee_addr_t extendedPanId = {};
  esp_zb_get_extended_pan_id(extendedPanId);

  Serial.println();
  Serial.println("Zigbee device joined.");
  Serial.printf(
    "Channel: %u\n"
    "PAN ID: 0x%04X\n"
    "Short address: 0x%04X\n"
    "Coordinator: 0x0000\n"
    "Profile: %s\n",
    esp_zb_get_current_channel(),
    esp_zb_get_pan_id(),
    esp_zb_get_short_address(),
    getZigbeeProfileName()
  );

  zigbeeJoined = true;
  zigbeeJoining = false;
  setZigbeeResult("join network", ESP_OK);

  if (
    zigbeeProfile == ZigbeeProfile::SENSOR &&
    zigbeeSensorEndpoint != nullptr
  ) {
    zigbeeSensorEndpoint->setTemperature(
      zigbeeSensorCentiCelsius / 100.0f
    );
  }
}

void handleZigbeeJoin() {
  if (zigbeeRebootRequired) {
    Serial.println(
      "Reboot is required before starting Zigbee again."
    );
    return;
  }

  startZigbeeEndDevice();
}

bool reportLightState() {
  if (
    !zigbeeJoined ||
    zigbeeLightEndpoint == nullptr
  ) {
    Serial.println("Light state changed locally; device is not joined.");
    return false;
  }

  esp_zb_zcl_report_attr_cmd_t report = {};
  report.address_mode =
    ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
  report.attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID;
  report.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
  report.clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF;
  report.zcl_basic_cmd.src_endpoint = ZIGBEE_ENDPOINT;
  report.manuf_specific = 0;
  report.dis_default_resp = 0;

  esp_zb_lock_acquire(portMAX_DELAY);
  const esp_err_t result =
    esp_zb_zcl_report_attr_cmd_req(&report);
  esp_zb_lock_release();

  setZigbeeResult("report light state", result);

  if (result != ESP_OK) {
    Serial.printf(
      "Could not report light state: %s\n",
      esp_err_to_name(result)
    );
    return false;
  }

  return true;
}

void setZigbeeLightState(
  bool state,
  ZigbeeLightStateSource source
) {
  zigbeeLightState = state;
  zigbeeLightStateSource = source;
  applyLightLedState(state);

  if (zigbeeLightEndpoint != nullptr) {
    zigbeeLocalLightUpdate = true;
    const bool updated =
      zigbeeLightEndpoint->setLight(state);
    zigbeeLocalLightUpdate = false;
    setZigbeeResult(
      "set light state",
      updated ? ESP_OK : ESP_FAIL
    );

    if (updated && zigbeeJoined) {
      reportLightState();
    }
  } else {
    printLightState(state, source);
  }

  if (!zigbeeJoined) {
    Serial.println(
      "Light state changed locally; device is not joined."
    );
  }
}

bool sendSensorReport() {
  if (zigbeeProfile != ZigbeeProfile::SENSOR) {
    Serial.println(
      "Zigbee SENSOR command rejected: selected profile is LIGHT."
    );
    return false;
  }

  if (
    !zigbeeJoined ||
    zigbeeSensorEndpoint == nullptr
  ) {
    Serial.println(
      "Cannot send sensor report: Zigbee device is not joined."
    );
    return false;
  }

  const float temperature =
    zigbeeSensorCentiCelsius / 100.0f;

  if (
    !zigbeeSensorEndpoint->setTemperature(temperature) ||
    !zigbeeSensorEndpoint->reportTemperature()
  ) {
    setZigbeeResult("send temperature report", ESP_FAIL);
    Serial.println("Temperature report failed.");
    return false;
  }

  zigbeeSensorReportsSent++;
  setZigbeeResult("send temperature report", ESP_OK);
  Serial.printf(
    "[ZIGBEE SENSOR] %.2f C report #%lu sent.\n",
    temperature,
    static_cast<unsigned long>(zigbeeSensorReportsSent)
  );
  return true;
}

void stopSensorReporting(bool announce) {
  if (zigbeeSensorReportingActive && announce) {
    Serial.println("Automatic sensor reporting stopped.");
  }

  zigbeeSensorReportingActive = false;
  zigbeeSensorReportingStopAt = 0;
  zigbeeSensorNextReportAt = 0;
}

void startSensorReporting(uint8_t seconds) {
  if (zigbeeProfile != ZigbeeProfile::SENSOR) {
    Serial.println(
      "Zigbee SENSOR command rejected: selected profile is LIGHT."
    );
    return;
  }

  if (!zigbeeJoined) {
    Serial.println(
      "Cannot start sensor reporting: Zigbee device is not joined."
    );
    return;
  }

  zigbeeSensorReportingActive = true;
  zigbeeSensorReportingStopAt =
    millis() + static_cast<uint32_t>(seconds) * 1000UL;
  zigbeeSensorNextReportAt = millis();

  Serial.printf(
    "Automatic sensor reporting started for %u seconds.\n",
    seconds
  );
}

void serviceSensorReporting() {
  if (!zigbeeSensorReportingActive) {
    return;
  }

  const uint32_t now = millis();

  if (
    static_cast<int32_t>(
      now - zigbeeSensorReportingStopAt
    ) >= 0
  ) {
    Serial.println("Sensor reporting duration completed.");
    stopSensorReporting(false);
    printPrompt();
    return;
  }

  if (
    static_cast<int32_t>(
      now - zigbeeSensorNextReportAt
    ) >= 0
  ) {
    sendSensorReport();
    zigbeeSensorNextReportAt =
      now + zigbeeSensorIntervalMs;
  }
}

void requestZigbeeLeave() {
  if (!zigbeeStackStarted || !zigbeeJoined) {
    Serial.println("Zigbee device is not joined.");
    return;
  }

  stopSensorReporting(false);
  zigbeeLastOperation = "leave network";
  Serial.println(
    "Requesting local Zigbee leave and clearing Zigbee network state..."
  );
  Serial.println(
    "The Arduino Zigbee core may reboot automatically after leave completes."
  );

  esp_zb_bdb_reset_via_local_action();
}

void requestZigbeeFactoryReset() {
  esp_err_t result = ESP_OK;

  if (zigbeeStackStarted) {
    Zigbee.factoryReset(false);
  } else {
    const esp_partition_t *partition =
      esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_ANY,
        "zb_storage"
      );

    if (partition == nullptr) {
      result = ESP_ERR_NOT_FOUND;
    } else {
      result = esp_partition_erase_range(
        partition,
        0,
        partition->size
      );
    }
  }

  zigbeeFactoryResetPending = false;
  zigbeeFactoryResetDeadline = 0;
  setZigbeeResult("factory reset Zigbee storage", result);

  if (result == ESP_OK) {
    stopSensorReporting(false);
    zigbeeJoined = false;
    zigbeeJoining = false;
    zigbeeRebootRequired = true;
    Serial.println("Zigbee factory state cleared.");
    Serial.println("Reboot required. Use: reboot");
  } else {
    Serial.printf(
      "Zigbee factory reset failed: %s\n",
      esp_err_to_name(result)
    );
  }
}

void printSensorStatus() {
  uint32_t remainingSeconds = 0;

  if (
    zigbeeSensorReportingActive &&
    static_cast<int32_t>(
      zigbeeSensorReportingStopAt - millis()
    ) > 0
  ) {
    remainingSeconds =
      (zigbeeSensorReportingStopAt - millis() + 999) / 1000;
  }

  Serial.printf(
    "Sensor value: %.2f C\n"
    "Report interval: %lu ms\n"
    "Automatic reporting: %s\n"
    "Reports sent: %lu\n",
    zigbeeSensorCentiCelsius / 100.0f,
    static_cast<unsigned long>(zigbeeSensorIntervalMs),
    zigbeeSensorReportingActive ? "active" : "inactive",
    static_cast<unsigned long>(zigbeeSensorReportsSent)
  );

  if (zigbeeSensorReportingActive) {
    Serial.printf(
      "Traffic stop in: %lu s\n",
      static_cast<unsigned long>(remainingSeconds)
    );
  } else {
    Serial.println("Traffic stop in: inactive");
  }
}

void printZigbeeStatus() {
  Serial.println();
  Serial.println("Zigbee status");
  Serial.println("-------------");
  Serial.printf("Profile: %s\n", getZigbeeProfileName());
  Serial.println("Role: End Device");
  Serial.printf(
    "Stack started: %s\n"
    "Endpoint ready: %s\n"
    "Joined: %s\n"
    "Joining: %s\n",
    zigbeeStackStarted ? "yes" : "no",
    zigbeeEndpointReady ? "yes" : "no",
    zigbeeJoined ? "yes" : "no",
    zigbeeJoining ? "yes" : "no"
  );

  if (zigbeeJoined) {
    esp_zb_ieee_addr_t extendedPanId = {};
    esp_zb_get_extended_pan_id(extendedPanId);

    Serial.printf(
      "Channel: %u\n"
      "PAN ID: 0x%04X\n"
      "Extended PAN ID: "
      "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n"
      "Short address: 0x%04X\n"
      "Coordinator address: 0x0000\n",
      esp_zb_get_current_channel(),
      esp_zb_get_pan_id(),
      extendedPanId[7],
      extendedPanId[6],
      extendedPanId[5],
      extendedPanId[4],
      extendedPanId[3],
      extendedPanId[2],
      extendedPanId[1],
      extendedPanId[0],
      esp_zb_get_short_address()
    );
  } else {
    Serial.println("Channel: unknown");
    Serial.println("PAN ID: unknown");
    Serial.println("Extended PAN ID: unknown");
    Serial.println("Short address: unknown");
    Serial.println("Coordinator address: unknown");
  }

  Serial.printf(
    "Endpoint: %u\n"
    "Manufacturer: %s\n"
    "Model: %s\n"
    "Last operation: %s\n",
    ZIGBEE_ENDPOINT,
    ZIGBEE_MANUFACTURER,
    getZigbeeModelName(),
    zigbeeLastOperation.c_str()
  );

  if (zigbeeHasLastResult) {
    Serial.printf(
      "Last result: %s\n",
      esp_err_to_name(zigbeeLastResult)
    );
  } else {
    Serial.println("Last result: none");
  }

  Serial.printf(
    "Reboot required: %s\n",
    zigbeeRebootRequired ? "yes" : "no"
  );

  if (zigbeeProfile == ZigbeeProfile::LIGHT) {
    Serial.printf(
      "Light state: %s\n"
      "LED pin: %u\n"
      "LED active low: %s\n"
      "Last state source: %s\n",
      zigbeeLightState ? "ON" : "OFF",
      ZIGBEE_LIGHT_LED_PIN,
      ZIGBEE_LIGHT_LED_ACTIVE_LOW ? "yes" : "no",
      getLightStateSourceName()
    );
  } else {
    printSensorStatus();
  }
}

void serviceZigbee() {
  if (
    zigbeeFactoryResetPending &&
    static_cast<int32_t>(
      millis() - zigbeeFactoryResetDeadline
    ) >= 0
  ) {
    zigbeeFactoryResetPending = false;
    zigbeeFactoryResetDeadline = 0;
    Serial.println();
    Serial.println("Zigbee factory-reset confirmation expired.");
    printPrompt();
  }

  if (
    zigbeeStackStarted &&
    !zigbeeRebootRequired
  ) {
    const bool connected =
      Zigbee.connected() &&
      esp_zb_bdb_dev_joined();

    if (connected && !zigbeeJoined) {
      announceZigbeeJoined();
      printPrompt();
    } else if (!connected && zigbeeJoined) {
      zigbeeJoined = false;
      zigbeeJoining = true;
      zigbeeLastOperation = "network connection lost";
      Serial.println();
      Serial.println(
        "Zigbee connection lost; network steering is active."
      );
      printPrompt();
    }
  }

  serviceSensorReporting();
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
  if (zigbeeStackStarted) {
    stopSensorReporting();
    currentMode = Mode::ZIGBEE_DEVICE;
    Serial.println(
      "Zigbee is active. Leave the network and reboot before using Wi-Fi or BLE."
    );
    return;
  }

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

  Serial.printf(
    "Training username: %s\n",
    DEMO_USERNAME
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

  Serial.println();
  Serial.println("Zigbee (ESP32-C6):");
  Serial.println("  zigbee profile");
  Serial.println("  zigbee profile sensor");
  Serial.println("  zigbee profile light");
  Serial.println("  zigbee join");
  Serial.println("  zigbee leave");
  Serial.println("  zigbee status");
  Serial.println("  zigbee factory-reset");
  Serial.println("  confirm zigbee factory-reset");
  Serial.println("  zigbee light on|off|toggle|status");
  Serial.println("  zigbee sensor value <temperature>");
  Serial.println("  zigbee sensor send");
  Serial.println("  zigbee sensor interval <milliseconds>");
  Serial.println("  zigbee sensor start [seconds]");
  Serial.println("  zigbee sensor stop");
  Serial.println("  zigbee sensor status");
  Serial.println("  aliases: zigbee start, zigbee send, zigbee stop");
}

// ============================================================
// Command parser
// ============================================================

void handleCommand(
  String command
) {
  command = normalizeCommandWhitespace(command);

  if (command.length() == 0) {
    return;
  }

  if (
    zigbeeStackStarted &&
    command != "help" &&
    command != "info" &&
    command != "status" &&
    command != "stop" &&
    command != "reboot" &&
    !command.startsWith("zigbee ") &&
    command != "zigbee" &&
    command != "confirm zigbee factory-reset"
  ) {
    Serial.println(
      "Zigbee is active. Leave the network and reboot before using Wi-Fi or BLE."
    );
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
      "CPU: %lu MHz\n"
      "Flash: %lu bytes\n"
      "Free heap: %lu bytes\n"
      "Mode: %s\n",
      FW_VERSION,
      ESP.getChipModel(),
      ESP.getChipRevision(),
      static_cast<unsigned long>(ESP.getCpuFreqMHz()),
      static_cast<unsigned long>(ESP.getFlashChipSize()),
      static_cast<unsigned long>(ESP.getFreeHeap()),
      getModeName().c_str()
    );
  }

  else if (command == "status") {
    Serial.printf(
      "Mode: %s\n"
      "BLE initialized: %s\n"
      "BLE advertising: %s\n"
      "Portal: %s\n"
      "Zigbee stack: %s\n",
      getModeName().c_str(),
      bleInitialized
        ? "yes"
        : "no",
      bleAdvertisingActive
        ? "active"
        : "inactive",
      portalActive
        ? "active"
        : "inactive",
      zigbeeStackStarted
        ? "active"
        : "inactive"
    );

    printZigbeeStatus();
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

  else if (command == "zigbee profile") {
    Serial.printf(
      "Selected Zigbee profile: %s\n",
      getZigbeeProfileName()
    );
    Serial.println(
      "Select the profile before running: zigbee join"
    );
  }

  else if (
    command == "zigbee profile sensor" ||
    command == "zigbee profile light"
  ) {
    if (zigbeeStackStarted) {
      Serial.println(
        "Zigbee profile cannot be changed after the stack starts. Factory reset and reboot first."
      );
    } else {
      zigbeeProfile =
        command.endsWith("light")
          ? ZigbeeProfile::LIGHT
          : ZigbeeProfile::SENSOR;

      Serial.printf(
        "Selected Zigbee profile: %s\n",
        getZigbeeProfileName()
      );
      Serial.println("Next command: zigbee join");
    }
  }

  else if (
    command.startsWith("zigbee profile ")
  ) {
    Serial.println(
      "Usage: zigbee profile [sensor|light]"
    );
  }

  else if (
    command == "zigbee join" ||
    command == "zigbee start"
  ) {
    handleZigbeeJoin();
  }

  else if (
    command.startsWith("zigbee join ") ||
    command.startsWith("zigbee start ")
  ) {
    Serial.println("Usage: zigbee join");
  }

  else if (command == "zigbee leave") {
    requestZigbeeLeave();
  }

  else if (command == "zigbee factory-reset") {
    zigbeeFactoryResetPending = true;
    zigbeeFactoryResetDeadline =
      millis() + ZIGBEE_FACTORY_RESET_CONFIRMATION_MS;

    Serial.println(
      "Zigbee factory reset requested."
    );
    Serial.println(
      "Confirm within 15 seconds with:"
    );
    Serial.println(
      "confirm zigbee factory-reset"
    );
  }

  else if (
    command == "confirm zigbee factory-reset"
  ) {
    if (
      !zigbeeFactoryResetPending ||
      static_cast<int32_t>(
        millis() - zigbeeFactoryResetDeadline
      ) >= 0
    ) {
      zigbeeFactoryResetPending = false;
      Serial.println(
        "No active Zigbee factory-reset confirmation."
      );
    } else {
      requestZigbeeFactoryReset();
    }
  }

  else if (command == "zigbee status") {
    printZigbeeStatus();
  }

  else if (
    command == "zigbee light on" ||
    command == "zigbee light off" ||
    command == "zigbee light toggle"
  ) {
    if (zigbeeProfile != ZigbeeProfile::LIGHT) {
      Serial.println(
        "Zigbee LIGHT command rejected: selected profile is SENSOR."
      );
    } else {
      bool requestedState = zigbeeLightState;

      if (command.endsWith(" on")) {
        requestedState = true;
      } else if (command.endsWith(" off")) {
        requestedState = false;
      } else {
        requestedState = !zigbeeLightState;
      }

      setZigbeeLightState(
        requestedState,
        ZigbeeLightStateSource::SERIAL
      );
    }
  }

  else if (command == "zigbee light status") {
    if (zigbeeProfile != ZigbeeProfile::LIGHT) {
      Serial.println(
        "Zigbee LIGHT command rejected: selected profile is SENSOR."
      );
    } else {
      Serial.printf(
        "Light state: %s\n"
        "Last state source: %s\n",
        zigbeeLightState ? "ON" : "OFF",
        getLightStateSourceName()
      );
    }
  }

  else if (command.startsWith("zigbee light")) {
    Serial.println(
      "Usage: zigbee light on|off|toggle|status"
    );
  }

  else if (
    command.startsWith("zigbee sensor value ")
  ) {
    if (zigbeeProfile != ZigbeeProfile::SENSOR) {
      Serial.println(
        "Zigbee SENSOR command rejected: selected profile is LIGHT."
      );
    } else {
      const String argument =
        command.substring(
          String("zigbee sensor value ").length()
        );
      float temperature = 0.0f;

      if (
        !parseStrictFloat(argument, temperature) ||
        temperature < -40.0f ||
        temperature > 125.0f
      ) {
        Serial.println(
          "Temperature must be a decimal value from -40.00 to 125.00 C."
        );
      } else {
        zigbeeSensorCentiCelsius =
          static_cast<int16_t>(
            lroundf(temperature * 100.0f)
          );

        Serial.printf(
          "Simulated sensor value: %.2f C\n",
          zigbeeSensorCentiCelsius / 100.0f
        );
      }
    }
  }

  else if (command == "zigbee sensor value") {
    Serial.println(
      "Usage: zigbee sensor value <temperature>"
    );
  }

  else if (
    command == "zigbee sensor send" ||
    (
      command == "zigbee send" &&
      zigbeeProfile == ZigbeeProfile::SENSOR
    )
  ) {
    sendSensorReport();
  }

  else if (
    command.startsWith("zigbee sensor interval ")
  ) {
    if (zigbeeProfile != ZigbeeProfile::SENSOR) {
      Serial.println(
        "Zigbee SENSOR command rejected: selected profile is LIGHT."
      );
    } else {
      const String argument =
        command.substring(
          String("zigbee sensor interval ").length()
        );
      long interval = 0;

      if (
        !parseStrictLong(argument, interval) ||
        interval < static_cast<long>(
          ZIGBEE_SENSOR_MIN_INTERVAL_MS
        ) ||
        interval > static_cast<long>(
          ZIGBEE_SENSOR_MAX_INTERVAL_MS
        )
      ) {
        Serial.println(
          "Interval must be an integer from 500 to 60000 ms."
        );
      } else {
        zigbeeSensorIntervalMs =
          static_cast<uint32_t>(interval);

        Serial.printf(
          "Sensor report interval: %lu ms\n",
          static_cast<unsigned long>(
            zigbeeSensorIntervalMs
          )
        );
      }
    }
  }

  else if (command == "zigbee sensor interval") {
    Serial.println(
      "Usage: zigbee sensor interval <milliseconds>"
    );
  }

  else if (
    command == "zigbee sensor start" ||
    command.startsWith("zigbee sensor start ")
  ) {
    String argument =
      command.substring(
        String("zigbee sensor start").length()
      );
    argument.trim();
    long seconds = 10;

    if (
      (
        argument.length() > 0 &&
        !parseStrictLong(argument, seconds)
      ) ||
      seconds < 1 ||
      seconds >
        MAX_ZIGBEE_REPORTING_DURATION_SECONDS
    ) {
      Serial.println(
        "Duration must be an integer from 1 to 60 seconds."
      );
    } else {
      startSensorReporting(
        static_cast<uint8_t>(seconds)
      );
    }
  }

  else if (
    command == "zigbee sensor stop" ||
    (
      command == "zigbee stop" &&
      zigbeeProfile == ZigbeeProfile::SENSOR
    )
  ) {
    if (zigbeeProfile != ZigbeeProfile::SENSOR) {
      Serial.println(
        "Zigbee SENSOR command rejected: selected profile is LIGHT."
      );
    } else {
      stopSensorReporting();
    }
  }

  else if (command == "zigbee sensor status") {
    if (zigbeeProfile != ZigbeeProfile::SENSOR) {
      Serial.println(
        "Zigbee SENSOR command rejected: selected profile is LIGHT."
      );
    } else {
      printSensorStatus();
    }
  }

  else if (
    command == "zigbee send" &&
    zigbeeProfile == ZigbeeProfile::LIGHT
  ) {
    if (!zigbeeJoined || zigbeeLightEndpoint == nullptr) {
      Serial.println(
        "Cannot synchronize light state: Zigbee device is not joined."
      );
    } else {
      setZigbeeLightState(
        zigbeeLightState,
        ZigbeeLightStateSource::SERIAL
      );
    }
  }

  else if (
    command == "zigbee stop" &&
    zigbeeProfile == ZigbeeProfile::LIGHT
  ) {
    Serial.println(
      "LIGHT has no periodic reporting to stop. Use 'zigbee leave' to leave the network."
    );
  }

  else if (command.startsWith("zigbee sensor")) {
    Serial.println(
      "Usage: zigbee sensor value|send|interval|start|stop|status"
    );
  }

  else if (command.startsWith("zigbee")) {
    Serial.println(
      "Unknown Zigbee command. Type: help"
    );
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
  esp_rom_printf("\n[BOOT] WirelessLab32-C6 entered setup()\n");

  Serial.begin(115200);
  pinMode(ZIGBEE_LIGHT_LED_PIN, OUTPUT);
  applyLightLedState(false);

  esp_rom_printf("[BOOT] Serial initialized; waiting 500 ms\n");
  delay(500);
  esp_rom_printf("[BOOT] Printing application banner\n");

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
  serviceZigbee();

  if (
    currentMode ==
    Mode::PORTAL
  ) {
    dnsServer.processNextRequest();
    webServer.handleClient();
  }

  delay(2);
}
