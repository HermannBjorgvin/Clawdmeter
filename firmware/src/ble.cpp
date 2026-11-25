#include "ble.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#ifdef USING_NIMBLE_ARDUINO_HEADERS
#include "nimble/nimble/host/include/host/ble_hs.h"
#else
#include "host/ble_hs.h"
#endif
#include <Preferences.h>

#define DEVICE_NAME "Clawdmeter"

// Custom GATT UUIDs for data channel
#define SERVICE_UUID        "4c41555a-4465-7669-6365-000000000001"
#define RX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000002"  // host writes here
#define TX_CHAR_UUID        "4c41555a-4465-7669-6365-000000000003"  // device ack/nack notifies
#define REQ_CHAR_UUID       "4c41555a-4465-7669-6365-000000000004"  // device-initiated refresh request

#define BLE_BUF_SIZE 512

// HID report descriptor: a keyboard (report ID 1, standard 6-KRO
// boot-protocol-compatible) and a consumer control (report ID 2, media keys).
// The keyboard includes the LED output report (Num/Caps/Scroll Lock
// indicators) — without it macOS's Keyboard Setup Assistant flags the device
// as "unidentifiable" because the descriptor doesn't look like a complete
// keyboard. Its key array covers usages 0..0x73 (A..F24, button_action.h).
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x01,  // Usage Page (Generic Desktop)
    0x09, 0x06,  // Usage (Keyboard)
    0xA1, 0x01,  // Collection (Application)
    0x85, 0x01,  //   Report ID (1)
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0xE0,  //   Usage Minimum (224) - Left Control
    0x29, 0xE7,  //   Usage Maximum (231) - Right GUI
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x01,  //   Logical Maximum (1)
    0x75, 0x01,  //   Report Size (1)
    0x95, 0x08,  //   Report Count (8)
    0x81, 0x02,  //   Input (Data, Variable, Absolute) - Modifier byte
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x08,  //   Report Size (8)
    0x81, 0x01,  //   Input (Constant) - Reserved byte
    // LED output report — required for macOS to treat this as a full keyboard.
    0x95, 0x05,  //   Report Count (5)
    0x75, 0x01,  //   Report Size (1)
    0x05, 0x08,  //   Usage Page (LEDs)
    0x19, 0x01,  //   Usage Minimum (Num Lock)
    0x29, 0x05,  //   Usage Maximum (Kana)
    0x91, 0x02,  //   Output (Data, Variable, Absolute) - LED report
    0x95, 0x01,  //   Report Count (1)
    0x75, 0x03,  //   Report Size (3)
    0x91, 0x01,  //   Output (Constant) - LED report padding
    0x95, 0x06,  //   Report Count (6)
    0x75, 0x08,  //   Report Size (8)
    0x15, 0x00,  //   Logical Minimum (0)
    0x25, 0x73,  //   Logical Maximum (115) - F24
    0x05, 0x07,  //   Usage Page (Key Codes)
    0x19, 0x00,  //   Usage Minimum (0)
    0x29, 0x73,  //   Usage Maximum (115)
    0x81, 0x00,  //   Input (Data, Array) - Key array (6 keys)
    0xC0,        // End Collection

    // Consumer control: one 16-bit usage (0 = released), for media keys.
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array, Absolute)
    0xC0,              // End Collection
};

// Bump when the GATT table changes (services, characteristics, the report
// map). On the first boot with a new value the device sends Service Changed,
// which NimBLE delivers to bonded hosts when they next connect, so they
// rediscover instead of using a cached table.
#define GATT_REVISION 3

static NimBLEServer* server = nullptr;
static NimBLEHIDDevice* hid_dev = nullptr;
static NimBLECharacteristic* input_kbd = nullptr;
static NimBLECharacteristic* input_consumer = nullptr;
static NimBLECharacteristic* tx_char = nullptr;
static NimBLECharacteristic* rx_char = nullptr;
static NimBLECharacteristic* req_char = nullptr;

static ble_state_t state = BLE_STATE_INIT;
static bool need_advertise = false;

