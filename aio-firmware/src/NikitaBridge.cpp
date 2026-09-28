// NikitaBridge implementation — GPIO heartbeat only (no network command server).
#include "NikitaBridge.h"

NikitaBridge nikitaBridge;

void NikitaBridge::begin() {
    _started = true;
    _lastBeat = millis();
    // Announce once at boot so the Flipper sees the board come up immediately.
    Serial.printf("[NIKITA-AIO:UP:%s]\r\n", NIKITA_FW_VERSION);
}

void NikitaBridge::loop() {
    if(!_started) return;
    uint32_t now = millis();
    if(now - _lastBeat >= NIKITA_HEARTBEAT_MS) {
        _lastBeat = now;
        // A recognizable marker on the UART. The Flipper (Nikita-V8) watches for
        // "[NIKITA-AIO:UP" -> shows "GPIO UP!"; its absence past a timeout ->
        // "GPIO DOWN". Harmless to any other UART reader (a single tagged line).
        Serial.printf("[NIKITA-AIO:UP:%s]\r\n", NIKITA_FW_VERSION);
    }
}
