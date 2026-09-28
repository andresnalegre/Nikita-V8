#pragma once

#include "bt.h"

#include <furi.h>
#include <furi_hal.h>
#include <api_lock.h>

#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/view.h>

#include <dialogs/dialogs.h>
#include <power/power_service/power.h>
#include <rpc/rpc.h>
#include <notification/notification.h>
#include <storage/storage.h>

#include <bt/bt_settings.h>
#include <bt/bt_service/bt_keys_storage.h>

#include "bt_keys_filename.h"

#define BT_KEYS_STORAGE_PATH INT_PATH(BT_KEYS_STORAGE_FILE_NAME)

// Dual-link: one RPC session per BLE connection (up to CFG_BLE_NUM_LINK). Each
// peer carries its own session, its own send/ack event flag, and the BLE
// connection handle used to route TX and flow control to that client only.
#define BT_PEERS_MAX 2

typedef struct {
    struct Bt* bt; // back-reference (context for RPC/serial callbacks)
    uint16_t conn_handle;
    bool used;
    RpcSession* rpc_session;
    FuriEventFlag* rpc_event;
} BtPeer;

typedef enum {
    BtMessageTypeUpdateStatus,
    BtMessageTypeUpdateBatteryLevel,
    BtMessageTypeUpdatePowerState,
    BtMessageTypePinCodeShow,
    BtMessageTypeKeysStorageUpdated,
    BtMessageTypeSetProfile,
    BtMessageTypeDisconnect,
    BtMessageTypeForgetBondedDevices,
    BtMessageTypeGetSettings,
    BtMessageTypeSetSettings,
    BtMessageTypeReloadKeysSettings,
} BtMessageType;

typedef struct {
    uint8_t* start_address;
    uint16_t size;
} BtKeyStorageUpdateData;

typedef union {
    uint32_t pin_code;
    uint8_t battery_level;
    bool power_state_charging;
    struct {
        const FuriHalBleProfileTemplate* template;
        FuriHalBleProfileParams params;
    } profile;
    FuriHalBleProfileParams profile_params;
    BtKeyStorageUpdateData key_storage_data;
    BtSettings* settings;
    const BtSettings* csettings;
} BtMessageData;

typedef struct {
    FuriApiLock lock;
    BtMessageType type;
    BtMessageData data;
    bool* result;
    FuriHalBleProfileBase** profile_instance;
} BtMessage;

struct Bt {
    uint8_t* bt_keys_addr_start;
    uint16_t bt_keys_size;
    uint16_t max_packet_size;
    BtSettings bt_settings;
    BtKeysStorage* keys_storage;
    BtStatus status;
    bool beacon_active;
    FuriHalBleProfileBase* current_profile;
    FuriMessageQueue* message_queue;
    NotificationApp* notification;
    Gui* gui;
    ViewPort* statusbar_view_port;
    ViewPort* pin_code_view_port;
    uint32_t pin_code;
    DialogsApp* dialogs;
    DialogMessage* dialog_message;
    Power* power;
    Rpc* rpc;
    RpcSession* rpc_session; // legacy/unused in the dual-link data path
    FuriEventFlag* rpc_event; // legacy/unused in the dual-link data path
    BtPeer peers[BT_PEERS_MAX];
    FuriEventFlag* api_event;
    BtStatusChangedCallback status_changed_cb;
    void* status_changed_ctx;
    uint32_t pin;
    bool suppress_pin_screen;
};

/** Open a new RPC connection for a BLE link (dual-link: one per connection)
 *
 * @param bt                    Bt instance
 * @param conn_handle           BLE connection handle of the client
 */
void bt_open_rpc_connection(Bt* bt, uint16_t conn_handle);

/** Close the RPC connection for one BLE link
 *
 * @param bt                    Bt instance
 * @param conn_handle           BLE connection handle of the client
 */
void bt_close_rpc_connection_peer(Bt* bt, uint16_t conn_handle);

/** Close ALL RPC connections
 *
 * @param bt                    Bt instance
 */
void bt_close_rpc_connection(Bt* bt);
