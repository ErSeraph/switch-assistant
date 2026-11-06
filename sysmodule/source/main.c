#include "app_state.h"
#include "config.h"
#include "logger.h"
#include "mqtt_client.h"
#include "screen_stream.h"
#include "svc_session.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INNER_HEAP_SIZE 0x100000
#define HEARTBEAT_PATH "sdmc:/switch/switch-ha/sysmodule-heartbeat.txt"
#define LOG_PATH "sdmc:/switch/switch-ha/sysmodule.log"
#define OPTIONAL_INIT_TIMEOUT_NS (1500ULL * 1000ULL * 1000ULL)
#define RESULT_OPTIONAL_TIMEOUT MAKERESULT(Module_Libnx, LibnxError_Timeout)
#define RESULT_NOT_INITIALIZED MAKERESULT(Module_Libnx, LibnxError_NotInitialized)
// psc only dispatches to ids in the system range (up to 127, Spsm): with
// 0x5348 the module registered fine but never received a single request.
// 120 is unused by the system (sys-con uses 126) and was verified on hardware.
#define SHA_PSC_MODULE_ID ((PscPmModuleId) 120)
#define SLEEP_SOCKETS_TIMEOUT_MS 3000
#define SLEEP_SOCKETS_FORCE_MS 1500

static Result g_socket_init_result = RESULT_NOT_INITIALIZED;
static Result g_psm_init_result = RESULT_NOT_INITIALIZED;
static Result g_spsm_init_result = RESULT_NOT_INITIALIZED;
static Result g_spl_init_result = RESULT_NOT_INITIALIZED;
static Result g_hid_init_result = RESULT_NOT_INITIALIZED;
static Result g_lbl_init_result = RESULT_NOT_INITIALIZED;
static Result g_audctl_init_result = RESULT_NOT_INITIALIZED;
static Result g_pminfo_init_result = RESULT_NOT_INITIALIZED;
static Result g_setsys_init_result = RESULT_NOT_INITIALIZED;
static Result g_ts_init_result = RESULT_NOT_INITIALIZED;
static Result g_nifm_init_result = RESULT_NOT_INITIALIZED;
static Result g_psc_init_result = RESULT_NOT_INITIALIZED;
static PscPmModule g_psc_module;
static Thread g_power_guard_thread = {0};
static bool g_power_guard_thread_active = false;

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));
    }

    // A custom __appInit skips libnx's default one, which is what sets the
    // firmware version. Without it hosversionGet() returns 0 and libnx takes
    // its old-firmware code paths: psc acknowledges with the pre-5.1.0
    // command, psm reads the pre-17.0.0 battery struct layout, nifm uses the
    // pre-3.0.0 general service, ...
    if (hosversionGet() == 0) {
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            }
            setsysExit();
        }
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    }
    fsdevMountSdmc();
}

