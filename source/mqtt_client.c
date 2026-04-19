#include "mqtt_client.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#define MQTT_BUFFER 2048
#define MQTT_KEEPALIVE 30
#define SENSOR_POLL_INTERVAL_MS 1000
#define MQTT_HEARTBEAT_INTERVAL_MS 15000
#define SWITCH_HA_ENABLE_SYSTEM_SENSORS 1
#define MAX_PLAYERS 8
#define NOTIFY_TEXT_MAX 512
#define NOTIFICATION_CURRENT_PATH "sdmc:/switch/switch-ha/notification-current.ini"
#define NOTIFICATION_LOG_PATH "sdmc:/switch/switch-ha/notifications.log"
#define NOTIFICATION_LOG_MAX_BYTES 8192

typedef struct {
    bool initialized;
    char battery[16];
    char charging[8];
    char charger_type[32];
    char battery_voltage[16];
    char battery_temperature[16];
    char battery_health[16];
    char brightness[16];
    char backlight[16];
    char volume[16];
    char audio_target[24];
    char game_running[8];
    char current_game_id[24];
    char player_controller_count[16];
    char player_controller[MAX_PLAYERS][32];
} SensorSnapshot;

typedef struct {
    Thread thread;
    bool active;
    int socket_fd;
} MqttContext;

static MqttContext g_ctx = {0};

static void sanitize_id(const char *input, char *output, size_t output_size);
static const char *client_id_or_device(const AppConfig *config);

static u64 monotonic_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

static void mqtt_set_status(AppState *state, bool dns_ok, bool tcp_ok, bool session_ok, const char *fmt, ...) {
    char message[SHA_LOG_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    mutexLock(&state->lock);
    state->mqtt_dns_ok = dns_ok;
    state->mqtt_tcp_ok = tcp_ok;
    state->mqtt_connected = session_ok;
    strncpy(state->mqtt_last_error, message, sizeof(state->mqtt_last_error) - 1);
    state->mqtt_last_error[sizeof(state->mqtt_last_error) - 1] = '\0';
    mutexUnlock(&state->lock);
}

static bool is_application_program_id(u64 program_id) {
    return program_id >= 0x0100000000010000ULL;
}

static bool parse_port(const char *value, int *port) {
    if (!value || *value == '\0') {
        return false;
    }

    for (const char *ptr = value; *ptr; ++ptr) {
        if (*ptr < '0' || *ptr > '9') {
            return false;
        }
    }

    int parsed = atoi(value);
    if (parsed <= 0 || parsed > 65535) {
        return false;
    }

    *port = parsed;
    return true;
}

static void normalize_endpoint(const AppConfig *config, char *host, size_t host_size, int *port) {
    const char *start = config->mqtt_host;
    const char *scheme = strstr(start, "://");
    if (scheme) {
        start = scheme + 3;
    }

    snprintf(host, host_size, "%s", start);

    char *path = strchr(host, '/');
    if (path) {
        *path = '\0';
    }

    char *query = strchr(host, '?');
    if (query) {
        *query = '\0';
    }

    char *colon = strrchr(host, ':');
    if (colon && strchr(colon + 1, ':') == NULL) {
        int parsed_port = 0;
        if (parse_port(colon + 1, &parsed_port)) {
            *colon = '\0';
            *port = parsed_port;
        }
    }

    while (host[0] == ' ') {
        memmove(host, host + 1, strlen(host));
    }

    size_t len = strlen(host);
    while (len > 0 && host[len - 1] == ' ') {
        host[--len] = '\0';
    }
}

static int encode_remaining_length(unsigned char *dst, int len) {
    int i = 0;
    do {
        unsigned char byte = len % 128;
        len /= 128;
        if (len > 0) {
            byte |= 0x80;
        }
        dst[i++] = byte;
    } while (len > 0 && i < 4);
    return i;
}

static unsigned char *write_string(unsigned char *dst, const char *value) {
    const size_t len = strlen(value);
    *dst++ = (unsigned char) ((len >> 8) & 0xFF);
    *dst++ = (unsigned char) (len & 0xFF);
    memcpy(dst, value, len);
    return dst + len;
}

static const char *connack_reason(int return_code) {
    switch (return_code) {
        case 0:
            return "accepted";
        case 1:
            return "bad protocol";
        case 2:
            return "client id rejected";
        case 3:
            return "server unavailable";
        case 4:
            return "bad user/password";
        case 5:
            return "not authorized";
        default:
            return "unknown";
    }
}

static const char *tcp_errno_reason(int code) {
    switch (code) {
        case ECONNREFUSED:
            return "broker/port closed";
        case ETIMEDOUT:
            return "timeout";
        case ENETUNREACH:
            return "network unreachable";
        case EHOSTUNREACH:
            return "host unreachable";
        default:
            return "tcp error";
    }
}

static bool socket_connect_ipv4(AppState *state, const char *host, int port, int *out_fd) {
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16) port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return false;
    }

    mqtt_set_status(state, true, false, false, "IPv4 OK: %s:%d", host, port);

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        mqtt_set_status(state, true, false, false, "TCP socket failed errno=%d rc=0x%x", errno, socketGetLastResult());
        return true;
    }

    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0) {
        *out_fd = fd;
        mqtt_set_status(state, true, true, false, "TCP OK: %s:%d", host, port);
        return true;
    }

    int last_errno = errno;
    close(fd);
    mqtt_set_status(state, true, false, false, "TCP failed: %s:%d %s errno=%d", host, port, tcp_errno_reason(last_errno), last_errno);
    return true;
}

static bool socket_connect_to(AppState *state, const char *host, int port, int *out_fd) {
    if (socket_connect_ipv4(state, host, port, out_fd)) {
        return *out_fd >= 0;
    }

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &result);
    if (gai != 0) {
        mqtt_set_status(state, false, false, false, "DNS failed: %s:%d gai=%d", host, port, gai);
        return false;
    }

    mqtt_set_status(state, true, false, false, "DNS OK: %s:%d", host, port);

    int fd = -1;
    int last_errno = 0;
    for (struct addrinfo *addr = result; addr; addr = addr->ai_next) {
        fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) {
            last_errno = errno;
            continue;
        }
        if (connect(fd, addr->ai_addr, addr->ai_addrlen) == 0) {
            *out_fd = fd;
            freeaddrinfo(result);
            mqtt_set_status(state, true, true, false, "TCP OK: %s:%d", host, port);
            return true;
        }
        last_errno = errno;
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    mqtt_set_status(state, true, false, false, "TCP failed: %s:%d %s errno=%d", host, port, tcp_errno_reason(last_errno), last_errno);
    return false;
}

