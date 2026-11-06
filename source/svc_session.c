#include "svc_session.h"

static Result svc_initialize(SvcId id) {
    switch (id) {
        case SvcId_Lbl:
            return lblInitialize();
        case SvcId_Spsm:
            return spsmInitialize();
        default:
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
}

Result svc_open(SvcId id) {
    // sm is refcounted too: this works whether or not the caller keeps a
    // global sm session (the sysmodule does, the NRO app gets one from libnx).
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = svc_initialize(id);
    smExit();
    return rc;
}

void svc_close(SvcId id) {
    switch (id) {
        case SvcId_Lbl:
            lblExit();
            break;
        case SvcId_Spsm:
            spsmExit();
            break;
        default:
            break;
    }
}