void __appExit(void) {
    if (g_power_guard_thread_active) {
        threadClose(&g_power_guard_thread);
        g_power_guard_thread_active = false;
    }
    if (R_SUCCEEDED(g_psc_init_result)) {
        pscPmModuleFinalize(&g_psc_module);
        pscPmModuleClose(&g_psc_module);
        pscmExit();
    }
    screen_stream_stop();
    if (R_SUCCEEDED(g_socket_init_result)) socketExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

static u64 monotonic_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static void init_core_services(AppState *state) {
    LOG_I("sys", "Core service init started");

    SocketInitConfig socket_config = {
        .tcp_tx_buf_size = 0x8000,
        .tcp_rx_buf_size = 0x8000,
        .tcp_tx_buf_max_size = 0,
        .tcp_rx_buf_max_size = 0,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 4,
        .num_bsd_sessions = 3,
        .bsd_service_type = BsdServiceType_System,
    };
    g_socket_init_result = socketInitialize(&socket_config);
    LOG_I("sys", "socketInitialize rc=0x%x", g_socket_init_result);

    // psm is opened again for each MQTT connection (svc_session).
    g_psm_init_result = psmInitialize();
    if (R_SUCCEEDED(g_psm_init_result)) psmExit();
    state->svc_psm_ready = R_SUCCEEDED(g_psm_init_result);
    LOG_I("sys", "psm probe rc=0x%x", g_psm_init_result);

    // Only probe spsm: keeping a session open exhausts its session limit and
    // makes fatal/am abort on reboot (OutOfSessions, 0x615). It is opened
    // again for each reboot/shutdown/sleep request (svc_session).
    g_spsm_init_result = spsmInitialize();
    if (R_SUCCEEDED(g_spsm_init_result)) spsmExit();
    state->svc_spsm_ready = R_SUCCEEDED(g_spsm_init_result);
    LOG_I("sys", "spsm probe rc=0x%x", g_spsm_init_result);

    // spl is opened again only to read the Atmosphere version (svc_session).
    g_spl_init_result = splInitialize();
    if (R_SUCCEEDED(g_spl_init_result)) splExit();
    state->svc_spl_ready = R_SUCCEEDED(g_spl_init_result);
    LOG_I("sys", "spl probe rc=0x%x", g_spl_init_result);

    LOG_I("sys", "Core service init completed");
}

static void set_power_sleeping(AppState *state, bool sleeping) {
    mutexLock(&state->lock);
    state->power_sleeping = sleeping;
    state->sensor_publish_requested = true;
    mutexUnlock(&state->lock);
    screen_stream_set_paused(sleeping);
}

// Live sockets and in-flight poll() calls make the power transition fail
// (omm then aborts with a psc error), so both MQTT and RTSP must be closed
// before the request is acknowledged. The MQTT thread sees power_sleeping,
// publishes the "off" states, disconnects cleanly and closes its socket.
static bool sockets_closed_now(AppState *state) {
    mutexLock(&state->lock);
    bool connected = state->mqtt_connected;
    mutexUnlock(&state->lock);
    return !connected && mqtt_service_socket_idle() && screen_stream_sockets_closed();
}

// Returns a short description of what was still open, or NULL when all closed.
static const char *wait_for_sockets_closed(AppState *state) {
    u64 start = monotonic_ms();
    bool forced = false;
    while (monotonic_ms() - start < SLEEP_SOCKETS_TIMEOUT_MS) {
        if (sockets_closed_now(state)) {
            return NULL;
        }
        if (!forced && monotonic_ms() - start >= SLEEP_SOCKETS_FORCE_MS) {
            // A send()/poll() blocked on a network that is going down would
            // keep its socket open past psc's deadline: force them to fail.
            mqtt_service_abort_socket();
            screen_stream_close_sockets();
            forced = true;
        }
        svcSleepThread(50ULL * 1000ULL * 1000ULL);
    }
    if (!mqtt_service_socket_idle()) return "mqtt";
    if (!screen_stream_sockets_closed()) return "rtsp";
    return "mqtt state";
}

static bool is_going_down(PscPmState pm_state, PscPmState previous) {
    switch (pm_state) {
        case PscPmState_ReadyAwaken:         // MinimumAwake
            // First step of sleep when coming from FullAwake; on wake-up it is
            // reached from EssentialServicesAwake instead.
            return previous == PscPmState_Awake;
        case PscPmState_ReadySleep:          // SleepReady
        case PscPmState_ReadySleepCritical:  // EssentialServicesSleepReady
        case PscPmState_ReadyShutdown:       // ShutdownReady
            return true;
        default:
            return false;
    }
}

// psc notifies registered modules before the console sleeps or shuts down and
// after it wakes, in handheld and docked mode alike. The handheld backlight
// cannot be used for this: it is also off while docked, which used to pause
// the RTSP stream whenever the console was in the dock.
// libnx names the states differently; the official names are used in the
// comments: 0 FullAwake, 1 MinimumAwake, 2 SleepReady,
// 3 EssentialServicesSleepReady, 4 EssentialServicesAwake, 5 ShutdownReady.
// Sleep goes 0 -> 1 -> 2 -> 3, wake-up 3 -> 4 -> 1 -> 0.
static void power_guard_thread_main(void *arg) {
    AppState *state = arg;
    PscPmState previous = PscPmState_Awake;

    while (true) {
        if (R_FAILED(eventWait(&g_psc_module.event, UINT64_MAX))) {
            continue;
        }

        PscPmState pm_state;
        u32 flags = 0;
        if (R_FAILED(pscPmModuleGetRequest(&g_psc_module, &pm_state, &flags))) {
            continue;
        }

        // psc waits for every module with a deadline, so nothing slow may run
        // before the acknowledgement. LOG_* writes to the SD card through the
        // single fs session shared by all threads, so it is done afterwards.
        // Sockets are closed at the very first step of sleep (MinimumAwake):
        // once that step is acknowledged the network is already being
        // suspended and sends can block.
        const char *event = NULL;
        const char *still_open = NULL;
        u64 wait_ms = 0;
        if (is_going_down(pm_state, previous)) {
            if (!state->power_sleeping) {
                u64 start = monotonic_ms();
                set_power_sleeping(state, true);
                still_open = wait_for_sockets_closed(state);
                wait_ms = monotonic_ms() - start;
                event = pm_state == PscPmState_ReadyShutdown ? "shutdown" : "sleep";
            }
        } else if (pm_state == PscPmState_Awake) {
            if (state->power_sleeping) {
                set_power_sleeping(state, false);
                event = "awake";
            }
        }
        previous = pm_state;

        Result ack_rc = pscPmModuleAcknowledge(&g_psc_module, pm_state);

        LOG_I("power", "psc state=%d flags=0x%x ack=0x%x", (int) pm_state, flags, ack_rc);
        if (event) {
            LOG_I("power", "power: %s (sockets closed in %llu ms)", event, (unsigned long long) wait_ms);
        }
        if (still_open) {
            LOG_W("power", "power: sockets close timed out (%s still open)", still_open);
        }
    }
}

static void init_power_guard(AppState *state) {
    // Modules we depend on transition after us: Fs keeps the SD card usable and
    // WlanSockets/Nifm keep the network up until MQTT has sent its DISCONNECT.
    static const u32 dependencies[] = {PscPmModuleId_Fs, PscPmModuleId_WlanSockets, PscPmModuleId_Nifm};

    Result rc = smInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = pscmInitialize();
        if (R_SUCCEEDED(rc)) {
            rc = pscmGetPmModule(&g_psc_module, SHA_PSC_MODULE_ID, dependencies, sizeof(dependencies) / sizeof(dependencies[0]), true);
            if (R_FAILED(rc)) {
                pscmExit();
            }
        }
        smExit();
    }
    g_psc_init_result = rc;
    LOG_I("sys", "psc_init rc=0x%x", rc);
    if (R_FAILED(rc)) {
        return;
    }

    rc = threadCreate(&g_power_guard_thread, power_guard_thread_main, state, NULL, 0x4000, 0x2B, -2);
    if (R_FAILED(rc)) {
        LOG_E("sys", "threadCreate power_guard failed rc=0x%x", rc);
        return;
    }
    g_power_guard_thread_active = true;
    rc = threadStart(&g_power_guard_thread);
    if (R_FAILED(rc)) {
        LOG_E("sys", "threadStart power_guard failed rc=0x%x", rc);
        threadClose(&g_power_guard_thread);
        g_power_guard_thread_active = false;
    }
}