// One-shot supervision-timeout pushback (see onConnParamsUpdate). Written by
// NimBLE host-task callbacks, consumed by ble_tick() on the loop task.
static const uint16_t CONN_HANDLE_NONE  = 0xFFFF;
static const uint16_t DESIRED_TIMEOUT   = 600;   // ×10ms = 6s, matches PPCP
static volatile uint16_t param_fix_handle = CONN_HANDLE_NONE;  // pending retry
static volatile uint32_t param_fix_at_ms  = 0;                 // when to send it
static volatile uint16_t param_fix_spent  = CONN_HANDLE_NONE;  // one per connection
// RX writes land in rx_buf on the NimBLE host task (core 0); the loop parses
// on core 1. Both sides copy under rx_lock, and the loop parses its own copy
// (parse_buf), so a write never changes the text mid-parse.
static char rx_buf[BLE_BUF_SIZE];
static char parse_buf[BLE_BUF_SIZE];
static portMUX_TYPE rx_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool data_ready = false;
static volatile bool has_received_data = false;
static char mac_str[18];

// --- Single-owner lock -----------------------------------------------------
//
// The board is a BLE peripheral that any central in range could connect to and
// write usage data to. To stop the display rotating to another machine's
// account, it locks to ONE owner: the identity address of the machine it is
// bonded to, persisted in NVS. Only that owner (over a bonded+encrypted link)
// may write usage data; a second machine that pairs is rejected so the board
// stays paired to a single machine. The hold-power bond-clear gesture resets
// the owner so the board can be handed to a different machine.
static Preferences prefs;
static char owner_addr[18] = {0};   // owner identity address, e.g. "aa:bb:cc:dd:ee:ff"
static bool owner_set = false;
static const char* ZERO_ADDR = "00:00:00:00:00:00";

static void save_owner() {
    prefs.begin("clawd", false);
    prefs.putString("owner", owner_addr);
    prefs.end();
}

static void clear_owner() {
    owner_set = false;
    owner_addr[0] = '\0';
    prefs.begin("clawd", false);
    prefs.remove("owner");
    prefs.end();
}

static void load_owner() {
    prefs.begin("clawd", true);
    String o = prefs.getString("owner", "");
    prefs.end();
    if (o.length() == 17) {  // "aa:bb:cc:dd:ee:ff"
        strncpy(owner_addr, o.c_str(), sizeof(owner_addr) - 1);
        owner_addr[sizeof(owner_addr) - 1] = '\0';
        owner_set = true;
        Serial.printf("BLE: owner loaded = %s\n", owner_addr);
    }
}

// Delete every stored bond that isn't the owner, so the board stays paired to
// exactly one machine. Removing a bond shifts the indices, so restart from 0.
static void prune_foreign_bonds() {
    if (!owner_set) return;
    bool removed;
    do {
        removed = false;
        int n = NimBLEDevice::getNumBonds();
        for (int i = 0; i < n; i++) {
            NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
            if (strcmp(a.toString().c_str(), owner_addr) != 0) {
                Serial.printf("BLE: pruning non-owner bond %s\n", a.toString().c_str());
                NimBLEDevice::deleteBond(a);
                removed = true;
                break;
            }
        }
    } while (removed);
}

static void claim_owner(const std::string& id) {
    strncpy(owner_addr, id.c_str(), sizeof(owner_addr) - 1);
    owner_addr[sizeof(owner_addr) - 1] = '\0';
    owner_set = true;
    save_owner();
    Serial.printf("BLE: owner claimed = %s\n", owner_addr);
    prune_foreign_bonds();
}