static bool mqtt_send_packet(int fd, const unsigned char *buffer, size_t size) {
    size_t sent_total = 0;
    while (sent_total < size) {
        ssize_t sent = send(fd, buffer + sent_total, size - sent_total, 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += (size_t) sent;
    }
    return true;
}

static bool socket_recv_all(int fd, unsigned char *buffer, size_t size) {
    size_t received_total = 0;
    while (received_total < size) {
        ssize_t received = recv(fd, buffer + received_total, size - received_total, 0);
        if (received <= 0) {
            return false;
        }
        received_total += (size_t) received;
    }
    return true;
}

static bool mqtt_send_connect(const AppConfig *config, int fd) {
    unsigned char packet[MQTT_BUFFER];
    unsigned char variable[MQTT_BUFFER];
    unsigned char *ptr = variable;

    ptr = write_string(ptr, "MQTT");
    *ptr++ = 0x04;
    unsigned char flags = 0x02;
    if (config->mqtt_username[0] != '\0') {
        flags |= 0x80;
    }
    if (config->mqtt_password[0] != '\0') {
        flags |= 0x40;
    }
    *ptr++ = flags;
    *ptr++ = 0x00;
    *ptr++ = MQTT_KEEPALIVE;
    ptr = write_string(ptr, config->mqtt_client_id[0] ? config->mqtt_client_id : config->device_name);
    if (config->mqtt_username[0] != '\0') {
        ptr = write_string(ptr, config->mqtt_username);
    }
    if (config->mqtt_password[0] != '\0') {
        ptr = write_string(ptr, config->mqtt_password);
    }

    size_t payload_len = (size_t) (ptr - variable);
    packet[0] = 0x10;
    int remain = encode_remaining_length(packet + 1, (int) payload_len);
    memcpy(packet + 1 + remain, variable, payload_len);
    return mqtt_send_packet(fd, packet, 1 + remain + payload_len);
}

static bool mqtt_send_publish(int fd, const char *topic, const char *payload, bool retain) {
    unsigned char packet[MQTT_BUFFER];
    unsigned char *ptr = packet;
    *ptr++ = retain ? 0x31 : 0x30;

    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t remain_len = 2 + topic_len + payload_len;
    if (1 + 4 + remain_len > sizeof(packet)) {
        return false;
    }
    ptr += encode_remaining_length(ptr, (int) remain_len);
    ptr = write_string(ptr, topic);
    memcpy(ptr, payload, payload_len);
    ptr += payload_len;
    return mqtt_send_packet(fd, packet, (size_t) (ptr - packet));
}

static bool mqtt_send_subscribe(int fd, const char *topic) {
    unsigned char packet[MQTT_BUFFER];
    unsigned char *ptr = packet;
    *ptr++ = 0x82;

    size_t topic_len = strlen(topic);
    size_t remain_len = 2 + 2 + topic_len + 1;
    ptr += encode_remaining_length(ptr, (int) remain_len);
    *ptr++ = 0x00;
    *ptr++ = 0x01;
    ptr = write_string(ptr, topic);
    *ptr++ = 0x00;
    return mqtt_send_packet(fd, packet, (size_t) (ptr - packet));
}

static bool mqtt_send_ping(int fd) {
    const unsigned char ping[2] = {0xC0, 0x00};
    return mqtt_send_packet(fd, ping, sizeof(ping));
}

static const char *charger_type_name(PsmChargerType type) {
    switch (type) {
        case PsmChargerType_Unconnected:
            return "unconnected";
        case PsmChargerType_EnoughPower:
            return "enough_power";
        case PsmChargerType_LowPower:
            return "low_power";
        case PsmChargerType_NotSupported:
            return "not_supported";
        default:
            return "unknown";
    }
}

static const char *audio_target_name(AudioTarget target) {
    switch (target) {
        case AudioTarget_Speaker:
            return "speaker";
        case AudioTarget_Headphone:
            return "headphone";
        case AudioTarget_Tv:
            return "tv";
        case AudioTarget_UsbOutputDevice:
            return "usb";
        case AudioTarget_Bluetooth:
            return "bluetooth";
        default:
            return "unknown";
    }
}

static const char *controller_style_name(u32 style) {
    if (style & HidNpadStyleTag_NpadHandheld) return "handheld";
    if (style & HidNpadStyleTag_NpadJoyDual) return "joydual";
    if (style & HidNpadStyleTag_NpadJoyLeft) return "joyleft";
    if (style & HidNpadStyleTag_NpadJoyRight) return "joyright";
    if (style & HidNpadStyleTag_NpadFullKey) return "pro";
    if (style & HidNpadStyleTag_NpadGc) return "gamecube";
    if (style & HidNpadStyleTag_NpadLucia) return "snes";
    if (style & HidNpadStyleTag_NpadLagon) return "n64";
    if (style & HidNpadStyleTag_NpadLager) return "genesis";
    if (style & HidNpadStyleTag_NpadSystemExt) return "system_ext";
    if (style & HidNpadStyleTag_NpadSystem) return "system";
    return "unknown";
}

static bool npad_has_state(HidNpadIdType id, u32 style, u32 *attributes) {
    HidNpadCommonState state;
    size_t count = 0;

    memset(&state, 0, sizeof(state));
    if (style & HidNpadStyleTag_NpadHandheld) {
        count = hidGetNpadStatesHandheld(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadJoyDual) {
        count = hidGetNpadStatesJoyDual(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadJoyLeft) {
        count = hidGetNpadStatesJoyLeft(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadJoyRight) {
        count = hidGetNpadStatesJoyRight(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadFullKey) {
        count = hidGetNpadStatesFullKey(id, &state, 1);
    } else if (style & HidNpadStyleTag_NpadGc) {
        HidNpadGcState gc_state;
        memset(&gc_state, 0, sizeof(gc_state));
        count = hidGetNpadStatesGc(id, &gc_state, 1);
        state.attributes = gc_state.attributes;
    }

    if (count == 0 || !(state.attributes & HidNpadAttribute_IsConnected)) {
        return false;
    }

    if (attributes) {
        *attributes = state.attributes;
    }
    return true;
}

static void controller_value(char *out, size_t out_size, u32 style) {
    snprintf(out, out_size, "%s", controller_style_name(style));
}

static void copy_text(char *out, size_t out_size, const char *text) {
    if (out_size == 0) {
        return;
    }
    size_t len = strnlen(text, out_size - 1);
    memcpy(out, text, len);
    out[len] = '\0';
}

static void trim_text(char *value) {
    while (value[0] == ' ' || value[0] == '\t' || value[0] == '\r' || value[0] == '\n') {
        memmove(value, value + 1, strlen(value));
    }

    size_t len = strlen(value);
    while (len > 0) {
        char ch = value[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        value[--len] = '\0';
    }
}

static int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool read_json_hex4(const char *pos, u32 *out) {
    u32 value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = hex_value(pos[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (u32) digit;
    }
    *out = value;
    return true;
}

static void append_utf8(char *out, size_t out_size, size_t *used, u32 cp) {
    if (*used + 1 >= out_size) {
        return;
    }

    if (cp <= 0x7F) {
        out[(*used)++] = (char) cp;
    } else if (cp <= 0x7FF && *used + 2 < out_size) {
        out[(*used)++] = (char) (0xC0 | ((cp >> 6) & 0x1F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF && *used + 3 < out_size) {
        out[(*used)++] = (char) (0xE0 | ((cp >> 12) & 0x0F));
        out[(*used)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    } else if (cp <= 0x10FFFF && *used + 4 < out_size) {
        out[(*used)++] = (char) (0xF0 | ((cp >> 18) & 0x07));
        out[(*used)++] = (char) (0x80 | ((cp >> 12) & 0x3F));
        out[(*used)++] = (char) (0x80 | ((cp >> 6) & 0x3F));
        out[(*used)++] = (char) (0x80 | (cp & 0x3F));
    }
}

static bool json_extract_string(const char *json, const char *key, char *out, size_t out_size) {
    char pattern[40];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) {
        return false;
    }
    pos += strlen(pattern);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != ':') return false;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != '"') return false;
    pos++;

    size_t used = 0;
    while (*pos && *pos != '"' && used + 1 < out_size) {
        if (*pos == '\\' && pos[1]) {
            pos++;
            switch (*pos) {
                case 'n': out[used++] = ' '; break;
                case 'r': out[used++] = ' '; break;
                case 't': out[used++] = ' '; break;
                case 'u': {
                    u32 cp = 0;
                    if (read_json_hex4(pos + 1, &cp)) {
                        pos += 5;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos[0] == '\\' && pos[1] == 'u') {
                            u32 low = 0;
                            if (read_json_hex4(pos + 2, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                pos += 6;
                            }
                        }
                        append_utf8(out, out_size, &used, cp);
                    } else {
                        out[used++] = 'u';
                        pos++;
                    }
                    break;
                }
                case '"':
                case '\\':
                case '/':
                    out[used++] = *pos;
                    pos++;
                    break;
                default:
                    out[used++] = *pos;
                    pos++;
                    break;
            }
        } else {
            out[used++] = *pos++;
        }
    }
    out[used] = '\0';
    return used > 0;
}

static void notification_topic(const AppConfig *config, const char *mode, char *topic, size_t topic_size) {
    char safe_id[80];
    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/notify/%s", safe_id, mode);
}

static void notification_status_topic(const AppConfig *config, char *topic, size_t topic_size) {
    char safe_id[80];
    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/notify/status", safe_id);
}

static void write_sanitized_value(FILE *file, const char *key, const char *value) {
    fprintf(file, "%s=", key);
    for (size_t i = 0; value[i]; ++i) {
        char ch = value[i];
        fputc((ch == '\n' || ch == '\r') ? ' ' : ch, file);
    }
    fputc('\n', file);
}

static void truncate_log_if_needed(const char *path, long max_bytes) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    fclose(file);

    if (size > max_bytes) {
        file = fopen(path, "w");
        if (file) {
            fclose(file);
        }
    }
}

static bool write_notification_files(const char *mode, const char *title, const char *message) {
    u64 id = monotonic_ms();

    FILE *current = fopen(NOTIFICATION_CURRENT_PATH, "w");
    if (!current) {
        return false;
    }
    fprintf(current, "id=%llu\n", (unsigned long long) id);
    write_sanitized_value(current, "mode", mode);
    write_sanitized_value(current, "title", title);
    write_sanitized_value(current, "message", message);
    fprintf(current, "duration_ms=4500\n");
    fclose(current);

    truncate_log_if_needed(NOTIFICATION_LOG_PATH, NOTIFICATION_LOG_MAX_BYTES);
    FILE *log = fopen(NOTIFICATION_LOG_PATH, "a");
    if (log) {
        fprintf(log, "%llu mode=%s title=%s message=%s\n", (unsigned long long) id, mode, title, message);
        fclose(log);
    }
    return true;
}

static void set_game_status(AppState *state, const char *fmt, ...) {
    char message[SHA_LOG_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    mutexLock(&state->lock);
    if (strncmp(state->game_status, message, sizeof(state->game_status)) != 0) {
        copy_text(state->game_status, sizeof(state->game_status), message);
    }
    mutexUnlock(&state->lock);
}

static bool get_program_id_quiet(u64 pid, u64 *program_id) {
    if (R_SUCCEEDED(pminfoGetProgramId(program_id, pid)) && *program_id != 0) {
        return true;
    }
    return false;
}

static bool scan_process_list_for_application(AppState *state, u64 *pid_out, u64 *program_id_out) {
    u64 process_ids[96];
    s32 process_count = 0;
    Result rc = svcGetProcessList(&process_count, process_ids, (u32) (sizeof(process_ids) / sizeof(process_ids[0])));
    if (R_FAILED(rc)) {
        set_game_status(state, "process list failed rc=0x%x", rc);
        return false;
    }

    u64 best_pid = 0;
    u64 best_program_id = 0;
    if (process_count > (s32) (sizeof(process_ids) / sizeof(process_ids[0]))) {
        process_count = (s32) (sizeof(process_ids) / sizeof(process_ids[0]));
    }

    for (s32 i = 0; i < process_count; ++i) {
        u64 program_id = 0;
        if (!get_program_id_quiet(process_ids[i], &program_id)) {
            continue;
        }
        if (!is_application_program_id(program_id)) {
            continue;
        }
        best_pid = process_ids[i];
        best_program_id = program_id;
        break;
    }

    if (best_program_id == 0) {
        set_game_status(state, "process scan no application count=%d", process_count);
        return false;
    }

    *pid_out = best_pid;
    *program_id_out = best_program_id;
    return true;
}

static bool current_game(AppState *state, u64 *application_id) {
    if (!state->svc_pminfo_ready) {
        set_game_status(state, "pminfo unavailable");
        return false;
    }

    u64 program_id = 0;
    u64 pid = 0;

    if (scan_process_list_for_application(state, &pid, &program_id)) {
        *application_id = program_id;
        set_game_status(state, "running scan=0x%016llx pid=%llu", (unsigned long long) program_id, (unsigned long long) pid);
        return true;
    }

    return false;
}

static void sanitize_id(const char *input, char *output, size_t output_size) {
    size_t j = 0;
    for (size_t i = 0; input[i] && j + 1 < output_size; ++i) {
        char ch = input[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) {
            output[j++] = ch;
        } else {
            output[j++] = '_';
        }
    }
    output[j] = '\0';
}

static const char *client_id_or_device(const AppConfig *config) {
    return config->mqtt_client_id[0] ? config->mqtt_client_id : config->device_name;
}

static void state_topic(const AppConfig *config, const char *sensor, char *topic, size_t topic_size) {
    char safe_id[80];
    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "switch_ha/%s/%s", safe_id, sensor);
}

static void discovery_topic(const AppConfig *config, const char *component, const char *sensor, char *topic, size_t topic_size) {
    char safe_id[80];
    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    snprintf(topic, topic_size, "%s/%s/%s/%s/config", config->mqtt_topic_prefix, component, safe_id, sensor);
}

static void publish_discovery_sensor(int fd, const AppConfig *config, const char *component, const char *sensor,
                                     const char *name, const char *device_class, const char *unit, const char *state_class) {
    char topic[256];
    char state[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    discovery_topic(config, component, sensor, topic, sizeof(topic));
    state_topic(config, sensor, state, sizeof(state));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}%s%s%s%s}",
             name,
             safe_id,
             sensor,
             state,
             safe_id,
             config->device_name,
             device_class ? ",\"dev_cla\":\"" : "",
             device_class ? device_class : "",
             device_class ? "\"" : "",
             unit ? "" : "");

    if (unit) {
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"unit_of_meas\":\"%s\"}", unit);
        }
    }
    if (state_class) {
        size_t len = strlen(payload);
        if (len > 0 && payload[len - 1] == '}') {
            payload[len - 1] = '\0';
            snprintf(payload + strlen(payload), sizeof(payload) - strlen(payload), ",\"stat_cla\":\"%s\"}", state_class);
        }
    }

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_button(int fd, const AppConfig *config, const char *button, const char *name, const char *payload_press) {
    char topic[256];
    char command_topic[SHA_MAX_TOPIC + 32];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    discovery_topic(config, "button", button, topic, sizeof(topic));
    snprintf(command_topic, sizeof(command_topic), "%s/command", config->mqtt_topic_prefix);

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"cmd_t\":\"%s\",\"payload_press\":\"%s\",\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             name,
             safe_id,
             button,
             command_topic,
             payload_press,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_discovery_notify(int fd, const AppConfig *config, const char *mode, const char *name) {
    char topic[256];
    char command_topic[192];
    char payload[MQTT_BUFFER];
    char safe_id[80];

    sanitize_id(client_id_or_device(config), safe_id, sizeof(safe_id));
    discovery_topic(config, "notify", mode, topic, sizeof(topic));
    notification_topic(config, mode, command_topic, sizeof(command_topic));

    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_notify_%s\",\"cmd_t\":\"%s\","
             "\"cmd_tpl\":\"{{ value }}\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"Nintendo\",\"mdl\":\"Switch\"}}",
             name,
             safe_id,
             mode,
             command_topic,
             safe_id,
             config->device_name);

    mqtt_send_publish(fd, topic, payload, true);
}

static void clear_discovery_topic(int fd, const AppConfig *config, const char *component, const char *sensor) {
    char topic[256];
    discovery_topic(config, component, sensor, topic, sizeof(topic));
    mqtt_send_publish(fd, topic, "", true);
}

static void clear_removed_discovery(AppState *state, int fd, const AppConfig *config) {
    (void) state;
    clear_discovery_topic(fd, config, "sensor", "connection_type");
    clear_discovery_topic(fd, config, "sensor", "controller_summary");
    clear_discovery_topic(fd, config, "sensor", "controller_unit_count");
    clear_discovery_topic(fd, config, "sensor", "current_app_id");
    clear_discovery_topic(fd, config, "sensor", "current_app_name");
    clear_discovery_topic(fd, config, "sensor", "fan_speed");
    clear_discovery_topic(fd, config, "sensor", "ip_address");
    clear_discovery_topic(fd, config, "sensor", "network_connected");
    clear_discovery_topic(fd, config, "sensor", "operation_mode");
    clear_discovery_topic(fd, config, "sensor", "pcb_temperature");
    clear_discovery_topic(fd, config, "sensor", "performance_mode");
    clear_discovery_topic(fd, config, "sensor", "soc_temperature");
    clear_discovery_topic(fd, config, "sensor", "wifi_rssi");
    clear_discovery_topic(fd, config, "sensor", "wifi_strength");
    clear_discovery_topic(fd, config, "binary_sensor", "audio_muted");
    clear_discovery_topic(fd, config, "binary_sensor", "docked");
    clear_discovery_topic(fd, config, "binary_sensor", "game_running");
    clear_discovery_topic(fd, config, "binary_sensor", "joycon_rail_attached");
    clear_discovery_topic(fd, config, "binary_sensor", "network_connected");
    clear_discovery_topic(fd, config, "binary_sensor", "tv_mode");
    clear_discovery_topic(fd, config, "sensor", "cradle_status");
}

static void publish_sensor_discovery(AppState *state, int fd, const AppConfig *config) {
    clear_removed_discovery(state, fd, config);

    publish_discovery_sensor(fd, config, "sensor", "battery", "Battery Level", "battery", "%", "measurement");
    publish_discovery_sensor(fd, config, "binary_sensor", "charging", "Is Charging", "battery_charging", NULL, NULL);
    publish_discovery_sensor(fd, config, "sensor", "charger_type", "Charger Type", NULL, NULL, NULL);
    publish_discovery_sensor(fd, config, "sensor", "battery_voltage", "Battery Voltage", "voltage", "mV", "measurement");
    publish_discovery_sensor(fd, config, "sensor", "battery_temperature", "Battery Temperature", "temperature", "C", "measurement");
    publish_discovery_sensor(fd, config, "sensor", "battery_health", "Battery Health", NULL, "%", "measurement");

    if (state->svc_lbl_ready) {
        publish_discovery_sensor(fd, config, "sensor", "brightness", "Screen Brightness", NULL, "%", "measurement");
        publish_discovery_sensor(fd, config, "binary_sensor", "backlight", "Screen", NULL, NULL, NULL);
    }
    if (state->svc_audctl_ready) {
        publish_discovery_sensor(fd, config, "sensor", "volume", "Volume", NULL, "%", "measurement");
        publish_discovery_sensor(fd, config, "sensor", "audio_target", "Audio Output Target", NULL, NULL, NULL);
    }
    publish_discovery_sensor(fd, config, "binary_sensor", "game_running", "Game Running", "running", NULL, NULL);
    publish_discovery_sensor(fd, config, "sensor", "current_game_id", "Current Game Title ID", NULL, NULL, NULL);
    clear_discovery_topic(fd, config, "sensor", "current_game_name");
    clear_discovery_topic(fd, config, "sensor", "game_pid");
    if (state->svc_hid_ready) {
        publish_discovery_sensor(fd, config, "sensor", "controller_count", "Player Count", NULL, NULL, "measurement");
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            char sensor[32];
            char name[32];
            snprintf(sensor, sizeof(sensor), "player_%d_controller", i + 1);
            snprintf(name, sizeof(name), "Player %d Controller", i + 1);
            publish_discovery_sensor(fd, config, "sensor", sensor, name, NULL, NULL, NULL);
        }
    }
    if (state->svc_spsm_ready) {
        publish_discovery_button(fd, config, "reboot", "Reboot", "reboot");
        publish_discovery_button(fd, config, "shutdown", "Shutdown", "shutdown");
    }
    publish_discovery_notify(fd, config, "popup", "Popup Notification");
    clear_discovery_topic(fd, config, "notify", "modal");
}

static void publish_value(int fd, const AppConfig *config, const char *sensor, const char *payload) {
    char topic[192];
    state_topic(config, sensor, topic, sizeof(topic));
    mqtt_send_publish(fd, topic, payload, true);
}

static void publish_if_changed(int fd, const AppConfig *config, SensorSnapshot *snapshot, bool force,
                               const char *sensor, char *previous, size_t previous_size, const char *current) {
    if (force || !snapshot->initialized || strncmp(previous, current, previous_size) != 0) {
        publish_value(fd, config, sensor, current);
        size_t len = strlen(current);
        if (len >= previous_size) {
            len = previous_size - 1;
        }
        memcpy(previous, current, len);
        previous[len] = '\0';
    }
}

static void publish_switch_sensors(AppState *state, int fd, const AppConfig *config, SensorSnapshot *snapshot, bool force) {
    char value[64];
    u32 percent = 0;
    PsmChargerType charger = PsmChargerType_Unconnected;
    PsmBatteryChargeInfoFields info = {0};

    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&percent))) {
        snprintf(value, sizeof(value), "%u", percent);
        publish_if_changed(fd, config, snapshot, force, "battery", snapshot->battery, sizeof(snapshot->battery), value);
    } else if (force) {
        publish_if_changed(fd, config, snapshot, force, "battery", snapshot->battery, sizeof(snapshot->battery), "0");
    }

    if (R_SUCCEEDED(psmGetChargerType(&charger))) {
        publish_if_changed(fd, config, snapshot, force, "charging", snapshot->charging, sizeof(snapshot->charging),
                           charger == PsmChargerType_Unconnected ? "OFF" : "ON");
        publish_if_changed(fd, config, snapshot, force, "charger_type", snapshot->charger_type, sizeof(snapshot->charger_type),
                           charger_type_name(charger));
    } else if (force) {
        publish_if_changed(fd, config, snapshot, force, "charging", snapshot->charging, sizeof(snapshot->charging), "OFF");
        publish_if_changed(fd, config, snapshot, force, "charger_type", snapshot->charger_type, sizeof(snapshot->charger_type), "unknown");
    }

    if (R_SUCCEEDED(psmGetBatteryChargeInfoFields(&info))) {
        snprintf(value, sizeof(value), "%u", info.battery_charge_milli_voltage);
        publish_if_changed(fd, config, snapshot, force, "battery_voltage", snapshot->battery_voltage, sizeof(snapshot->battery_voltage), value);

        snprintf(value, sizeof(value), "%.1f", (double) info.temperature_celcius / 1000.0);
        publish_if_changed(fd, config, snapshot, force, "battery_temperature", snapshot->battery_temperature, sizeof(snapshot->battery_temperature), value);

        snprintf(value, sizeof(value), "%.1f", (double) info.battery_age_percentage / 1000.0);
        publish_if_changed(fd, config, snapshot, force, "battery_health", snapshot->battery_health, sizeof(snapshot->battery_health), value);
    } else if (force) {
        publish_if_changed(fd, config, snapshot, force, "battery_voltage", snapshot->battery_voltage, sizeof(snapshot->battery_voltage), "0");
        publish_if_changed(fd, config, snapshot, force, "battery_temperature", snapshot->battery_temperature, sizeof(snapshot->battery_temperature), "0");
        publish_if_changed(fd, config, snapshot, force, "battery_health", snapshot->battery_health, sizeof(snapshot->battery_health), "0");
    }

    if (state->svc_lbl_ready) {
        float brightness = 0.0f;
        if (R_SUCCEEDED(lblGetCurrentBrightnessSetting(&brightness)) || R_SUCCEEDED(lblGetBrightnessSettingAppliedToBacklight(&brightness))) {
            snprintf(value, sizeof(value), "%.0f", (double) (brightness * 100.0f));
            publish_if_changed(fd, config, snapshot, force, "brightness", snapshot->brightness, sizeof(snapshot->brightness), value);
        }

        LblBacklightSwitchStatus backlight = LblBacklightSwitchStatus_Disabled;
        if (R_SUCCEEDED(lblGetBacklightSwitchStatus(&backlight))) {
            publish_if_changed(fd, config, snapshot, force, "backlight", snapshot->backlight, sizeof(snapshot->backlight),
                               backlight == LblBacklightSwitchStatus_Enabled || backlight == LblBacklightSwitchStatus_Enabling ? "ON" : "OFF");
        }
    }

    if (state->svc_audctl_ready) {
        AudioTarget audio_target = AudioTarget_Invalid;
        if (R_SUCCEEDED(audctlGetActiveOutputTarget(&audio_target)) || R_SUCCEEDED(audctlGetDefaultTarget(&audio_target))) {
            publish_if_changed(fd, config, snapshot, force, "audio_target", snapshot->audio_target, sizeof(snapshot->audio_target),
                               audio_target_name(audio_target));

            s32 target_volume = 0;
            s32 min_volume = 0;
            s32 max_volume = 0;
            if (R_SUCCEEDED(audctlGetTargetVolume(&target_volume, audio_target)) &&
                R_SUCCEEDED(audctlGetTargetVolumeMin(&min_volume)) &&
                R_SUCCEEDED(audctlGetTargetVolumeMax(&max_volume)) &&
                max_volume > min_volume) {
                double pct = ((double) (target_volume - min_volume) * 100.0) / (double) (max_volume - min_volume);
                if (pct < 0.0) pct = 0.0;
                if (pct > 100.0) pct = 100.0;
                snprintf(value, sizeof(value), "%.0f", pct);
                publish_if_changed(fd, config, snapshot, force, "volume", snapshot->volume, sizeof(snapshot->volume), value);
            } else {
                float master_volume = 0.0f;
                if (R_SUCCEEDED(audctlGetSystemOutputMasterVolume(&master_volume))) {
                    snprintf(value, sizeof(value), "%.0f", (double) (master_volume * 100.0f));
                    publish_if_changed(fd, config, snapshot, force, "volume", snapshot->volume, sizeof(snapshot->volume), value);
                }
            }
        }
    }

    u64 application_id = 0;
    if (current_game(state, &application_id)) {
        publish_if_changed(fd, config, snapshot, force, "game_running", snapshot->game_running, sizeof(snapshot->game_running), "ON");
        if (application_id != 0) {
            snprintf(value, sizeof(value), "%016llx", (unsigned long long) application_id);
        } else {
            snprintf(value, sizeof(value), "unknown");
        }
        publish_if_changed(fd, config, snapshot, force, "current_game_id", snapshot->current_game_id, sizeof(snapshot->current_game_id), value);
    } else {
        publish_if_changed(fd, config, snapshot, force, "game_running", snapshot->game_running, sizeof(snapshot->game_running), "OFF");
        publish_if_changed(fd, config, snapshot, force, "current_game_id", snapshot->current_game_id, sizeof(snapshot->current_game_id), "none");
    }

    HidNpadIdType player_ids[] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8
    };
    if (state->svc_hid_ready) {
        int player_controller_count = 0;
        char player_values[MAX_PLAYERS][32];

        for (int i = 0; i < MAX_PLAYERS; ++i) {
            snprintf(player_values[i], sizeof(player_values[i]), "none");
        }

        for (size_t i = 0; i < sizeof(player_ids) / sizeof(player_ids[0]); ++i) {
            u32 style = hidGetNpadStyleSet(player_ids[i]);
            u32 attributes = 0;
            if (style == 0 || !npad_has_state(player_ids[i], style, &attributes)) {
                continue;
            }

            player_controller_count++;
            controller_value(player_values[i], sizeof(player_values[i]), style);
        }

        if (player_controller_count == 0) {
            u32 style = hidGetNpadStyleSet(HidNpadIdType_Handheld);
            u32 attributes = 0;
            if (style != 0 && npad_has_state(HidNpadIdType_Handheld, style, &attributes)) {
                player_controller_count = 1;
                controller_value(player_values[0], sizeof(player_values[0]), style);
            }
        }

        snprintf(value, sizeof(value), "%d", player_controller_count);
        publish_if_changed(fd, config, snapshot, force, "controller_count", snapshot->player_controller_count, sizeof(snapshot->player_controller_count), value);
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            char sensor[32];
            snprintf(sensor, sizeof(sensor), "player_%d_controller", i + 1);
            publish_if_changed(fd, config, snapshot, force, sensor, snapshot->player_controller[i], sizeof(snapshot->player_controller[i]), player_values[i]);
        }
    }

    snapshot->initialized = true;
}