static void apply_startup_delay(const AppConfig *config) {
    if (config->startup_delay_seconds <= 0) {
        return;
    }

    LOG_I("sys", "Startup delay %d seconds starting", config->startup_delay_seconds);
    svcSleepThread((u64) config->startup_delay_seconds * 1000ULL * 1000ULL * 1000ULL);
    LOG_I("sys", "Startup delay completed");
}

typedef enum {
    OptionalService_Hid,
    OptionalService_Lbl,
    OptionalService_Audctl,
    OptionalService_Pminfo,
    OptionalService_Setsys,
    OptionalService_Spl,
    OptionalService_Ts,
    OptionalService_Nifm,
} OptionalService;

typedef struct {
    OptionalService service;
    volatile bool done;
    Result rc;
} OptionalInitTask;

// Every optional service is only probed: the session is closed in this thread,
// so it is released even if the probe finishes after the timeout. Sessions are
// opened again where they are used (see svc_session.h for the lifetimes).
static void optional_init_thread(void *arg) {
    OptionalInitTask *task = arg;
    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    switch (task->service) {
        case OptionalService_Hid:
            rc = hidInitialize();
            if (R_SUCCEEDED(rc)) hidExit();
            break;
        case OptionalService_Lbl:
            // fatal opens its own lbl session to show an error screen and
            // aborts if none is left.
            rc = lblInitialize();
            if (R_SUCCEEDED(rc)) lblExit();
            break;
        case OptionalService_Audctl:
            rc = audctlInitialize();
            if (R_SUCCEEDED(rc)) audctlExit();
            break;
        case OptionalService_Pminfo:
            rc = pminfoInitialize();
            if (R_SUCCEEDED(rc)) pminfoExit();
            break;
        case OptionalService_Setsys:
            rc = setsysInitialize();
            if (R_SUCCEEDED(rc)) setsysExit();
            break;
        case OptionalService_Spl:
            rc = splInitialize();
            if (R_SUCCEEDED(rc)) splExit();
            break;
        case OptionalService_Ts:
            rc = tsInitialize();
            if (R_SUCCEEDED(rc)) tsExit();
            break;
        case OptionalService_Nifm:
            rc = nifmInitialize(NifmServiceType_System);
            if (R_SUCCEEDED(rc)) nifmExit();
            break;
    }
    task->rc = rc;
    task->done = true;
}

