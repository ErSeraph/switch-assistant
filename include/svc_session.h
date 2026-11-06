#pragma once

#include <switch.h>

// Services the sysmodule must not keep open for its whole lifetime.
//
// Several system processes (fatal, am, ...) open their own sessions to these
// services when the console reboots or shows an error screen, and abort with
// OutOfSessions (0x615) if none is left. So sessions are only opened around
// the code that needs them.
typedef enum {
    SvcId_Lbl,
    SvcId_Spsm,
    SvcId_Count,
} SvcId;

// Opens a session for one use. Every successful svc_open must be paired with
// svc_close. libnx refcounts each service, so nested or concurrent opens of
// the same service from several threads are safe.
Result svc_open(SvcId id);
void svc_close(SvcId id);
