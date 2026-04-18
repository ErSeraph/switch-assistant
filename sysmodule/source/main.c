#include "app_state.h"
#include "config.h"
#include "mqtt_client.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INNER_HEAP_SIZE 0x100000
#define HEARTBEAT_PATH "sdmc:/switch/switch-ha/sysmodule-heartbeat.txt"
#define LOG_PATH "sdmc:/switch/switch-ha/sysmodule.log"
#define OPTIONAL_INIT_TIMEOUT_NS (1500ULL * 1000ULL * 1000ULL)
#define RESULT_OPTIONAL_TIMEOUT MAKERESULT(Module_Libnx, LibnxError_Timeout)

static Result g_socket_init_result = 0;
static Result g_psm_init_result = 0;
static Result g_spsm_init_result = 0;
static Result g_hid_init_result = 0;
static Result g_lbl_init_result = 0;
static Result g_audctl_init_result = 0;
static Result g_pminfo_init_result = 0;

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

    rc = fsInitialize();
    if (R_FAILED(rc)) {
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    }
    fsdevMountSdmc();

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
    g_psm_init_result = psmInitialize();
    g_spsm_init_result = spsmInitialize();

    smExit();
}

void __appExit(void) {
    if (R_SUCCEEDED(g_pminfo_init_result)) pminfoExit();
    if (R_SUCCEEDED(g_audctl_init_result)) audctlExit();
    if (R_SUCCEEDED(g_lbl_init_result)) lblExit();
    if (R_SUCCEEDED(g_hid_init_result)) hidExit();
    if (R_SUCCEEDED(g_spsm_init_result)) spsmExit();
    psmExit();
    socketExit();
    fsdevUnmountAll();
    fsExit();
}

static u64 monotonic_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static void append_log(const char *message) {
    FILE *file = fopen(LOG_PATH, "a");
    if (!file) {
        return;
    }
    fprintf(file, "%lu %s\n", monotonic_ms(), message);
    fclose(file);
}

static void append_log_result(const char *name, Result rc) {
    char line[96];
    snprintf(line, sizeof(line), "%s=0x%x", name, rc);
    append_log(line);
}

typedef enum {
    OptionalService_Hid,
    OptionalService_Lbl,
    OptionalService_Audctl,
    OptionalService_Pminfo,
} OptionalService;

typedef struct {
    OptionalService service;
    volatile bool done;
    Result rc;
} OptionalInitTask;

static void optional_init_thread(void *arg) {
    OptionalInitTask *task = arg;
    Result rc = smInitialize();
    if (R_SUCCEEDED(rc)) {
        switch (task->service) {
            case OptionalService_Hid:
                rc = hidInitialize();
                break;
            case OptionalService_Lbl:
                rc = lblInitialize();
                break;
            case OptionalService_Audctl:
                rc = audctlInitialize();
                break;
            case OptionalService_Pminfo:
                rc = pminfoInitialize();
                break;
        }
        smExit();
    }
    task->rc = rc;
    task->done = true;
}

static Result init_service_with_timeout(const char *label, OptionalService service) {
    char line[96];
    snprintf(line, sizeof(line), "starting %s", label);
    append_log(line);

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

    snprintf(line, sizeof(line), "%s init timed out", label);
    append_log(line);
    return RESULT_OPTIONAL_TIMEOUT;
}

static void init_optional_services(AppState *state) {
    append_log("optional service init start");

    g_hid_init_result = init_service_with_timeout("hid", OptionalService_Hid);
    state->svc_hid_ready = R_SUCCEEDED(g_hid_init_result);
    append_log_result("hid_init", g_hid_init_result);

    g_lbl_init_result = init_service_with_timeout("lbl", OptionalService_Lbl);
    state->svc_lbl_ready = R_SUCCEEDED(g_lbl_init_result);
    append_log_result("lbl_init", g_lbl_init_result);

    g_audctl_init_result = init_service_with_timeout("audctl", OptionalService_Audctl);
    state->svc_audctl_ready = R_SUCCEEDED(g_audctl_init_result);
    append_log_result("audctl_init", g_audctl_init_result);

    g_pminfo_init_result = init_service_with_timeout("pminfo", OptionalService_Pminfo);
    state->svc_pminfo_ready = R_SUCCEEDED(g_pminfo_init_result);
    append_log_result("pminfo_init", g_pminfo_init_result);

    append_log("optional service init done");
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
    fprintf(file, "game_status=%s\n", state->game_status);
    fprintf(file, "client_id=%s\n", state->config.mqtt_client_id);
    fprintf(file, "socket_init=0x%x\n", g_socket_init_result);
    fprintf(file, "socket_last=0x%x\n", socketGetLastResult());
    fprintf(file, "psm_init=0x%x\n", g_psm_init_result);
    fprintf(file, "spsm_init=0x%x\n", g_spsm_init_result);
    fprintf(file, "hid_init=0x%x\n", g_hid_init_result);
    fprintf(file, "lbl_init=0x%x\n", g_lbl_init_result);
    fprintf(file, "audctl_init=0x%x\n", g_audctl_init_result);
    fprintf(file, "pminfo_init=0x%x\n", g_pminfo_init_result);
    mutexUnlock(&state->lock);

    fclose(file);
}

int main(int argc, char **argv) {
    AppState state;
    app_state_init(&state);
    state.app_mode = AppMode_Sysmodule;
    state.svc_spsm_ready = R_SUCCEEDED(g_spsm_init_result);
    append_log("sysmodule boot");
    init_optional_services(&state);

    if (!config_load(&state.config)) {
        snprintf(state.config_status, sizeof(state.config_status), "no config");
        app_state_push_log(&state, "No config, waiting");
        append_log("config missing");
    } else {
        snprintf(state.config_status, sizeof(state.config_status), "loaded");
        app_state_push_log(&state, "Config loaded");
        append_log("config loaded");
    }

    mqtt_service_start(&state);
    append_log("mqtt thread started");

    while (true) {
        AppConfig loaded;
        if (config_load(&loaded)) {
            mutexLock(&state.lock);
            state.config = loaded;
            snprintf(state.config_status, sizeof(state.config_status), "loaded");
            mutexUnlock(&state.lock);
        }

        write_heartbeat(&state);
        svcSleepThread(30ULL * 1000ULL * 1000ULL * 1000ULL);
    }

    return 0;
}