static void publish_state(AppState *state, int fd, const AppConfig *config, const char *status) {
    char topic[SHA_MAX_TOPIC + 32];
    char payload[256];
    snprintf(topic, sizeof(topic), "%s/status", config->mqtt_topic_prefix);
    snprintf(payload, sizeof(payload),
             "{\"device\":\"%s\",\"client_id\":\"%s\",\"status\":\"%s\",\"ha_validated\":%s}",
             config->device_name,
             config->mqtt_client_id[0] ? config->mqtt_client_id : config->device_name,
             status,
             state->ha_validated ? "true" : "false");
    mqtt_send_publish(fd, topic, payload, true);
}

static bool read_mqtt_remaining_length(const unsigned char *buffer, int size, int *value, int *bytes_used) {
    int multiplier = 1;
    int result = 0;
    int i = 1;

    while (i < size && i <= 4) {
        unsigned char encoded = buffer[i++];
        result += (encoded & 127) * multiplier;
        if ((encoded & 128) == 0) {
            *value = result;
            *bytes_used = i - 1;
            return true;
        }
        multiplier *= 128;
    }

    return false;
}

static void handle_command_payload(AppState *state, int fd, const AppConfig *config, const char *payload) {
    if (!payload || payload[0] == '\0') {
        return;
    }

    app_state_push_log(state, "MQTT command: %s", payload);

    if (strcasecmp(payload, "reboot") == 0) {
        if (!state->svc_spsm_ready) {
            app_state_push_log(state, "Reboot unavailable: spsm not ready");
            return;
        }
        app_state_push_log(state, "Reboot command accepted");
        spsmShutdown(true);
    } else if (strcasecmp(payload, "shutdown") == 0 || strcasecmp(payload, "poweroff") == 0) {
        if (!state->svc_spsm_ready) {
            app_state_push_log(state, "Shutdown unavailable: spsm not ready");
            return;
        }
        app_state_push_log(state, "Shutdown command accepted");
        spsmShutdown(false);
    } else if (strcasecmp(payload, "sleep") == 0 || strcasecmp(payload, "standby") == 0) {
        app_state_push_log(state, "Sleep command removed: unsupported in sysmodule");
    } else {
        app_state_push_log(state, "Unknown MQTT command");
    }
}

