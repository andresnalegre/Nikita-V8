#include "serial_service.h"
#include "app_common.h"
#include <ble/ble.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>

#include <furi.h>

#include "serial_service_uuid.inc"
#include <stdint.h>

#define TAG "BtSerialSvc"

typedef enum {
    SerialSvcGattCharacteristicRx = 0,
    SerialSvcGattCharacteristicTx,
    SerialSvcGattCharacteristicFlowCtrl,
    SerialSvcGattCharacteristicStatus,
    SerialSvcGattCharacteristicCount,
} SerialSvcGattCharacteristicId;

static const BleGattCharacteristicParams ble_svc_serial_chars[SerialSvcGattCharacteristicCount] = {
    [SerialSvcGattCharacteristicRx] =
        {.name = "RX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = BLE_SVC_SERIAL_DATA_LEN_MAX,
         .uuid.Char_UUID_128 = BLE_SVC_SERIAL_RX_CHAR_UUID,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_WRITE_WITHOUT_RESP | CHAR_PROP_WRITE | CHAR_PROP_READ,
         .security_permissions = ATTR_PERMISSION_AUTHEN_READ | ATTR_PERMISSION_AUTHEN_WRITE,
         .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [SerialSvcGattCharacteristicTx] =
        {.name = "TX",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = BLE_SVC_SERIAL_DATA_LEN_MAX,
         .uuid.Char_UUID_128 = BLE_SVC_SERIAL_TX_CHAR_UUID,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_INDICATE,
         .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_VARIABLE},
    [SerialSvcGattCharacteristicFlowCtrl] =
        {.name = "Flow control",
         .data_prop_type = FlipperGattCharacteristicDataFixed,
         .data.fixed.length = sizeof(uint32_t),
         .uuid.Char_UUID_128 = BLE_SVC_SERIAL_FLOW_CONTROL_UUID,
         .uuid_type = UUID_TYPE_128,
         .char_properties = CHAR_PROP_READ | CHAR_PROP_NOTIFY,
         .security_permissions = ATTR_PERMISSION_AUTHEN_READ,
         .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
         .is_variable = CHAR_VALUE_LEN_CONSTANT},
    [SerialSvcGattCharacteristicStatus] = {
        .name = "RPC status",
        .data_prop_type = FlipperGattCharacteristicDataFixed,
        .data.fixed.length = sizeof(uint32_t),
        .uuid.Char_UUID_128 = BLE_SVC_SERIAL_RPC_STATUS_UUID,
        .uuid_type = UUID_TYPE_128,
        .char_properties = CHAR_PROP_READ | CHAR_PROP_WRITE | CHAR_PROP_NOTIFY,
        .security_permissions = ATTR_PERMISSION_AUTHEN_READ | ATTR_PERMISSION_AUTHEN_WRITE,
        .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
        .is_variable = CHAR_VALUE_LEN_CONSTANT}};

// Dual-link: the firmware can hold CFG_BLE_NUM_LINK (2) BLE connections at once,
// each with its OWN RPC session. The serial GATT attributes are shared, so we
// route by BLE connection handle: RX (ACI_GATT_ATTRIBUTE_MODIFIED) and the TX
// ack (ACI_GATT_SERVER_CONFIRMATION) both carry Connection_Handle, and TX itself
// targets one client via aci_gatt_update_char_value_ext(conn_handle, ...). Each
// peer keeps its own flow-control window (bytes_ready_to_receive).
#define BLE_SVC_SERIAL_PEERS   2
#define BLE_SVC_SERIAL_CONN_ANY 0xFFFF

typedef struct {
    bool used;
    uint16_t conn_handle; // BLE_SVC_SERIAL_CONN_ANY -> match any (single-peer compat)
    SerialServiceEventCallback callback;
    void* context;
    uint32_t buff_size;
    uint16_t bytes_ready_to_receive;
} SerialServicePeer;

struct BleServiceSerial {
    uint16_t svc_handle;
    BleGattCharacteristicInstance chars[SerialSvcGattCharacteristicCount];
    FuriMutex* buff_size_mtx;
    SerialServicePeer peers[BLE_SVC_SERIAL_PEERS];
    GapSvcEventHandler* event_handler;
};

// Find the peer for a connection handle. Falls back to a CONN_ANY peer (the
// single-peer/legacy path) so old callers that never set a handle still work.
static SerialServicePeer*
    serial_svc_find_peer(BleServiceSerial* serial_svc, uint16_t conn_handle) {
    SerialServicePeer* wildcard = NULL;
    for(uint8_t i = 0; i < BLE_SVC_SERIAL_PEERS; i++) {
        if(!serial_svc->peers[i].used) continue;
        if(serial_svc->peers[i].conn_handle == conn_handle) return &serial_svc->peers[i];
        if(serial_svc->peers[i].conn_handle == BLE_SVC_SERIAL_CONN_ANY)
            wildcard = &serial_svc->peers[i];
    }
    return wildcard;
}

static SerialServicePeer* serial_svc_first_peer(BleServiceSerial* serial_svc) {
    for(uint8_t i = 0; i < BLE_SVC_SERIAL_PEERS; i++) {
        if(serial_svc->peers[i].used) return &serial_svc->peers[i];
    }
    return NULL;
}

// Per-connection flow-control notify: tell THIS client how much it may send.
static void serial_svc_send_flow_ctrl(BleServiceSerial* serial_svc, SerialServicePeer* peer) {
    uint32_t buff_size_reversed = REVERSE_BYTES_U32(peer->buff_size);
    if(peer->conn_handle == BLE_SVC_SERIAL_CONN_ANY) {
        ble_gatt_characteristic_update(
            serial_svc->svc_handle,
            &serial_svc->chars[SerialSvcGattCharacteristicFlowCtrl],
            &buff_size_reversed);
    } else {
        aci_gatt_update_char_value_ext(
            peer->conn_handle,
            serial_svc->svc_handle,
            serial_svc->chars[SerialSvcGattCharacteristicFlowCtrl].handle,
            0x01, // notify
            sizeof(uint32_t),
            0,
            sizeof(uint32_t),
            (const uint8_t*)&buff_size_reversed);
    }
}

static BleEventAckStatus ble_svc_serial_event_handler(void* event, void* context) {
    BleServiceSerial* serial_svc = (BleServiceSerial*)context;
    BleEventAckStatus ret = BleEventNotAck;
    hci_event_pckt* event_pckt = (hci_event_pckt*)(((hci_uart_pckt*)event)->data);
    evt_blecore_aci* blecore_evt = (evt_blecore_aci*)event_pckt->data;
    aci_gatt_attribute_modified_event_rp0* attribute_modified;
    if(event_pckt->evt == HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE) {
        if(blecore_evt->ecode == ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE) {
            attribute_modified = (aci_gatt_attribute_modified_event_rp0*)blecore_evt->data;
            if(attribute_modified->Attr_Handle ==
               serial_svc->chars[SerialSvcGattCharacteristicRx].handle + 2) {
                // Descriptor handle
                ret = BleEventAckFlowEnable;
                FURI_LOG_D(TAG, "RX descriptor event");
            } else if(
                attribute_modified->Attr_Handle ==
                serial_svc->chars[SerialSvcGattCharacteristicRx].handle + 1) {
                FURI_LOG_D(TAG, "Received %d bytes", attribute_modified->Attr_Data_Length);
                // Route this write to the peer that sent it (by connection handle).
                SerialServicePeer* peer =
                    serial_svc_find_peer(serial_svc, attribute_modified->Connection_Handle);
                if(peer && peer->callback) {
                    furi_check(
                        furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) ==
                        FuriStatusOk);
                    if(attribute_modified->Attr_Data_Length > peer->bytes_ready_to_receive) {
                        FURI_LOG_W(
                            TAG,
                            "Received %d, while was ready to receive %d bytes. Can lead to buffer overflow!",
                            attribute_modified->Attr_Data_Length,
                            peer->bytes_ready_to_receive);
                    }
                    peer->bytes_ready_to_receive -=
                        MIN(peer->bytes_ready_to_receive, attribute_modified->Attr_Data_Length);
#ifndef LOGS_RELEASE_BUILD
                    SerialServiceEvent event = {
                        .event = SerialServiceEventTypeDataReceived,
                        .data = {
                            .buffer = attribute_modified->Attr_Data,
                            .size = attribute_modified->Attr_Data_Length,
                        }};
                    uint32_t buff_free_size = peer->callback(event, peer->context);
                    FURI_LOG_D(TAG, "Available buff size: %ld", buff_free_size);
#else
                    SerialServiceEvent event = {
                        .event = SerialServiceEventTypeDataReceived,
                        .data = {
                            .buffer = attribute_modified->Attr_Data,
                            .size = attribute_modified->Attr_Data_Length,
                        }};
                    peer->callback(event, peer->context);
#endif
                    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
                }
                ret = BleEventAckFlowEnable;
            } else if(
                attribute_modified->Attr_Handle ==
                serial_svc->chars[SerialSvcGattCharacteristicStatus].handle + 1) {
                bool* rpc_status = (bool*)attribute_modified->Attr_Data;
                if(!*rpc_status) {
                    SerialServicePeer* peer =
                        serial_svc_find_peer(serial_svc, attribute_modified->Connection_Handle);
                    if(peer && peer->callback) {
                        SerialServiceEvent event = {
                            .event = SerialServiceEventTypesBleResetRequest,
                        };
                        peer->callback(event, peer->context);
                    }
                }
            }
        } else if(blecore_evt->ecode == ACI_GATT_SERVER_CONFIRMATION_VSEVT_CODE) {
            FURI_LOG_T(TAG, "Ack received");
            // The TX-sent ack carries the connection handle -> wake the right peer.
            aci_gatt_server_confirmation_event_rp0* confirmation =
                (aci_gatt_server_confirmation_event_rp0*)blecore_evt->data;
            SerialServicePeer* peer =
                serial_svc_find_peer(serial_svc, confirmation->Connection_Handle);
            if(peer && peer->callback) {
                SerialServiceEvent event = {
                    .event = SerialServiceEventTypeDataSent,
                };
                peer->callback(event, peer->context);
            }
            ret = BleEventAckFlowEnable;
        }
    }
    return ret;
}