static void start_advertising() {
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->reset();
    // Primary advertising packet (≤31 bytes):
    //   flags (3) + appearance (4) + HID service 0x1812 (4) + name "Clawdmeter" (12)
    //   = 23 bytes. macOS Bluetooth Settings only surfaces BLE-only devices
    //   that explicitly advertise the standard HID service UUID (0x1812) —
    //   without it the device is recognized internally but hidden from the
    //   GUI nearby-devices list.
    adv->setAppearance(HID_KEYBOARD);
    adv->addServiceUUID(NimBLEUUID((uint16_t)0x1812));  // BLE HID Service
    adv->setName(DEVICE_NAME);
    // Scan response carries the 128-bit custom data-service UUID for active
    // scanners (the host app scans actively).
    NimBLEAdvertisementData scanResp;
    scanResp.setCompleteServices(NimBLEUUID(SERVICE_UUID));
    adv->setScanResponseData(scanResp);
    adv->enableScanResponse(true);
    bool ok = adv->start();
    // Only reflect ADVERTISING in the UI state when no client is connected.
    // With MAX_CONNECTIONS=2, onConnect re-advertises to fill the second slot;
    // without this guard the UI would flip CONNECTED → ADVERTISING on every
    // first connect and never come back until a second client arrived.
    if (!server || server->getConnectedCount() == 0) {
        state = BLE_STATE_ADVERTISING;
    }
    Serial.printf("BLE: advertising start=%s (connected=%u)\n",
        ok ? "OK" : "FAILED",
        server ? (unsigned)server->getConnectedCount() : 0);
}

// Who gets an HID input report: the peers that subscribed to it over an
// encrypted link, so a central in the spare connection slot that never paired
// gets no keystrokes. NimBLE reports a bonded host's stored subscriptions when
// it reconnects as well as new ones. Each report goes out with its own bytes:
// a bare notify() only queues an update and NimBLE reads the characteristic
// when it sends, so reports set in quick succession (modifiers, key, release)
// would all go out as the last.
class HidReportSubscribers : public NimBLECharacteristicCallbacks {
public:
    HidReportSubscribers() {
        for (uint16_t& h : handles) h = CONN_HANDLE_NONE;
    }

    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        (void)chr;
        drop(info.getConnHandle());
        if (!(subValue & 1)) return;
        for (uint16_t& h : handles) {
            if (h == CONN_HANDLE_NONE) {
                h = info.getConnHandle();
                return;
            }
        }
    }

    void drop(uint16_t conn) {
        for (uint16_t& h : handles) {
            if (h == conn) h = CONN_HANDLE_NONE;
        }
    }

    // False if a subscribed peer didn't take it (no buffer free), to retry.
    bool send(NimBLECharacteristic* chr, const uint8_t* report, size_t len) const {
        bool ok = true;
        for (uint16_t h : handles) {
            if (h == CONN_HANDLE_NONE || !server->getPeerInfoByHandle(h).isEncrypted()) continue;
            ok = chr->notify(report, len, h) && ok;
        }
        return ok;
    }

private:
    uint16_t handles[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];
};

static HidReportSubscribers kbd_subscribers, consumer_subscribers;

