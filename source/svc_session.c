#include "svc_session.h"

static u32 g_mqtt_open_mask = 0;

static Result svc_initialize(SvcId id) {
    switch (id) {
        case SvcId_Lbl:
            return lblInitialize();
        case SvcId_Spsm:
            return spsmInitialize();
        case SvcId_Setsys:
            return setsysInitialize();
        case SvcId_Spl:
            return splInitialize();
        case SvcId_Psm:
            return psmInitialize();
        case SvcId_Audctl:
            return audctlInitialize();
        case SvcId_Pminfo:
            return pminfoInitialize();
        case SvcId_Hid:
            return hidInitialize();
        case SvcId_Nifm:
            return nifmInitialize(NifmServiceType_System);
        case SvcId_Ts:
            return tsInitialize();
        case SvcId_Apm:
            return apmInitialize();
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
        case SvcId_Setsys:
            setsysExit();
            break;
        case SvcId_Spl:
            splExit();
            break;
        case SvcId_Psm:
            psmExit();
            break;
        case SvcId_Audctl:
            audctlExit();
            break;
        case SvcId_Pminfo:
            pminfoExit();
            break;
        case SvcId_Hid:
            hidExit();
            break;
        case SvcId_Nifm:
            nifmExit();
            break;
        case SvcId_Ts:
            tsExit();
            break;
        case SvcId_Apm:
            apmExit();
            break;
        default:
            break;
    }
}

static void svc_mqtt_open_one(SvcId id, bool available) {
    if (available && R_SUCCEEDED(svc_open(id))) {
        g_mqtt_open_mask |= 1u << id;
    }
}

void svc_mqtt_open_all(AppState *state) {
    svc_mqtt_close_all();
    svc_mqtt_open_one(SvcId_Psm, state->svc_psm_ready);
    svc_mqtt_open_one(SvcId_Audctl, state->svc_audctl_ready);
    svc_mqtt_open_one(SvcId_Pminfo, state->svc_pminfo_ready);
    svc_mqtt_open_one(SvcId_Hid, state->svc_hid_ready);
    svc_mqtt_open_one(SvcId_Nifm, true);
    if (svc_mqtt_is_open(SvcId_Nifm) && !state->svc_nifm_ready) {
        mutexLock(&state->lock);
        state->svc_nifm_ready = true;
        mutexUnlock(&state->lock);
    }
    // apm always exists; its performance mode is how a sysmodule tells docked
    // from handheld (appletGetOperationMode only works in applets).
    svc_mqtt_open_one(SvcId_Apm, true);
    // tsGetTemperature returns IncompatSysVer on 17.0.0+, so ts would only cost IPC there.
    svc_mqtt_open_one(SvcId_Ts, state->svc_ts_ready && hosversionBefore(17, 0, 0));
}

void svc_mqtt_close_all(void) {
    for (int id = 0; id < SvcId_Count; ++id) {
        if (g_mqtt_open_mask & (1u << id)) {
            svc_close((SvcId) id);
        }
    }
    g_mqtt_open_mask = 0;
}

bool svc_mqtt_is_open(SvcId id) {
    return (g_mqtt_open_mask & (1u << id)) != 0;
}
