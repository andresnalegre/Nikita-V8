# Dual BLE link (2 simultaneous RPC clients) — design

Goal: let TWO clients (e.g. the iOS app AND the qFlipper desktop app) stay
connected over BLE at the same time, each with its own working RPC session, so
the user never has to choose one or the other and both share state through the
SD mailbox. Today the firmware is single-peer: it stops advertising on the first
connection and keeps exactly one `rpc_session`.

## Why it is possible on this hardware

- The radio stack already allows 2 links: `CFG_BLE_NUM_LINK 2` (app_conf.h). This
  is NOT a core2/radio change — only app-level BLE glue.
- Per-connection RX is available: `aci_gatt_attribute_modified_event_rp0` carries
  `Connection_Handle` as its first field, so an incoming write tells us WHICH
  client sent it.
- Per-connection TX is available: `aci_gatt_update_char_value_ext(Conn_Handle_To_Notify, ...)`
  targets a single connection. Today serial_service passes `0` (notify all) —
  that is the ONLY reason two clients would cross-talk. Passing the real handle
  keeps each client's RPC stream private.

## The single-peer coupling points (what must become per-peer)

1. `gap.c` — stops advertising on connect (HCI_LE_CONNECTION_COMPLETE) and only
   restarts on disconnect; tracks ONE `service.connection_handle`.
   -> Keep advertising until `CFG_BLE_NUM_LINK` links are up; track a small array
      of handles; restart advertising on any disconnect while below the max.
2. `services/serial_service.c` — one `{callback, context, buff_size,
   bytes_ready_to_receive}` and TX/flow-control notify to `0` (all).
   -> Per-peer slots keyed by connection handle. RX handler routes by
      `attribute_modified->Connection_Handle`. New `_to` TX/flow helpers take a
      conn handle and pass it to `_ext`. Old API kept (broadcasts) for ABI.
3. `profiles/serial_profile.c` — thin pass-through; add per-conn variants.
4. `bt_service/bt.c` — one `rpc_session`, one `current_profile`.
   -> Up to 2 `rpc_session`s keyed by handle; serial RX -> the matching session;
      each session's TX -> its handle. GapEventTypeConnected/Disconnected carry
      the handle so bt.c can open/close the right session.

## ABI safety

`ble_svc_serial_update_tx`, `ble_profile_serial_tx`, `ble_svc_serial_set_callbacks`
are in `api_symbols.csv` (the .fap ABI). Do NOT change their signatures. ADD new
symbols (`*_to` taking a connection handle) and leave the originals as
broadcast-to-all wrappers.

## Fallback / flexibility (already true, keep it)

The Flipper runs BLE-RPC and USB-CDC-RPC at the same time already, so
"iOS on BLE + qFlipper on USB" also works and is the zero-risk path. Dual BLE is
the extra option the user asked for on top of that.

## Test plan (hardware present, reflash over USB is safe)

1. Build + flash. Verify ONE client still connects and RPC works (no regression).
2. Connect a second client; verify both stay connected (advertising resumed).
3. Drive the agent mailbox from BOTH; verify each `res` is correct and streams do
   not cross (the cross-talk check).
4. Disconnect one; verify the other keeps working and advertising resumes.