// The last report of each kind that a subscriber didn't take; ble_tick()
// sends it again. A report is the whole state, so a resend can't reorder.
static uint8_t kbd_retry[8];
static uint8_t consumer_retry[2];
static bool kbd_retry_due = false, consumer_retry_due = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        state = BLE_STATE_CONNECTED;
        Serial.printf("BLE: connected from %s (active=%u)\n",
            info.getAddress().toString().c_str(),
            (unsigned)s->getConnectedCount());
        // Log negotiated link timing — the difference between guessing and
        // knowing when debugging disconnects (e.g. reason=520 supervision
        // timeouts are only explainable next to the negotiated timeout).
        // Units: interval ×1.25ms, timeout ×10ms, latency = skippable events.
        Serial.printf("BLE: connparams itvl=%u(%.2fms) lat=%u timeout=%u(%ums)\n",
            info.getConnInterval(), info.getConnInterval() * 1.25f,
            info.getConnLatency(), info.getConnTimeout(), info.getConnTimeout() * 10);
        // Keep advertising while a connection slot is still free so a second
        // central (e.g. the host app alongside an OS-held HID link) can
        // discover and connect. NimBLE auto-stops advertising on each accept.
        if (s->getConnectedCount() < CONFIG_BT_NIMBLE_MAX_CONNECTIONS) {
            need_advertise = true;
        }
        // Start pairing/encryption ourselves. RX writes are only accepted over
        // an encrypted link, but nothing in the GATT table forces the central
        // to pair; macOS can keep a cached GATT table after a bond clear and
        // never read a protected HID characteristic, leaving it stuck on
        // "Connecting" with an unencrypted link.
        if (!info.isEncrypted()) {
            int rc = 0;
            bool ok = NimBLEDevice::startSecurity(info.getConnHandle(), &rc);
            Serial.printf("BLE: startSecurity %s (rc=%d)\n", ok ? "OK" : "FAILED", rc);
        }
    }

    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        // Only flip the UI state to DISCONNECTED when the last client leaves.
        if (s->getConnectedCount() == 0) state = BLE_STATE_DISCONNECTED;
        need_advertise = true;
        // Drop any pending/spent param pushback for this handle — NimBLE
        // reuses conn handles, so stale state would leak onto the next link.
        if (param_fix_handle == info.getConnHandle()) param_fix_handle = CONN_HANDLE_NONE;
        if (param_fix_spent  == info.getConnHandle()) param_fix_spent  = CONN_HANDLE_NONE;
        kbd_subscribers.drop(info.getConnHandle());
        consumer_subscribers.drop(info.getConnHandle());
        Serial.printf("BLE: disconnected (reason=%d, remaining=%u)\n",
            reason, (unsigned)s->getConnectedCount());
    }

    // Centrals re-negotiate parameters mid-connection. Windows in particular
    // clamps the supervision timeout to 2s once an app GATT session goes
    // active (captured on hardware; it honors 9.6s while only its HID driver
    // holds the link) — and a 2s window is tight enough that ordinary radio
    // gaps kill the link (HCI 0x208 → reason=520), which caused constant
    // reconnect churn. Push back ONCE per connection: schedule a
    // deferred LL connection-parameter request for the same interval range but
    // a 6s timeout (the mechanism Microsoft's accessory guidelines prescribe).
    // Deferred ~2s so it can't race the central's own in-flight update
    // transaction (Windows has a documented late-instant bug there), and
    // one-shot so a central that re-clamps doesn't trigger an update war.
    void onConnParamsUpdate(NimBLEConnInfo& info) override {
        Serial.printf("BLE: connparams update itvl=%u(%.2fms) lat=%u timeout=%u(%ums)\n",
            info.getConnInterval(), info.getConnInterval() * 1.25f,
            info.getConnLatency(), info.getConnTimeout(), info.getConnTimeout() * 10);
        if (info.getConnTimeout() < DESIRED_TIMEOUT &&
            info.getConnHandle() != param_fix_spent) {
            param_fix_handle = info.getConnHandle();
            param_fix_at_ms  = millis() + 2000;
        }
    }

    // Lock the board to a single owner machine. The first machine to bond
    // becomes the owner; any other machine that pairs is un-bonded and dropped
    // so the board never shows (or rotates to) a second machine's account.
    void onAuthenticationComplete(NimBLEConnInfo& info) override {
        std::string id = info.getIdAddress().toString();
        Serial.printf("BLE: auth complete peer=%s bonded=%d enc=%d\n",
            id.c_str(), info.isBonded() ? 1 : 0, info.isEncrypted() ? 1 : 0);
        // Bonded reconnects START at the central's clamped parameters (no
        // later update event fires), so the supervision-timeout pushback must
        // also arm here, not just in onConnParamsUpdate.
        if (info.getConnTimeout() < DESIRED_TIMEOUT &&
            info.getConnHandle() != param_fix_spent) {
            param_fix_handle = info.getConnHandle();
            param_fix_at_ms  = millis() + 2000;
        }
        if (id == ZERO_ADDR) return;
        if (!owner_set) {
            claim_owner(id);
        } else if (strcmp(id.c_str(), owner_addr) != 0) {
            Serial.printf("BLE: rejecting non-owner %s (owner=%s)\n", id.c_str(), owner_addr);
            NimBLEDevice::deleteBond(info.getIdAddress());
            server->disconnect(info);
        }
    }

};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
        // Only accept usage data over a bonded+encrypted link, and only from the
        // owner machine. Another machine's app in range is ignored so the
        // display never rotates to a foreign account. The first encrypted writer
        // claims ownership when none is set yet (e.g. a fresh pairing).
        std::string id = info.getIdAddress().toString();
        if (!info.isEncrypted()) {
            Serial.println("BLE: dropping RX write from unencrypted link");
            return;
        }
        if (!owner_set && id != ZERO_ADDR) {
            claim_owner(id);
        }
        if (owner_set && strcmp(id.c_str(), owner_addr) != 0) {
            Serial.printf("BLE: dropping RX write from non-owner %s\n", id.c_str());
            return;
        }
        std::string val = chr->getValue();
        size_t len = std::min(val.length(), (size_t)(BLE_BUF_SIZE - 1));
        portENTER_CRITICAL(&rx_lock);
        memcpy(rx_buf, val.c_str(), len);
        rx_buf[len] = '\0';
        data_ready = true;
        portEXIT_CRITICAL(&rx_lock);
        has_received_data = true;
    }
};