typedef enum {
    SerialServiceRpcStatusNotActive = 0UL,
    SerialServiceRpcStatusActive = 1UL,
} SerialServiceRpcStatus;

static void
    ble_svc_serial_update_rpc_char(BleServiceSerial* serial_svc, SerialServiceRpcStatus status) {
    ble_gatt_characteristic_update(
        serial_svc->svc_handle, &serial_svc->chars[SerialSvcGattCharacteristicStatus], &status);
}

BleServiceSerial* ble_svc_serial_start(void) {
    BleServiceSerial* serial_svc = malloc(sizeof(BleServiceSerial));

    serial_svc->event_handler =
        ble_event_dispatcher_register_svc_handler(ble_svc_serial_event_handler, serial_svc);

    if(!ble_gatt_service_add(
           UUID_TYPE_128, &service_uuid, PRIMARY_SERVICE, 12, &serial_svc->svc_handle)) {
        free(serial_svc);
        return NULL;
    }
    for(uint8_t i = 0; i < SerialSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_init(
            serial_svc->svc_handle, &ble_svc_serial_chars[i], &serial_svc->chars[i]);
    }

    ble_svc_serial_update_rpc_char(serial_svc, SerialServiceRpcStatusNotActive);
    serial_svc->buff_size_mtx = furi_mutex_alloc(FuriMutexTypeNormal);
    for(uint8_t i = 0; i < BLE_SVC_SERIAL_PEERS; i++) {
        serial_svc->peers[i].used = false;
    }

    return serial_svc;
}

