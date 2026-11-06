#pragma once

#include "app_state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MQTT_BUFFER 2048
#define MQTT_KEEPALIVE 30
#define SENSOR_POLL_INTERVAL_MS 1000
#define MQTT_HEARTBEAT_INTERVAL_MS 15000

uint64_t mqtt_monotonic_ms(void);
void mqtt_set_status(AppState *state, bool dns_ok, bool tcp_ok, bool session_ok, const char *fmt, ...);
bool mqtt_parse_port(const char *value, int *port);
void mqtt_normalize_endpoint(const AppConfig *config, char *host, size_t host_size, int *port);

const char *mqtt_connack_reason(int return_code);
const char *mqtt_tcp_errno_reason(int code);

bool mqtt_socket_connect_to(AppState *state, const char *host, int port, int *out_fd);
bool mqtt_socket_recv_all(int fd, unsigned char *buffer, size_t size);
bool mqtt_send_packet(int fd, const unsigned char *buffer, size_t size);

int mqtt_encode_remaining_length(unsigned char *dst, int len);
bool mqtt_read_remaining_length(const unsigned char *buffer, int size, int *value, int *bytes_used);
unsigned char *mqtt_write_string(unsigned char *dst, const char *value);

bool mqtt_send_connect(const AppConfig *config, int fd);
bool mqtt_send_publish_bytes(int fd, const char *topic, const void *payload, size_t payload_len, bool retain);
bool mqtt_send_publish(int fd, const char *topic, const char *payload, bool retain);
bool mqtt_send_subscribe(int fd, const char *topic);
bool mqtt_send_ping(int fd);
bool mqtt_send_disconnect(int fd);