// When the app enables notifications on the refresh char, ask for data
// if we have none yet. Firing on subscribe (not on connect) ensures the
// notification isn't dropped before the app's CCCD write completes.
class ReqCallbacks : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic* chr, NimBLEConnInfo& info, uint16_t subValue) override {
        Serial.printf("BLE: req_char onSubscribe subValue=%u has_data=%d\n", subValue, has_received_data ? 1 : 0);
        if (subValue != 0 && !has_received_data) {
            ble_request_refresh();
        }
    }
};

// Collects the stored subscriptions (CCCDs) to drop: every one but Service
// Changed's and the keyboard report's, whose handles every revision kept.
struct StaleCccds {
    uint16_t keep[2];
    int count;
    ble_store_key_cccd keys[MYNEWT_VAL(BLE_STORE_MAX_CCCDS)];
};

static int collect_stale_cccd(int obj_type, union ble_store_value* val, void* cookie) {
    (void)obj_type;
    StaleCccds* s = (StaleCccds*)cookie;
    if (val->cccd.chr_val_handle == s->keep[0] || val->cccd.chr_val_handle == s->keep[1]) return 0;
    if (s->count < (int)(sizeof(s->keys) / sizeof(s->keys[0])))
        ble_store_key_from_value_cccd(&s->keys[s->count++], &val->cccd);
    return 0;
}

// A firmware with a different GATT table than the one bonded hosts saw:
// drop their stored subscriptions to handles that moved, which would
// otherwise fill the CCCD store, then tell them (Service Changed, held by
// NimBLE for each bonded host that subscribed to it until it reconnects).
// Hosts re-subscribe after rediscovering. Hosts that don't act on Service
// Changed need the board forgotten and paired again (docs/user-guide.md).
static void gatt_revision_check() {
    prefs.begin("clawd", true);
    const int seen = prefs.getInt("gatt_rev", 1);
    prefs.end();
    if (seen == GATT_REVISION) return;
    static StaleCccds stale;
    stale.count = 0;
    stale.keep[1] = input_kbd->getHandle();
    const NimBLEUUID gatt_svc((uint16_t)0x1801), service_changed((uint16_t)0x2A05);
    if (ble_gatts_find_chr(gatt_svc.getBase(), service_changed.getBase(), nullptr, &stale.keep[0]) != 0) {
        Serial.println("BLE: no Service Changed characteristic; GATT revision check skipped");
        return;
    }
    ble_store_iterate(BLE_STORE_OBJ_TYPE_CCCD, collect_stale_cccd, &stale);
    for (int i = 0; i < stale.count; i++) ble_store_delete_cccd(&stale.keys[i]);
    server->sendServiceChangedIndication();
    prefs.begin("clawd", false);
    prefs.putInt("gatt_rev", GATT_REVISION);
    prefs.end();
    Serial.printf("BLE: GATT revision %d -> %d, dropped %d stale subscriptions, Service Changed sent to bonded hosts\n",
                  seen, GATT_REVISION, stale.count);
}