static void handle_notification_payload(AppState *state, int fd, const AppConfig *config, const char *topic, const char *payload) {
    char popup_topic[192];
    char status_topic[192];
    char mode[16];
    char title[96];
    char message[NOTIFY_TEXT_MAX];

    (void) topic;
    notification_topic(config, "popup", popup_topic, sizeof(popup_topic));
    notification_status_topic(config, status_topic, sizeof(status_topic));

    snprintf(mode, sizeof(mode), "popup");
    snprintf(title, sizeof(title), "Home Assistant");
    copy_text(message, sizeof(message), payload);

    if (payload[0] == '{') {
        json_extract_string(payload, "title", title, sizeof(title));
        json_extract_string(payload, "message", message, sizeof(message));
    } else {
        const char *line = strchr(payload, '\n');
        if (line && line > payload && (size_t) (line - payload) < sizeof(title)) {
            memcpy(title, payload, (size_t) (line - payload));
            title[line - payload] = '\0';
            copy_text(message, sizeof(message), line + 1);
        }
    }

    trim_text(mode);
    trim_text(title);
    trim_text(message);
    if (message[0] == '\0') {
        mqtt_send_publish(fd, status_topic, "empty", false);
        return;
    }

    bool ok = write_notification_files(mode, title, message);
    mqtt_send_publish(fd, status_topic, ok ? "queued" : "queue_failed", false);
    app_state_push_log(state, "Notify %s: %.48s", mode, message);
}