// Register a peer for a specific BLE connection. Each connected client gets its
// own peer slot (own callback/context and own flow-control window).
void ble_svc_serial_add_peer(
    BleServiceSerial* serial_svc,
    uint16_t conn_handle,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    furi_check(serial_svc);
    furi_check(furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) == FuriStatusOk);
    SerialServicePeer* peer = serial_svc_find_peer(serial_svc, conn_handle);
    if(!peer) {
        for(uint8_t i = 0; i < BLE_SVC_SERIAL_PEERS; i++) {
            if(!serial_svc->peers[i].used) {
                peer = &serial_svc->peers[i];
                break;
            }
        }
    }
    if(peer) {
        peer->used = true;
        peer->conn_handle = conn_handle;
        peer->callback = callback;
        peer->context = context;
        peer->buff_size = buff_size;
        peer->bytes_ready_to_receive = buff_size;
        serial_svc_send_flow_ctrl(serial_svc, peer);
    } else {
        FURI_LOG_E(TAG, "No free serial peer slot for conn %d", conn_handle);
    }
    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
}

void ble_svc_serial_remove_peer(BleServiceSerial* serial_svc, uint16_t conn_handle) {
    furi_check(serial_svc);
    furi_check(furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) == FuriStatusOk);
    SerialServicePeer* peer = serial_svc_find_peer(serial_svc, conn_handle);
    if(peer) {
        peer->used = false;
        peer->callback = NULL;
        peer->context = NULL;
    }
    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
}