void ble_init(void) {
    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setSecurityAuth(true, false, true);  // bonding, no MITM, SC

    // Restore the locked owner (if any) and drop any stale non-owner bonds so
    // the board stays paired to a single machine across reboots.
    load_owner();
    prune_foreign_bonds();

    // Format MAC address
    NimBLEAddress addr = NimBLEDevice::getAddress();
    snprintf(mac_str, sizeof(mac_str), "%s", addr.toString().c_str());
    for (int i = 0; mac_str[i]; i++) {
        if (mac_str[i] >= 'a' && mac_str[i] <= 'f') mac_str[i] -= 32;
    }

    server = NimBLEDevice::createServer();
    static ServerCallbacks serverCb;
    server->setCallbacks(&serverCb);

    // --- HID keyboard service ---
    hid_dev = new NimBLEHIDDevice(server);
    hid_dev->setReportMap((uint8_t*)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
    hid_dev->setManufacturer("Clawdmeter");
    // PnP ID: (vendorIdSource, vendorId, productId, version).
    // Source 1 = Bluetooth SIG, vendor 0x02E5 = Espressif. Don't claim
    // Apple's IDs (USB vendor 0x05AC): macOS validates Apple-claimed HIDs
    // against known devices and silently refuses to surface a Connect button
    // for spoofers.
    hid_dev->setPnp(0x01, 0x02E5, 0x0001, 0x0100);
    // country=33 (US ANSI). Setting this to 0 ("not supported") causes macOS
    // to launch the Keyboard Setup Assistant on first pair asking the user
    // to identify the layout. Keys are HID usages (key positions); the host
    // applies its own layout, and the app records shortcuts as US positions.
    hid_dev->setHidInfo(33, 0x02);
    hid_dev->setBatteryLevel(100);
    input_kbd = hid_dev->getInputReport(1);       // report ID 1
    input_consumer = hid_dev->getInputReport(2);  // report ID 2
    input_kbd->setCallbacks(&kbd_subscribers);
    input_consumer->setCallbacks(&consumer_subscribers);

    // --- Custom data service ---
    NimBLEService* svc = server->createService(SERVICE_UUID);

    rx_char = svc->createCharacteristic(
        RX_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    static RxCallbacks rxCb;
    rx_char->setCallbacks(&rxCb);

    tx_char = svc->createCharacteristic(
        TX_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    req_char = svc->createCharacteristic(
        REQ_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY
    );
    static ReqCallbacks reqCb;
    req_char->setCallbacks(&reqCb);

    svc->start();
    server->start();
    gatt_revision_check();
    start_advertising();

    Serial.printf("BLE: init complete, MAC=%s\n", mac_str);
}

void ble_tick(void) {
    if (need_advertise) {
        need_advertise = false;
        start_advertising();
    }
    if (state != BLE_STATE_CONNECTED) {
        kbd_retry_due = consumer_retry_due = false;
    } else {
        if (kbd_retry_due) kbd_retry_due = !kbd_subscribers.send(input_kbd, kbd_retry, sizeof(kbd_retry));
        if (consumer_retry_due)
            consumer_retry_due = !consumer_subscribers.send(input_consumer, consumer_retry, sizeof(consumer_retry));
    }
    // Deferred one-shot supervision-timeout pushback (see onConnParamsUpdate).
    if (param_fix_handle != CONN_HANDLE_NONE &&
        (int32_t)(millis() - param_fix_at_ms) >= 0) {
        uint16_t h = param_fix_handle;
        param_fix_handle = CONN_HANDLE_NONE;
        param_fix_spent  = h;
        if (server && server->getConnectedCount() > 0) {
            Serial.println("BLE: requesting 6s supervision timeout");
            server->updateConnParams(h, 12, 24, 0, DESIRED_TIMEOUT);
        }
    }
}

ble_state_t ble_get_state(void) {
    return state;
}

const char* ble_get_device_name(void) {
    return DEVICE_NAME;
}

const char* ble_get_mac_address(void) {
    return mac_str;
}

void ble_clear_bonds(void) {
    // Delete bonds one at a time (always index 0: each delete shifts the list)
    // and report failures: deleteAllBonds() bails silently on the first error,
    // leaving a stale bond in NVS that breaks re-pairing.
    for (int guard = NimBLEDevice::getNumBonds(); guard > 0 && NimBLEDevice::getNumBonds() > 0; guard--) {
        NimBLEAddress a = NimBLEDevice::getBondedAddress(0);
        bool ok = NimBLEDevice::deleteBond(a);
        if (!ok) {
            // ble_gap_unpair() stops at the first missing record (e.g. our_sec
            // already gone) and leaves the peer's security entry behind, so
            // delete each record directly and ignore "not found".
            struct ble_store_key_sec key;
            memset(&key, 0, sizeof(key));
            key.peer_addr = *a.getBase();
            int r1 = ble_store_delete_peer_sec(&key);
            int r2 = ble_store_delete_our_sec(&key);
            Serial.printf("BLE: unpair failed; direct delete peer_sec rc=%d our_sec rc=%d\n", r1, r2);
        }
        Serial.printf("BLE: delete bond %s -> %s\n", a.toString().c_str(), ok ? "ok" : "forced");
    }
    clear_owner();  // release ownership so the board can be handed to another machine
    Serial.printf("BLE: bonds cleared (remaining=%d)\n", NimBLEDevice::getNumBonds());
    if (state == BLE_STATE_CONNECTED) {
        server->disconnect(server->getPeerInfo(0).getConnHandle());
    }
    need_advertise = true;
}

void ble_print_status(void) {
    static const char* names[] = {"INIT", "ADVERTISING", "CONNECTED", "DISCONNECTED"};
    Serial.printf("BLE status: state=%s bonds=%d owner=%s\n",
        names[state], NimBLEDevice::getNumBonds(), owner_set ? owner_addr : "none");
    if (!server) return;
    for (uint8_t i = 0; i < server->getConnectedCount(); i++) {
        NimBLEConnInfo ci = server->getPeerInfo(i);
        Serial.printf("BLE status: peer=%s id=%s enc=%d bonded=%d\n",
            ci.getAddress().toString().c_str(), ci.getIdAddress().toString().c_str(),
            ci.isEncrypted() ? 1 : 0, ci.isBonded() ? 1 : 0);
    }
}

void ble_disconnect_all(void) {
    if (!server) return;
    // Collect the handles first: each disconnect shrinks the peer list.
    uint16_t handles[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];
    uint8_t n = 0;
    for (uint8_t i = 0; i < server->getConnectedCount() && n < CONFIG_BT_NIMBLE_MAX_CONNECTIONS; i++) {
        handles[n++] = server->getPeerInfo(i).getConnHandle();
    }
    for (uint8_t i = 0; i < n; i++) server->disconnect(handles[i]);
}

bool ble_has_data(void) {
    return data_ready;
}

const char* ble_get_data(void) {
    portENTER_CRITICAL(&rx_lock);
    memcpy(parse_buf, rx_buf, sizeof(parse_buf));
    data_ready = false;
    portEXIT_CRITICAL(&rx_lock);
    return parse_buf;
}

void ble_send_ack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"ack\":true}");
        tx_char->notify();
    }
}

