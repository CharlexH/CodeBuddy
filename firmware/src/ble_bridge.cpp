#include "ble_bridge.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include <Arduino.h>
#include <string.h>

// Nordic UART Service UUIDs — every BLE serial example uses these, so
// existing tools (nRF Connect, bluefy, Web Bluetooth examples) can talk to
// us without custom UUIDs.
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// Incoming bytes are buffered in a simple ring for bleRead()/bleAvailable().
// Sized to hold a transcript snapshot JSON plus headroom; the GATT layer
// will flow-control if we fall behind.
static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static BLEServer*         server = nullptr;
static BLECharacteristic* txChar = nullptr;
static BLECharacteristic* rxChar = nullptr;
static volatile bool      connected = false;
static volatile bool      secure = false;
static volatile uint32_t  passkey = 0;
static volatile uint16_t  mtu = 23;
static volatile bool      ready = false;
static volatile bool      startupFailed = false;

static void formatAddr(const uint8_t* addr, char* out, size_t outSize) {
  if (!addr || outSize < 18) {
    if (outSize) out[0] = 0;
    return;
  }
  snprintf(
    out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
    addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]
  );
}

static void gapLog(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
  char addr[18] = {0};
  switch (event) {
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      ready = param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
      startupFailed = !ready;
      Serial.printf("[ble] advertising %s\n", ready ? "ready" : "FAILED");
      break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
      formatAddr(param->ble_security.ble_req.bd_addr, addr, sizeof(addr));
      Serial.printf("[ble] gap SEC_REQ from %s\n", addr);
      break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
      formatAddr(param->ble_security.ble_req.bd_addr, addr, sizeof(addr));
      Serial.printf("[ble] gap PASSKEY_REQ from %s\n", addr);
      break;
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
      formatAddr(param->ble_security.key_notif.bd_addr, addr, sizeof(addr));
      Serial.printf("[ble] gap PASSKEY_NOTIF for %s = %06lu\n", addr, (unsigned long)param->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_NC_REQ_EVT:
      formatAddr(param->ble_security.key_notif.bd_addr, addr, sizeof(addr));
      Serial.printf("[ble] gap NC_REQ for %s = %06lu\n", addr, (unsigned long)param->ble_security.key_notif.passkey);
      break;
    case ESP_GAP_BLE_KEY_EVT:
      Serial.printf("[ble] gap KEY_EVT type=%d\n", (int)param->ble_security.ble_key.key_type);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      formatAddr(param->ble_security.auth_cmpl.bd_addr, addr, sizeof(addr));
      Serial.printf("[ble] gap AUTH_CMPL addr=%s success=%d fail_reason=%d\n", addr, param->ble_security.auth_cmpl.success, param->ble_security.auth_cmpl.fail_reason);
      break;
    default:
      break;
  }
}

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;  // full — drop (upstream should keep up)
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    std::string v = c->getValue();
    if (!v.empty()) {
      Serial.printf("[ble] rx write len=%u\n", (unsigned)v.size());
      rxPush((const uint8_t*)v.data(), v.size());
    }
  }
};

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    connected = true;
    secure = false;
    Serial.println("[ble] connected");
  }
  void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    char addr[18] = {0};
    formatAddr(param->connect.remote_bda, addr, sizeof(addr));
    Serial.printf("[ble] connect conn_id=%u peer=%s\n", param->connect.conn_id, addr);
  }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    secure = false;
    passkey = 0;
    mtu = 23;
    Serial.println("[ble] disconnected");
    // Restart advertising so the next client can find us.
    BLEDevice::startAdvertising();
  }
  void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
    mtu = param->mtu.mtu;
    Serial.printf("[ble] mtu=%u\n", mtu);
  }
};

// Security callbacks are left in place for future secure-mode work, but the
// current hardware-validation path runs the NUS service open so we can finish
// end-to-end verification without CoreBluetooth pairing churn.
class SecCallbacks : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  bool onConfirmPIN(uint32_t) override { return false; }
  bool onSecurityRequest() override { return true; }
  void onPassKeyNotify(uint32_t pk) override {
    passkey = pk;
    Serial.printf("[ble] passkey %06lu\n", (unsigned long)pk);
  }
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    passkey = 0;
    secure = cmpl.success;
    Serial.printf("[ble] auth %s\n", cmpl.success ? "ok" : "FAIL");
    if (!cmpl.success && server) server->disconnect(server->getConnId());
  }
};

void bleInit(const char* deviceName) {
  ready = false;
  startupFailed = false;
  BLEDevice::init(deviceName);
  BLEDevice::setCustomGapHandler(gapLog);
  // Request the biggest MTU we can get. macOS negotiates to 185 typically.
  BLEDevice::setMTU(517);

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  txChar->setAccessPermissions(ESP_GATT_PERM_READ);
  BLE2902* cccd = new BLE2902();
  cccd->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
  txChar->addDescriptor(cccd);

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxChar->setAccessPermissions(ESP_GATT_PERM_WRITE);
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);   // iOS-friendly connection interval
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising requested as '%s'\n", deviceName);
}

bool bleReady()     { return ready; }
bool bleStartupFailed() { return startupFailed; }
bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  int n = esp_ble_get_bond_device_num();
  if (n <= 0) return;
  esp_ble_bond_dev_t* list = (esp_ble_bond_dev_t*)malloc(n * sizeof(esp_ble_bond_dev_t));
  if (!list) return;
  esp_ble_get_bond_device_list(&n, list);
  for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
  free(list);
  Serial.printf("[ble] cleared %d bond(s)\n", n);
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  // ATT notify payload is limited to (MTU - 3). macOS negotiates 185, so
  // the 182-byte chunk works there; use the live mtu so a peer that caps
  // at the 23-byte default doesn't get truncated notifies.
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    // Small yield so the BLE stack flushes before the next chunk.
    delay(4);
  }
  return sent;
}