static void handle_mqtt_publish(AppState *state, int fd, const AppConfig *config, const unsigned char *buffer, int size) {
    int remaining = 0;
    int remaining_bytes = 0;
    if (!read_mqtt_remaining_length(buffer, size, &remaining, &remaining_bytes)) {
        return;
    }

    int pos = 1 + remaining_bytes;
    if (pos + 2 > size) {
        return;
    }

    int topic_len = ((int) buffer[pos] << 8) | buffer[pos + 1];
    pos += 2;
    if (topic_len <= 0 || pos + topic_len > size) {
        return;
    }

    char topic[192];
    int copy_topic_len = topic_len;
    if (copy_topic_len >= (int) sizeof(topic)) {
        copy_topic_len = (int) sizeof(topic) - 1;
    }
    memcpy(topic, buffer + pos, (size_t) copy_topic_len);
    topic[copy_topic_len] = '\0';
    pos += topic_len;

    int qos = (buffer[0] & 0x06) >> 1;
    if (qos > 0) {
        pos += 2;
    }
    if (pos > size) {
        return;
    }

    int payload_len = size - pos;
    if (payload_len <= 0) {
        return;
    }
    if (payload_len >= NOTIFY_TEXT_MAX) {
        payload_len = NOTIFY_TEXT_MAX - 1;
    }

    char payload[NOTIFY_TEXT_MAX];
    memcpy(payload, buffer + pos, (size_t) payload_len);
    payload[payload_len] = '\0';

    char command_topic[SHA_MAX_TOPIC + 32];
    char popup_topic[192];
    snprintf(command_topic, sizeof(command_topic), "%s/command", config->mqtt_topic_prefix);
    notification_topic(config, "popup", popup_topic, sizeof(popup_topic));

    if (strcmp(topic, command_topic) == 0) {
        handle_command_payload(state, fd, config, payload);
    } else if (strcmp(topic, popup_topic) == 0) {
        handle_notification_payload(state, fd, config, topic, payload);
    }
}