void ble_send_nack(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"err\":true}");
        tx_char->notify();
    }
}

void ble_send_dismiss(void) {
    if (state == BLE_STATE_CONNECTED && tx_char) {
        tx_char->setValue("{\"dismiss\":true}");
        tx_char->notify();
        Serial.println("BLE: alert dismissed on the device");
    }
}

void ble_set_battery_level(int pct) {
    if (!hid_dev || pct < 0) return;
    if (pct > 100) pct = 100;
    hid_dev->setBatteryLevel((uint8_t)pct, state == BLE_STATE_CONNECTED);
}

void ble_request_refresh(void) {
    if (state == BLE_STATE_CONNECTED && req_char) {
        uint8_t v = 0x01;
        req_char->setValue(&v, 1);
        req_char->notify();
        Serial.println("BLE: refresh requested");
    }
}

void ble_keyboard_report(const uint8_t report[8]) {
    if (state != BLE_STATE_CONNECTED || !input_kbd) return;
    input_kbd->setValue(report, 8);
    memcpy(kbd_retry, report, sizeof(kbd_retry));
    kbd_retry_due = !kbd_subscribers.send(input_kbd, report, 8);
}

void ble_consumer_press(uint16_t usage) {
    if (state != BLE_STATE_CONNECTED || !input_consumer) return;
    uint8_t report[2] = {(uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8)};
    input_consumer->setValue(report, sizeof(report));
    memcpy(consumer_retry, report, sizeof(consumer_retry));
    consumer_retry_due = !consumer_subscribers.send(input_consumer, report, sizeof(report));
}

void ble_consumer_release(void) {
    ble_consumer_press(0);
}
