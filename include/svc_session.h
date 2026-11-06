#pragma once

#include "app_state.h"

#include <switch.h>

// Services the sysmodule must not keep open for its whole lifetime.
//
// Several system processes (fatal, am, ...) open their own sessions to these
// services when the console reboots or shows an error screen, and abort with
// OutOfSessions (0x615) if none is left. So no session stays open longer than
// needed. Each service is probed (opened and closed) once at boot; the
// svc_*_ready flags only say the service exists, they never mean a session is
// open.
//
// Two lifetimes:
// - per use (svc_open/svc_close around the calls): lbl, spsm, setsys, spl.
//   lbl is read every second but fatal needs its own lbl session at any time.
// - per MQTT connection (svc_mqtt_*): psm, audctl, pminfo, hid, nifm, ts.
//   They are read every second by the sensor loop; they are opened when MQTT
//   connects and closed when the connection ends, which includes sleep and
//   shutdown (the MQTT loop disconnects before psc is acknowledged).
//
// Never call a service function unless its session is known to be open: hid
// reads abort the process when hid is closed, and domain services such as nifm
// hit undefined behavior.
typedef enum {
    SvcId_Lbl,
    SvcId_Spsm,
    SvcId_Setsys,
    SvcId_Spl,
    SvcId_Psm,
    SvcId_Audctl,
    SvcId_Pminfo,
    SvcId_Hid,
    SvcId_Nifm,
    SvcId_Ts,
    SvcId_Count,
} SvcId;

// Opens a session for one use. Every successful svc_open must be paired with
// svc_close. libnx refcounts each service, so nested or concurrent opens of
// the same service from several threads are safe.
Result svc_open(SvcId id);
void svc_close(SvcId id);

// Per MQTT connection. Called from the MQTT thread only. Only services whose
// boot probe succeeded are opened, so a service that is not registered cannot
// block the MQTT thread.
void svc_mqtt_open_all(const AppState *state);
void svc_mqtt_close_all(void);
bool svc_mqtt_is_open(SvcId id);