static Result init_service_with_timeout(const char *label, OptionalService service) {
    LOG_D("sys", "Starting service init: %s", label);

    OptionalInitTask *task = calloc(1, sizeof(*task));
    if (!task) {
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    task->service = service;
    task->rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    Thread *thread = calloc(1, sizeof(*thread));
    if (!thread) {
        free(task);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }

    Result rc = threadCreate(thread, optional_init_thread, task, NULL, 0x4000, 0x2B, -2);
    if (R_FAILED(rc)) {
        free(thread);
        free(task);
        return rc;
    }

    rc = threadStart(thread);
    if (R_FAILED(rc)) {
        threadClose(thread);
        free(thread);
        free(task);
        return rc;
    }

    rc = waitSingle(waiterForThread(thread), OPTIONAL_INIT_TIMEOUT_NS);
    if (R_SUCCEEDED(rc) && task->done) {
        Result init_rc = task->rc;
        threadWaitForExit(thread);
        threadClose(thread);
        free(thread);
        free(task);
        return init_rc;
    }

    LOG_W("sys", "Service %s init timed out", label);
    return RESULT_OPTIONAL_TIMEOUT;
}

static void init_optional_services(AppState *state) {
    LOG_I("sys", "Optional service init start");

    g_hid_init_result = init_service_with_timeout("hid", OptionalService_Hid);
    state->svc_hid_ready = R_SUCCEEDED(g_hid_init_result);
    LOG_I("sys", "hid_init rc=0x%x", g_hid_init_result);

    g_lbl_init_result = init_service_with_timeout("lbl", OptionalService_Lbl);
    state->svc_lbl_ready = R_SUCCEEDED(g_lbl_init_result);
    LOG_I("sys", "lbl_init rc=0x%x", g_lbl_init_result);

    g_audctl_init_result = init_service_with_timeout("audctl", OptionalService_Audctl);
    state->svc_audctl_ready = R_SUCCEEDED(g_audctl_init_result);
    LOG_I("sys", "audctl_init rc=0x%x", g_audctl_init_result);

    g_pminfo_init_result = init_service_with_timeout("pminfo", OptionalService_Pminfo);
    state->svc_pminfo_ready = R_SUCCEEDED(g_pminfo_init_result);
    LOG_I("sys", "pminfo_init rc=0x%x", g_pminfo_init_result);

    g_setsys_init_result = init_service_with_timeout("setsys", OptionalService_Setsys);
    state->svc_setsys_ready = R_SUCCEEDED(g_setsys_init_result);
    LOG_I("sys", "setsys_init rc=0x%x", g_setsys_init_result);

    g_ts_init_result = init_service_with_timeout("ts", OptionalService_Ts);
    state->svc_ts_ready = R_SUCCEEDED(g_ts_init_result);
    LOG_I("sys", "ts_init rc=0x%x", g_ts_init_result);

    g_nifm_init_result = init_service_with_timeout("nifm", OptionalService_Nifm);
    state->svc_nifm_ready = R_SUCCEEDED(g_nifm_init_result);
    LOG_I("sys", "nifm_init rc=0x%x", g_nifm_init_result);

    LOG_I("sys", "Optional service init completed");
}

static void write_heartbeat(AppState *state) {
    FILE *file = fopen(HEARTBEAT_PATH, "w");
    if (!file) {
        return;
    }

    mutexLock(&state->lock);
    fprintf(file, "build=%s\n", SHA_APP_BUILD);
    fprintf(file, "uptime_ms=%lu\n", monotonic_ms());
    fprintf(file, "config=%s\n", state->config_status);
    fprintf(file, "mqtt_connected=%d\n", state->mqtt_connected ? 1 : 0);
    fprintf(file, "mqtt_dns_ok=%d\n", state->mqtt_dns_ok ? 1 : 0);
    fprintf(file, "mqtt_tcp_ok=%d\n", state->mqtt_tcp_ok ? 1 : 0);
    fprintf(file, "mqtt_detail=%s\n", state->mqtt_last_error);
    fprintf(file, "power_sleeping=%d\n", state->power_sleeping ? 1 : 0);
    fprintf(file, "game_status=%s\n", state->game_status);
    fprintf(file, "client_id=%s\n", state->config.mqtt_client_id);
    fprintf(file, "socket_init=0x%x\n", g_socket_init_result);
    fprintf(file, "socket_last=0x%x\n", socketGetLastResult());
    fprintf(file, "psm_init=0x%x\n", g_psm_init_result);
    fprintf(file, "spsm_init=0x%x\n", g_spsm_init_result);
    fprintf(file, "grcd_open=0x%x\n", screen_stream_get_grcd_open_result());
    fprintf(file, "grcd_begin=0x%x\n", screen_stream_get_grcd_begin_result());
    {
        char stream_status[32];
        screen_stream_get_status(stream_status, sizeof(stream_status));
        fprintf(file, "screen_stream=%s\n", stream_status);
        fprintf(file, "screen_rtsp_port=%u\n", screen_stream_port());
    }
    fprintf(file, "hid_init=0x%x\n", g_hid_init_result);
    fprintf(file, "lbl_init=0x%x\n", g_lbl_init_result);
    fprintf(file, "audctl_init=0x%x\n", g_audctl_init_result);
    fprintf(file, "pminfo_init=0x%x\n", g_pminfo_init_result);
    fprintf(file, "setsys_init=0x%x\n", g_setsys_init_result);
    fprintf(file, "ts_init=0x%x\n", g_ts_init_result);
    fprintf(file, "nifm_init=0x%x\n", g_nifm_init_result);
    fprintf(file, "psc_init=0x%x\n", g_psc_init_result);
    fprintf(file, "hos_version=%u.%u.%u\n", HOSVER_MAJOR(hosversionGet()), HOSVER_MINOR(hosversionGet()), HOSVER_MICRO(hosversionGet()));
    mutexUnlock(&state->lock);

    fclose(file);
}

int main(int argc, char **argv) {
    AppState state;
    app_state_init(&state);
    state.app_mode = AppMode_Sysmodule;
    logger_init(&state, LOG_PATH);
    LOG_I("sys", "Switch Assistant Sysmodule Boot (Version %s)", SHA_APP_BUILD);

    if (!config_load(&state.config)) {
        snprintf(state.config_status, sizeof(state.config_status), "no config");
        app_state_push_log(&state, "No config, waiting");
        LOG_W("config", "Config file missing or invalid");
    } else {
        snprintf(state.config_status, sizeof(state.config_status), "loaded");
        app_state_push_log(&state, "Config loaded");
        LOG_I("config", "Config loaded successfully");
    }

    apply_startup_delay(&state.config);
    init_core_services(&state);
    init_optional_services(&state);
    app_state_refresh_device_info(&state);
    screen_stream_start(&state);
    // Registered after the system services are up (the configuration verified
    // on hardware); the modules in its dependency list must exist already.
    init_power_guard(&state);

    mqtt_service_start(&state);
    LOG_I("mqtt", "MQTT worker thread launched");

    while (true) {
        AppConfig loaded;
        if (config_load(&loaded)) {
            mutexLock(&state.lock);
            state.config = loaded;
            snprintf(state.config_status, sizeof(state.config_status), "loaded");
            mutexUnlock(&state.lock);
        }

        mutexLock(&state.lock);
        bool power_sleeping = state.power_sleeping;
        mutexUnlock(&state.lock);

        if (!power_sleeping) {
            write_heartbeat(&state);
            svcSleepThread(30ULL * 1000ULL * 1000ULL * 1000ULL);
        } else {
            svcSleepThread(1000ULL * 1000ULL * 1000ULL);
        }
    }

    return 0;
}
