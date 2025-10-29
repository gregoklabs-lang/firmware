#include "provisioning.h"

#include <BLE2902.h>
#include <BLEAdvertising.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include <algorithm>
#include <string>

namespace Provisioning {
namespace {
constexpr char kServiceUuid[] = "12345678-1234-1234-1234-1234567890ab";
constexpr char kCharacteristicUuid[] = "87654321-4321-4321-4321-0987654321ba";
constexpr uint16_t kInvalidConnId = 0xFFFF;

BLEServer *g_server = nullptr;
BLECharacteristic *g_characteristic = nullptr;
BLEAdvertising *g_advertising = nullptr;
CredentialsCallback g_callback;
String g_deviceId;
bool g_initialized = false;
bool g_sessionActive = false;
bool g_centralConnected = false;
uint16_t g_connId = kInvalidConnId;
volatile bool g_restartAdvertising = false;

struct ParsedCredentials {
  bool valid = false;
  String ssid;
  String password;
  String error;
};

bool isWhitespace(char c) {
  return c == '\r' || c == '\n' || c == '\t' || c == ' ';
}

void trim(std::string &text) {
  while (!text.empty() && isWhitespace(text.front())) {
    text.erase(text.begin());
  }
  while (!text.empty() && isWhitespace(text.back())) {
    text.pop_back();
  }
}

ParsedCredentials parseCredentials(const std::string &raw) {
  ParsedCredentials result;
  if (raw.empty()) {
    result.error = "vacio";
    return result;
  }

  std::string payload = raw;
  payload.erase(std::remove(payload.begin(), payload.end(), '\r'), payload.end());

  size_t separator = payload.find('\n');
  if (separator == std::string::npos) {
    separator = payload.find('|');
  }

  if (separator == std::string::npos) {
    result.error = "formato";
    return result;
  }

  std::string ssid(payload.begin(), payload.begin() + separator);
  std::string password(payload.begin() + separator + 1, payload.end());

  trim(ssid);
  trim(password);

  if (ssid.empty()) {
    result.error = "ssid";
    return result;
  }

  result.valid = true;
  result.ssid = String(ssid.c_str());
  result.password = String(password.c_str());
  return result;
}

void notify(const String &message) {
  if (!g_characteristic) {
    return;
  }
  g_characteristic->setValue(message.c_str());
  if (g_centralConnected) {
    g_characteristic->notify();
  }
}

class ProvisioningCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    const std::string value = characteristic->getValue();
    ParsedCredentials credentials = parseCredentials(value);

    if (!credentials.valid) {
      notify("error:" + credentials.error);
      return;
    }

    notify("credenciales");

    if (g_callback) {
      g_callback(credentials.ssid, credentials.password);
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    g_centralConnected = true;
    g_connId = server->getConnId();
  }

  void onDisconnect(BLEServer *server) override {
    g_centralConnected = false;
    g_connId = kInvalidConnId;
    if (g_sessionActive) {
      g_restartAdvertising = true;
    }
  }
};

void configureAdvertising() {
  if (!g_advertising) {
    g_advertising = BLEDevice::getAdvertising();
  }

  if (!g_advertising) {
    return;
  }

  BLEAdvertisementData advData;
  advData.setName(g_deviceId.c_str());
  advData.setCompleteServices(BLEUUID(kServiceUuid));

  g_advertising->setAdvertisementData(advData);
  g_advertising->setScanResponse(false);
  g_advertising->setMinPreferred(0x06);
  g_advertising->setMaxPreferred(0x12);
}

void ensureInitialized(const String &deviceId) {
  if (g_initialized) {
    g_deviceId = deviceId;
    configureAdvertising();
    return;
  }

  g_deviceId = deviceId;
  BLEDevice::init(g_deviceId.c_str());

  g_server = BLEDevice::createServer();
  g_server->setCallbacks(new ServerCallbacks());

  BLEService *service = g_server->createService(kServiceUuid);
  g_characteristic = service->createCharacteristic(
      kCharacteristicUuid,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY);
  g_characteristic->addDescriptor(new BLE2902());
  g_characteristic->setCallbacks(new ProvisioningCallbacks());
  g_characteristic->setValue("inactivo");

  service->start();

  g_advertising = BLEDevice::getAdvertising();
  if (g_advertising) {
    g_advertising->addServiceUUID(kServiceUuid);
  }

  configureAdvertising();
  g_initialized = true;
}
}  // namespace

void begin(const String &deviceId, CredentialsCallback callback) {
  g_callback = callback;
  ensureInitialized(deviceId);
  notify("inactivo");
}

bool startBle() {
  if (!g_initialized || !g_advertising) {
    return false;
  }

  configureAdvertising();
  g_advertising->start();
  notify("activo");
  g_sessionActive = true;
  g_restartAdvertising = false;
  return true;
}

void stopBle() {
  if (!g_initialized) {
    return;
  }

  if (g_advertising) {
    g_advertising->stop();
  }

  if (g_centralConnected && g_server && g_connId != kInvalidConnId) {
    g_server->disconnect(g_connId);
  }

  g_sessionActive = false;
  g_restartAdvertising = false;
  notify("inactivo");
}

bool isActive() { return g_sessionActive; }

void notifyStatus(const String &message) { notify(message); }

void loop() {
  if (g_restartAdvertising && g_sessionActive && g_advertising) {
    g_restartAdvertising = false;
    g_advertising->start();
  }
}

}  // namespace Provisioning

