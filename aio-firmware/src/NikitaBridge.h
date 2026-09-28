// NikitaBridge — the "Nikita v2.0.0" additions on top of ESP32 Marauder.
//
// Scope is deliberately SMALL and safe: it does NOT open any network command
// server (that would be a remote-code-execution surface). Nikita reaches the
// board through the FLIPPER's existing, user-controlled channel (the WIFI app
// mailbox -> UART -> Marauder CommandLine). This module only adds a status
// HEARTBEAT on the UART so the Flipper (Nikita-V8) can show "GPIO UP!" when the
// board is present and "GPIO DOWN" when it disappears.
#pragma once

#include <Arduino.h>

#define NIKITA_FW_VERSION   "v2.0.0"
#define NIKITA_HEARTBEAT_MS 3000

class NikitaBridge {
public:
    void begin();   // call in setup()
    void loop();    // call in loop(); emits the GPIO heartbeat when idle
private:
    uint32_t _lastBeat = 0;
    bool _started = false;
};

extern NikitaBridge nikitaBridge;
