#include "mqtt/mqtt_proto.h"
#include "mqtt/mqtt_topics.h"

#include <switch.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

uint64_t mqtt_monotonic_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

void mqtt_set_status(AppState *state, bool dns_ok, bool tcp_ok, bool session_ok, const char *fmt, ...) {
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

bool mqtt_parse_port(const char *value, int *port) {
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

void mqtt_normalize_endpoint(const AppConfig *config, char *host, size_t host_size, int *port) {
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
        if (mqtt_parse_port(colon + 1, &parsed_port)) {
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

const char *mqtt_connack_reason(int return_code) {
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

const char *mqtt_tcp_errno_reason(int code) {
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
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        *out_fd = fd;
        mqtt_set_status(state, true, true, false, "TCP OK: %s:%d", host, port);
        return true;
    }

    int last_errno = errno;
    close(fd);
    mqtt_set_status(state, true, false, false, "TCP failed: %s:%d %s errno=%d", host, port, mqtt_tcp_errno_reason(last_errno), last_errno);
    return true;
}

bool mqtt_socket_connect_to(AppState *state, const char *host, int port, int *out_fd) {
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
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            struct timeval tv = {.tv_sec = 10, .tv_usec = 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
    mqtt_set_status(state, true, false, false, "TCP failed: %s:%d %s errno=%d", host, port, mqtt_tcp_errno_reason(last_errno), last_errno);
    return false;
}

bool mqtt_send_packet(int fd, const unsigned char *buffer, size_t size) {
    size_t sent_total = 0;
    int retries = 0;
    while (sent_total < size) {
        ssize_t sent = send(fd, buffer + sent_total, size - sent_total, 0);
        if (sent > 0) {
            sent_total += (size_t) sent;
            retries = 0;
        } else if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++retries > 50) { // allow up to ~500ms of buffer drainage
                return false;
            }
            svcSleepThread(10ULL * 1000ULL * 1000ULL); // 10ms
        } else {
            return false;
        }
    }
    return true;
}

bool mqtt_socket_recv_all(int fd, unsigned char *buffer, size_t size) {
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

int mqtt_encode_remaining_length(unsigned char *dst, int len) {
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

bool mqtt_read_remaining_length(const unsigned char *buffer, int size, int *value, int *bytes_used) {
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

unsigned char *mqtt_write_string(unsigned char *dst, const char *value) {
    const size_t len = strlen(value);
    *dst++ = (unsigned char) ((len >> 8) & 0xFF);
    *dst++ = (unsigned char) (len & 0xFF);
    memcpy(dst, value, len);
    return dst + len;
}

bool mqtt_send_connect(const AppConfig *config, int fd) {
    unsigned char packet[MQTT_BUFFER];
    unsigned char variable[MQTT_BUFFER];
    unsigned char *ptr = variable;

    char will_topic[192];
    mqtt_availability_topic(config, will_topic, sizeof(will_topic));
    const char *will_payload = "offline";

    ptr = mqtt_write_string(ptr, "MQTT");
    *ptr++ = 0x04;
    // Clean session (0x02) + Will Flag (0x04) + Will Retain (0x20) = 0x26
    unsigned char flags = 0x02 | 0x04 | 0x20;
    if (config->mqtt_username[0] != '\0') {
        flags |= 0x80;
    }
    if (config->mqtt_password[0] != '\0') {
        flags |= 0x40;
    }
    *ptr++ = flags;
    *ptr++ = 0x00;
    *ptr++ = MQTT_KEEPALIVE;
    ptr = mqtt_write_string(ptr, config->mqtt_client_id[0] ? config->mqtt_client_id : config->device_name);

    // Will Topic and Will Message
    ptr = mqtt_write_string(ptr, will_topic);
    ptr = mqtt_write_string(ptr, will_payload);

    if (config->mqtt_username[0] != '\0') {
        ptr = mqtt_write_string(ptr, config->mqtt_username);
    }
    if (config->mqtt_password[0] != '\0') {
        ptr = mqtt_write_string(ptr, config->mqtt_password);
    }

    size_t payload_len = (size_t) (ptr - variable);
    packet[0] = 0x10;
    int remain = mqtt_encode_remaining_length(packet + 1, (int) payload_len);
    memcpy(packet + 1 + remain, variable, payload_len);
    return mqtt_send_packet(fd, packet, 1 + remain + payload_len);
}

bool mqtt_send_publish_bytes(int fd, const char *topic, const void *payload, size_t payload_len, bool retain) {
    size_t topic_len = strlen(topic);
    size_t remain_len = 2 + topic_len + payload_len;
    size_t packet_size = 1 + 4 + remain_len;
    unsigned char *packet = malloc(packet_size);
    if (!packet) {
        return false;
    }
    unsigned char *ptr = packet;
    *ptr++ = retain ? 0x31 : 0x30;
    ptr += mqtt_encode_remaining_length(ptr, (int) remain_len);
    ptr = mqtt_write_string(ptr, topic);
    memcpy(ptr, payload, payload_len);
    ptr += payload_len;
    bool ok = mqtt_send_packet(fd, packet, (size_t) (ptr - packet));
    free(packet);
    return ok;
}

bool mqtt_send_publish(int fd, const char *topic, const char *payload, bool retain) {
    return mqtt_send_publish_bytes(fd, topic, payload, strlen(payload), retain);
}

bool mqtt_send_subscribe(int fd, const char *topic) {
    unsigned char packet[MQTT_BUFFER];
    unsigned char *ptr = packet;
    *ptr++ = 0x82;

    size_t topic_len = strlen(topic);
    size_t remain_len = 2 + 2 + topic_len + 1;
    ptr += mqtt_encode_remaining_length(ptr, (int) remain_len);
    *ptr++ = 0x00;
    *ptr++ = 0x01;
    ptr = mqtt_write_string(ptr, topic);
    *ptr++ = 0x00;
    return mqtt_send_packet(fd, packet, (size_t) (ptr - packet));
}

bool mqtt_send_ping(int fd) {
    const unsigned char ping[2] = {0xC0, 0x00};
    return mqtt_send_packet(fd, ping, sizeof(ping));
}

// A clean DISCONNECT makes the broker discard the will message, so the retained
// "offline" availability is not published when the console goes to sleep.
bool mqtt_send_disconnect(int fd) {
    const unsigned char disconnect[2] = {0xE0, 0x00};
    return mqtt_send_packet(fd, disconnect, sizeof(disconnect));
}