// Legacy single-peer API (kept for the .fap ABI): registers one CONN_ANY peer.
void ble_svc_serial_set_callbacks(
    BleServiceSerial* serial_svc,
    uint16_t buff_size,
    SerialServiceEventCallback callback,
    void* context) {
    ble_svc_serial_add_peer(serial_svc, BLE_SVC_SERIAL_CONN_ANY, buff_size, callback, context);
}

// Per-connection flow control: replenish THIS peer's receive window.
void ble_svc_serial_notify_buffer_is_empty_to(BleServiceSerial* serial_svc, uint16_t conn_handle) {
    furi_check(serial_svc);
    furi_check(serial_svc->buff_size_mtx);

    furi_check(furi_mutex_acquire(serial_svc->buff_size_mtx, FuriWaitForever) == FuriStatusOk);
    SerialServicePeer* peer = serial_svc_find_peer(serial_svc, conn_handle);
    if(peer && peer->bytes_ready_to_receive == 0) {
        FURI_LOG_D(TAG, "Buffer is empty. Notifying client %d", conn_handle);
        peer->bytes_ready_to_receive = peer->buff_size;
        serial_svc_send_flow_ctrl(serial_svc, peer);
    }
    furi_check(furi_mutex_release(serial_svc->buff_size_mtx) == FuriStatusOk);
}

// Legacy: operate on the first registered peer.
void ble_svc_serial_notify_buffer_is_empty(BleServiceSerial* serial_svc) {
    furi_check(serial_svc);
    SerialServicePeer* peer = serial_svc_first_peer(serial_svc);
    if(peer) ble_svc_serial_notify_buffer_is_empty_to(serial_svc, peer->conn_handle);
}

void ble_svc_serial_stop(BleServiceSerial* serial_svc) {
    furi_check(serial_svc);

    ble_event_dispatcher_unregister_svc_handler(serial_svc->event_handler);

    for(uint8_t i = 0; i < SerialSvcGattCharacteristicCount; i++) {
        ble_gatt_characteristic_delete(serial_svc->svc_handle, &serial_svc->chars[i]);
    }
    ble_gatt_service_delete(serial_svc->svc_handle);
    furi_mutex_free(serial_svc->buff_size_mtx);
    free(serial_svc);
}

// Send to ONE client (conn_handle). conn_handle 0 notifies whoever is subscribed
// (legacy broadcast); a real handle keeps this client's RPC stream private.
bool ble_svc_serial_update_tx_to(
    BleServiceSerial* serial_svc,
    uint16_t conn_handle,
    uint8_t* data,
    uint16_t data_len) {
    if(data_len > BLE_SVC_SERIAL_DATA_LEN_MAX) {
        return false;
    }

    for(uint16_t remained = data_len; remained > 0;) {
        uint8_t value_len = MIN(BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX, remained);
        uint16_t value_offset = data_len - remained;
        remained -= value_len;

        tBleStatus result = aci_gatt_update_char_value_ext(
            conn_handle,
            serial_svc->svc_handle,
            serial_svc->chars[SerialSvcGattCharacteristicTx].handle,
            remained ? 0x00 : 0x02,
            data_len,
            value_offset,
            value_len,
            data + value_offset);

        if(result) {
            FURI_LOG_E(TAG, "Failed updating TX characteristic: %d", result);
            return false;
        }
    }

    return true;
}

// Legacy: send to the first registered peer (or broadcast if none/CONN_ANY).
bool ble_svc_serial_update_tx(BleServiceSerial* serial_svc, uint8_t* data, uint16_t data_len) {
    SerialServicePeer* peer = serial_svc_first_peer(serial_svc);
    uint16_t conn = (peer && peer->conn_handle != BLE_SVC_SERIAL_CONN_ANY) ? peer->conn_handle : 0;
    return ble_svc_serial_update_tx_to(serial_svc, conn, data, data_len);
}

void ble_svc_serial_set_rpc_active(BleServiceSerial* serial_svc, bool active) {
    furi_check(serial_svc);
    ble_svc_serial_update_rpc_char(
        serial_svc, active ? SerialServiceRpcStatusActive : SerialServiceRpcStatusNotActive);
}