static void mqtt_loop(void *arg) {
    AppState *state = arg;
    u64 last_sensor_poll_ms = 0;
    u64 last_heartbeat_ms = 0;
    app_state_push_log(state, "MQTT thread started");

    while (true) {
        mutexLock(&state->lock);
        bool should_exit = state->exit_requested;
        AppConfig config = state->config;
        char prefix[SHA_MAX_TOPIC];
        strncpy(prefix, state->config.mqtt_topic_prefix, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';
        mutexUnlock(&state->lock);

        char host[SHA_MAX_FIELD];
        int port = config.mqtt_port;
        normalize_endpoint(&config, host, sizeof(host), &port);

        if (should_exit) {
            break;
        }

        if (host[0] == '\0' || prefix[0] == '\0') {
            mqtt_set_status(state, false, false, false, "MQTT incomplete config");
            svcSleepThread(1000 * 1000 * 1000ULL);
            continue;
        }

        if (port <= 0 || port > 65535) {
            mqtt_set_status(state, false, false, false, "Invalid MQTT port: %d", port);
            app_state_push_log(state, "MQTT invalid port: %d", port);
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        if (port == 8123) {
            mqtt_set_status(state, true, false, false, "Port 8123 is HA HTTP; MQTT is usually 1883");
            app_state_push_log(state, "MQTT wrong port: 8123 is HA HTTP");
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        int fd = -1;
        app_state_push_log(state, "MQTT connecting %s:%d", host, port);
        if (!socket_connect_to(state, host, port, &fd)) {
            mutexLock(&state->lock);
            char error[SHA_LOG_LINE];
            strncpy(error, state->mqtt_last_error, sizeof(error) - 1);
            error[sizeof(error) - 1] = '\0';
            mutexUnlock(&state->lock);
            app_state_push_log(state, "MQTT %s", error);
            app_state_set_status(state, "%s", error);
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        g_ctx.socket_fd = fd;

        if (!mqtt_send_connect(&config, fd)) {
            mqtt_set_status(state, true, true, false, "MQTT CONNECT send failed errno=%d", errno);
            app_state_push_log(state, "MQTT CONNECT send failed errno=%d", errno);
            close(fd);
            g_ctx.socket_fd = -1;
            svcSleepThread(1000 * 1000 * 1000ULL);
            continue;
        }

        unsigned char ack[4] = {0};
        if (!socket_recv_all(fd, ack, sizeof(ack)) || ack[0] != 0x20 || ack[1] != 0x02 || ack[3] != 0x00) {
            int return_code = ack[3];
            mqtt_set_status(state, true, true, false, "MQTT rejected rc=%d %s", return_code, connack_reason(return_code));
            app_state_push_log(state, "MQTT rejected rc=%d %s", return_code, connack_reason(return_code));
            close(fd);
            g_ctx.socket_fd = -1;
            svcSleepThread(2 * 1000 * 1000 * 1000ULL);
            continue;
        }

        char command_topic[SHA_MAX_TOPIC + 32];
        snprintf(command_topic, sizeof(command_topic), "%s/command", prefix);
        mqtt_send_subscribe(fd, command_topic);
        char popup_topic[192];
        notification_topic(&config, "popup", popup_topic, sizeof(popup_topic));
        mqtt_send_subscribe(fd, popup_topic);

        mutexLock(&state->lock);
        state->mqtt_dns_ok = true;
        state->mqtt_tcp_ok = true;
        state->mqtt_connected = true;
        snprintf(state->mqtt_last_error, sizeof(state->mqtt_last_error), "connected");
        mutexUnlock(&state->lock);
        app_state_push_log(state, "MQTT connected");
        app_state_set_status(state, "MQTT connected");
        SensorSnapshot sensor_snapshot = {0};
        publish_sensor_discovery(state, fd, &config);
        publish_state(state, fd, &config, "online");
        publish_switch_sensors(state, fd, &config, &sensor_snapshot, true);
        last_sensor_poll_ms = monotonic_ms();
        last_heartbeat_ms = last_sensor_poll_ms;

        while (true) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int poll_rc = poll(&pfd, 1, 1000);
            if (poll_rc < 0) {
                break;
            }
            if (poll_rc > 0 && (pfd.revents & POLLIN)) {
                unsigned char buffer[MQTT_BUFFER];
                int size = recv(fd, buffer, sizeof(buffer), 0);
                if (size <= 0) {
                    break;
                }
                if ((buffer[0] & 0xF0) == 0x30) {
                    handle_mqtt_publish(state, fd, &config, buffer, size);
                }
            }

            mutexLock(&state->lock);
            should_exit = state->exit_requested;
            mutexUnlock(&state->lock);
            if (should_exit) {
                break;
            }

            u64 now = monotonic_ms();
            if (now - last_sensor_poll_ms >= SENSOR_POLL_INTERVAL_MS) {
                publish_switch_sensors(state, fd, &config, &sensor_snapshot, false);
                last_sensor_poll_ms = now;
            }

            if (now - last_heartbeat_ms >= MQTT_HEARTBEAT_INTERVAL_MS) {
                if (!mqtt_send_ping(fd)) {
                    break;
                }
                publish_state(state, fd, &config, "online");
                last_heartbeat_ms = now;
            }
        }

        publish_state(state, fd, &config, "offline");
        close(fd);
        g_ctx.socket_fd = -1;
        mutexLock(&state->lock);
        state->mqtt_tcp_ok = false;
        state->mqtt_connected = false;
        snprintf(state->mqtt_last_error, sizeof(state->mqtt_last_error), "disconnected");
        mutexUnlock(&state->lock);
        app_state_push_log(state, "MQTT disconnected");
    }
}

bool mqtt_service_start(AppState *state) {
    mutexLock(&state->lock);
    state->mqtt_running = true;
    mutexUnlock(&state->lock);
    g_ctx.socket_fd = -1;
    g_ctx.active = R_SUCCEEDED(threadCreate(&g_ctx.thread, mqtt_loop, state, NULL, 0x8000, 0x2B, -2));
    if (!g_ctx.active) {
        app_state_push_log(state, "Failed to start MQTT thread");
        return false;
    }
    threadStart(&g_ctx.thread);
    return true;
}

void mqtt_service_stop(AppState *state) {
    mutexLock(&state->lock);
    state->exit_requested = true;
    mutexUnlock(&state->lock);

    if (g_ctx.socket_fd >= 0) {
        shutdown(g_ctx.socket_fd, SHUT_RDWR);
    }
    if (g_ctx.active) {
        threadWaitForExit(&g_ctx.thread);
        threadClose(&g_ctx.thread);
        g_ctx.active = false;
    }
}

bool mqtt_test_connection(AppState *state) {
    AppConfig config;
    mutexLock(&state->lock);
    config = state->config;
    mutexUnlock(&state->lock);

    char test_client_id[sizeof(config.mqtt_client_id)];
    snprintf(test_client_id, sizeof(test_client_id), "%.58s-test", client_id_or_device(&config));
    strncpy(config.mqtt_client_id, test_client_id, sizeof(config.mqtt_client_id) - 1);
    config.mqtt_client_id[sizeof(config.mqtt_client_id) - 1] = '\0';

    char host[SHA_MAX_FIELD];
    int port = config.mqtt_port;
    normalize_endpoint(&config, host, sizeof(host), &port);

    if (host[0] == '\0') {
        mqtt_set_status(state, false, false, false, "MQTT host missing");
        app_state_push_log(state, "MQTT test failed: host missing");
        return false;
    }
    if (port <= 0 || port > 65535) {
        mqtt_set_status(state, false, false, false, "Invalid MQTT port: %d", port);
        app_state_push_log(state, "MQTT test failed: invalid port");
        return false;
    }
    if (port == 8123) {
        mqtt_set_status(state, true, false, false, "Port 8123 is HA HTTP; MQTT is usually 1883");
        app_state_push_log(state, "MQTT test failed: port 8123");
        return false;
    }

    int fd = -1;
    app_state_push_log(state, "MQTT test %s:%d", host, port);
    if (!socket_connect_to(state, host, port, &fd)) {
        mutexLock(&state->lock);
        char error[SHA_LOG_LINE];
        strncpy(error, state->mqtt_last_error, sizeof(error) - 1);
        error[sizeof(error) - 1] = '\0';
        mutexUnlock(&state->lock);
        app_state_push_log(state, "MQTT test failed: %s", error);
        return false;
    }

    bool ok = false;
    if (!mqtt_send_connect(&config, fd)) {
        mqtt_set_status(state, true, true, false, "MQTT CONNECT send failed errno=%d", errno);
        app_state_push_log(state, "MQTT test failed: CONNECT send");
    } else {
        unsigned char ack[4] = {0};
        if (socket_recv_all(fd, ack, sizeof(ack)) && ack[0] == 0x20 && ack[1] == 0x02 && ack[3] == 0x00) {
            mqtt_set_status(state, true, true, true, "MQTT test OK");
            app_state_push_log(state, "MQTT test OK");
            ok = true;
        } else {
            int return_code = ack[3];
            mqtt_set_status(state, true, true, false, "MQTT rejected rc=%d %s", return_code, connack_reason(return_code));
            app_state_push_log(state, "MQTT test rejected rc=%d %s", return_code, connack_reason(return_code));
        }
    }

    close(fd);
    return ok;
}
