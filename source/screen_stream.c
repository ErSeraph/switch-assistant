#include "screen_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>

#define SCREEN_STREAM_RTSP_PORT 6666
#define SCREEN_STREAM_VBUF_SIZE 0x54000
#define SCREEN_STREAM_MAX_RTP_PACKET (8 * 1024)
#define SCREEN_STREAM_RTP_HEADER_SIZE 12
#define SCREEN_STREAM_INTERLEAVED_HEADER_SIZE 4
#define SCREEN_STREAM_MAX_RTP_PAYLOAD (SCREEN_STREAM_MAX_RTP_PACKET - SCREEN_STREAM_RTP_HEADER_SIZE)
#define SCREEN_STREAM_PACKET_MAGIC 0xCCCCCCCC
#define SCREEN_STREAM_NOT_INITIALIZED 0x3E8D4
#define SCREEN_STREAM_CONTROL_TIMEOUT_MS 1000
#define SCREEN_STREAM_RTP_SEND_TIMEOUT_MS 15

typedef struct {
    u32 magic;
    u32 data_size;
    u64 timestamp;
    u8 metadata;
    u8 replay_slot;
} __attribute__((packed)) ScreenPacketHeader;

typedef struct {
    ScreenPacketHeader header;
    unsigned char data[SCREEN_STREAM_VBUF_SIZE];
} ScreenVideoPacket;

static AppState *g_state = NULL;
static Service g_grcd_video = {0};
static Mutex g_begin_mutex;
static Mutex g_client_lock;
static Thread g_server_thread = {0};
static Thread g_video_thread = {0};
static atomic_bool g_running = false;
static atomic_bool g_paused = false;
static atomic_bool g_listener_ready = false;
static atomic_bool g_client_connected = false;
static atomic_bool g_client_streaming = false;
static bool g_begin_called = false;
static bool g_force_sps_pps = false;
static Result g_grcd_open_result = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
static Result g_grcd_begin_result = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
static int g_listener_fd = -1;
static int g_client_fd = -1;
static u16 g_rtp_sequence = 1;
static ScreenVideoPacket g_video_packet __attribute__((aligned(0x1000)));
static unsigned char g_send_buffer[SCREEN_STREAM_MAX_RTP_PACKET + SCREEN_STREAM_INTERLEAVED_HEADER_SIZE];

static const unsigned char g_sps[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B,
    0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80
};
static const unsigned char g_pps[] = {
    0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0
};

static const char g_sdp[] =
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=Switch-HA Screen\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "t=0 0\r\n"
    "m=video 0 RTP/AVP 96\r\n"
    "a=rtpmap:96 H264/90000\r\n"
    "a=fmtp:96 packetization-mode=1; profile-level-id=42A01E; sprop-parameter-sets=Z2QMIKwrQCgC3TUBDQHggA==,aO48sA==\r\n"
    "a=control:*\r\n"
    "a=control:streamid=0\r\n";

static void screen_stream_log(const char *fmt, ...) {
    if (!g_state) {
        return;
    }

    char line[SHA_LOG_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    app_state_push_log(g_state, "%s", line);
}

static const char *find_token_case_insensitive(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return haystack;
    }

    for (const char *cur = haystack; *cur; ++cur) {
        if (strncasecmp(cur, needle, needle_len) == 0) {
            return cur;
        }
    }

    return NULL;
}

static bool wait_for_socket_event(int fd, short events, int timeout_ms, bool *timed_out) {
    struct pollfd pfd = {
        .fd = fd,
        .events = events,
        .revents = 0,
    };

    if (timed_out) {
        *timed_out = false;
    }

    while (g_running) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0) {
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return false;
            }
            if ((pfd.revents & events) != 0) {
                return true;
            }
        } else if (rc == 0) {
            if (timed_out) {
                *timed_out = true;
            }
            return false;
        } else {
            return false;
        }
    }

    return false;
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void close_socket(int *fd) {
    if (*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
}

static void disconnect_client(void) {
    mutexLock(&g_client_lock);
    close_socket(&g_client_fd);
    close_socket(&g_listener_fd);
    mutexUnlock(&g_client_lock);
    g_client_connected = false;
    g_client_streaming = false;
    g_listener_ready = false;
}

static Result grcd_service_open(Service *out) {
    if (serviceIsActive(out)) {
        return 0;
    }

    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }

    rc = smGetService(out, "grc:d");
    smExit();

    if (R_FAILED(rc)) {
        serviceClose(out);
    }

    return rc;
}

static Result grcd_service_begin(Service *svc) {
    return serviceDispatch(svc, 1);
}

static Result grcd_service_transfer(Service *svc, GrcStream stream, void *buffer, size_t size,
                                    u32 *num_frames, u32 *data_size, u64 *start_timestamp) {
    struct {
        u32 num_frames;
        u32 data_size;
        u64 start_timestamp;
    } out = {0};

    u32 tmp = (u32) stream;
    Result rc = serviceDispatchInOut(svc, 2, tmp, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } });

    if (R_SUCCEEDED(rc)) {
        if (num_frames) {
            *num_frames = out.num_frames;
        }
        if (data_size) {
            *data_size = out.data_size;
        }
        if (start_timestamp) {
            *start_timestamp = out.start_timestamp;
        }
    }

    return rc;
}

static bool ensure_grcd_begin(Result *rc) {
    if (g_begin_called) {
        return false;
    }

    mutexLock(&g_begin_mutex);
    if (!g_begin_called) {
        g_begin_called = true;
        g_grcd_begin_result = grcd_service_begin(&g_grcd_video);
        *rc = g_grcd_begin_result;
    } else {
        *rc = g_grcd_begin_result;
    }
    mutexUnlock(&g_begin_mutex);

    return true;
}

static void video_connected(void) {
    g_video_packet.header.magic = SCREEN_STREAM_PACKET_MAGIC;
    g_video_packet.header.replay_slot = 0xFF;
    g_force_sps_pps = true;
    g_rtp_sequence = 1;
}

static bool capture_read_video(void) {
    u32 data_size = 0;
    u64 timestamp = 0;

    Result rc = grcd_service_transfer(&g_grcd_video, GrcStream_Video, g_video_packet.data, sizeof(g_video_packet.data),
                                      NULL, &data_size, &timestamp);

    if (rc == SCREEN_STREAM_NOT_INITIALIZED && ensure_grcd_begin(&rc)) {
        if (R_FAILED(rc)) {
            return false;
        }
        return capture_read_video();
    }

    g_video_packet.header.data_size = data_size;
    g_video_packet.header.timestamp = timestamp;

    if (R_FAILED(rc) || data_size <= 4) {
        return false;
    }

    {
        const bool is_idr = (g_video_packet.data[4] & 0x1F) == 5;
        static int idr_count = 0;
        const bool emit_meta = g_force_sps_pps || (is_idr && ++idr_count >= 5);
        const size_t meta_size = sizeof(g_sps) + sizeof(g_pps);

        if (emit_meta && (sizeof(g_video_packet.data) - data_size) >= meta_size) {
            idr_count = 0;
            g_force_sps_pps = false;
            memmove(g_video_packet.data + meta_size, g_video_packet.data, data_size);
            memcpy(g_video_packet.data, g_sps, sizeof(g_sps));
            memcpy(g_video_packet.data + sizeof(g_sps), g_pps, sizeof(g_pps));
            g_video_packet.header.data_size += (u32) meta_size;
        }
    }

    return true;
}

static bool send_all_locked(const void *buffer, size_t size, int timeout_ms, const char *slow_log) {
    bool ok = true;
    mutexLock(&g_client_lock);
    if (g_client_fd < 0) {
        ok = false;
    } else {
        const unsigned char *ptr = buffer;
        size_t remaining = size;
        while (remaining > 0) {
            bool timed_out = false;
            if (!wait_for_socket_event(g_client_fd, POLLOUT, timeout_ms, &timed_out)) {
                ok = false;
                if (timed_out && slow_log) {
                    screen_stream_log("%s", slow_log);
                } else {
                    screen_stream_log("rtsp send wait failed");
                }
                break;
            }

            ssize_t sent = send(g_client_fd, ptr, remaining, 0);
            if (sent <= 0) {
                if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    continue;
                }
                ok = false;
                screen_stream_log("rtsp send failed errno=%d", errno);
                break;
            }
            ptr += sent;
            remaining -= (size_t) sent;
        }
        if (!ok) {
            close_socket(&g_client_fd);
            g_client_connected = false;
            g_client_streaming = false;
        }
    }
    mutexUnlock(&g_client_lock);
    return ok;
}

static void prepare_rtp_header(unsigned char *header, u32 ts90k, bool marker) {
    header[0] = 2u << 6;
    header[1] = 96;
    if (marker) {
        header[1] |= 0x80;
    }

    header[2] = (unsigned char) (g_rtp_sequence >> 8);
    header[3] = (unsigned char) (g_rtp_sequence & 0xFF);
    g_rtp_sequence++;

    header[4] = (unsigned char) ((ts90k >> 24) & 0xFF);
    header[5] = (unsigned char) ((ts90k >> 16) & 0xFF);
    header[6] = (unsigned char) ((ts90k >> 8) & 0xFF);
    header[7] = (unsigned char) (ts90k & 0xFF);
    header[8] = 0;
    header[9] = 0;
    header[10] = 0;
    header[11] = 0;
}

static bool send_rtp_packet(const unsigned char *header, size_t header_len, const unsigned char *payload, size_t payload_len) {
    const size_t packet_len = header_len + payload_len;
    if (packet_len > SCREEN_STREAM_MAX_RTP_PACKET) {
        return false;
    }

    g_send_buffer[0] = '$';
    g_send_buffer[1] = 0;
    g_send_buffer[2] = (unsigned char) ((packet_len >> 8) & 0xFF);
    g_send_buffer[3] = (unsigned char) (packet_len & 0xFF);
    memcpy(g_send_buffer + SCREEN_STREAM_INTERLEAVED_HEADER_SIZE, header, header_len);
    memcpy(g_send_buffer + SCREEN_STREAM_INTERLEAVED_HEADER_SIZE + header_len, payload, payload_len);
    return send_all_locked(g_send_buffer, SCREEN_STREAM_INTERLEAVED_HEADER_SIZE + packet_len,
                           SCREEN_STREAM_RTP_SEND_TIMEOUT_MS, "rtsp client too slow");
}

static s32 find_next_nal_offset(const unsigned char *data, size_t len) {
    for (size_t i = 2; i < len; ++i) {
        if (data[i - 2] == 0 && data[i - 1] == 0 && data[i] == 1) {
            return (s32) (i + 1);
        }
    }
    return -1;
}

static bool packetize_h264_single(const unsigned char *nal, size_t len, u32 ts_ms) {
    unsigned char header[SCREEN_STREAM_RTP_HEADER_SIZE + 2];
    const u32 ts90k = ts_ms * 90u;

    if (len <= SCREEN_STREAM_MAX_RTP_PAYLOAD) {
        prepare_rtp_header(header, ts90k, (nal[0] & 0x1F) <= 5);
        return send_rtp_packet(header, SCREEN_STREAM_RTP_HEADER_SIZE, nal, len);
    }

    header[SCREEN_STREAM_RTP_HEADER_SIZE + 0] = (unsigned char) ((nal[0] & 0xE0) | 28);
    header[SCREEN_STREAM_RTP_HEADER_SIZE + 1] = (unsigned char) ((nal[0] & 0x1F) | 0x80);
    nal++;
    len--;

    while (len > 0) {
        size_t chunk = SCREEN_STREAM_MAX_RTP_PAYLOAD - 2;
        if (len <= chunk) {
            header[SCREEN_STREAM_RTP_HEADER_SIZE + 1] |= 0x40;
            chunk = len;
        }

        prepare_rtp_header(header, ts90k, (header[SCREEN_STREAM_RTP_HEADER_SIZE + 1] & 0x40) != 0);
        if (!send_rtp_packet(header, SCREEN_STREAM_RTP_HEADER_SIZE + 2, nal, chunk)) {
            return false;
        }

        nal += chunk;
        len -= chunk;
        header[SCREEN_STREAM_RTP_HEADER_SIZE + 1] &= 0x1F;
    }

    return true;
}

static bool packetize_h264(const unsigned char *data, size_t len, u32 ts_ms) {
    s32 current = find_next_nal_offset(data, len);
    while (current >= 0) {
        s32 next = find_next_nal_offset(data + current, len - (size_t) current);
        if (next < 0) {
            return packetize_h264_single(data + current, len - (size_t) current, ts_ms);
        }

        next += current;
        s32 current_size = (next - 3) - current;
        if (current_size > 0 && !packetize_h264_single(data + current, (size_t) current_size, ts_ms)) {
            return false;
        }

        current = next;
    }

    return true;
}

static bool parse_cseq(const char *request, int *out_cseq) {
    const char *pos = find_token_case_insensitive(request, "\r\nCSeq:");
    if (!pos) {
        pos = find_token_case_insensitive(request, "CSeq:");
    }
    if (!pos) {
        return false;
    }

    pos += 5;
    while (*pos == ' ' || *pos == '\t') {
        pos++;
    }
    *out_cseq = atoi(pos);
    return *out_cseq > 0;
}

static void extract_rtsp_url(const char *request, char *out, size_t out_size) {
    const char *start = strchr(request, ' ');
    if (!start) {
        out[0] = '\0';
        return;
    }
    start++;
    const char *end = strchr(start, ' ');
    if (!end || end <= start) {
        out[0] = '\0';
        return;
    }

    size_t len = (size_t) (end - start);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
}

static bool send_rtsp_response(int cseq, const char *status, const char *extra_headers,
                               const char *body, const char *content_type, const char *content_base) {
    char response[2048];
    int body_len = body ? (int) strlen(body) : 0;
    int used = snprintf(response, sizeof(response),
                        "RTSP/1.0 %s\r\n"
                        "CSeq: %d\r\n"
                        "Server: switch-ha\r\n",
                        status, cseq);

    if (extra_headers && used > 0 && used < (int) sizeof(response)) {
        used += snprintf(response + used, sizeof(response) - (size_t) used, "%s", extra_headers);
    }
    if (content_base && used > 0 && used < (int) sizeof(response)) {
        used += snprintf(response + used, sizeof(response) - (size_t) used, "Content-Base: %s\r\n", content_base);
    }
    if (body && content_type && used > 0 && used < (int) sizeof(response)) {
        used += snprintf(response + used, sizeof(response) - (size_t) used,
                         "Content-Type: %s\r\nContent-Length: %d\r\n", content_type, body_len);
    }
    if (used > 0 && used < (int) sizeof(response)) {
        used += snprintf(response + used, sizeof(response) - (size_t) used, "\r\n");
    }
    if (body && used > 0 && used < (int) sizeof(response)) {
        used += snprintf(response + used, sizeof(response) - (size_t) used, "%s", body);
    }

    if (used <= 0 || used >= (int) sizeof(response)) {
        return false;
    }
    return send_all_locked(response, (size_t) used, SCREEN_STREAM_CONTROL_TIMEOUT_MS, NULL);
}

static bool handle_rtsp_request(const char *request) {
    int cseq = 1;
    char url[256];

    parse_cseq(request, &cseq);
    extract_rtsp_url(request, url, sizeof(url));
    if (strncmp(request, "OPTIONS", 7) == 0) {
        return send_rtsp_response(cseq, "200 OK", "Public: OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, SET_PARAMETER, TEARDOWN\r\n", NULL, NULL, NULL);
    }
    if (strncmp(request, "DESCRIBE", 8) == 0) {
        return send_rtsp_response(cseq, "200 OK", NULL, g_sdp, "application/sdp", url[0] ? url : NULL);
    }
    if (strncmp(request, "SETUP", 5) == 0) {
        const char *transport = find_token_case_insensitive(request, "Transport:");
        if (!transport || find_token_case_insensitive(transport, "RTP/AVP/TCP") == NULL) {
            return send_rtsp_response(cseq, "461 Unsupported Transport", NULL, NULL, NULL, NULL);
        }
        return send_rtsp_response(cseq, "200 OK", "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\nSession: 1;timeout=60\r\n", NULL, NULL, NULL);
    }
    if (strncmp(request, "PLAY", 4) == 0) {
        video_connected();
        g_client_streaming = true;
        return send_rtsp_response(cseq, "200 OK", "Session: 1;timeout=60\r\nRange: npt=0.000-\r\nRTP-Info: url=streamid=0;seq=1;rtptime=0\r\n", NULL, NULL, NULL);
    }
    if (strncmp(request, "GET_PARAMETER", 13) == 0) {
        return send_rtsp_response(cseq, "200 OK", "Session: 1;timeout=60\r\n", NULL, NULL, NULL);
    }
    if (strncmp(request, "SET_PARAMETER", 13) == 0) {
        return send_rtsp_response(cseq, "200 OK", "Session: 1;timeout=60\r\n", NULL, NULL, NULL);
    }
    if (strncmp(request, "TEARDOWN", 8) == 0) {
        send_rtsp_response(cseq, "200 OK", "Session: 1\r\n", NULL, NULL, NULL);
        return false;
    }

    return send_rtsp_response(cseq, "405 Method Not Allowed", NULL, NULL, NULL, NULL);
}

static void server_thread_main(void *arg) {
    (void) arg;

    while (g_running) {
        while (g_running && g_paused) {
            disconnect_client();
            svcSleepThread(100ULL * 1000ULL * 1000ULL);
        }
        if (!g_running) {
            break;
        }

        int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener < 0) {
            svcSleepThread(1000ULL * 1000ULL * 1000ULL);
            continue;
        }

        int opt = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        set_nonblocking(listener);

        struct sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(SCREEN_STREAM_RTSP_PORT);

        if (bind(listener, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
            close(listener);
            svcSleepThread(1000ULL * 1000ULL * 1000ULL);
            continue;
        }

        mutexLock(&g_client_lock);
        g_listener_fd = listener;
        mutexUnlock(&g_client_lock);
        g_listener_ready = true;
        while (g_running) {
            if (g_paused) {
                break;
            }
            struct pollfd pfd = { .fd = listener, .events = POLLIN };
            int poll_rc = poll(&pfd, 1, 1000);
            if (poll_rc <= 0 || (pfd.revents & POLLIN) == 0) {
                continue;
            }

            int client = accept(listener, NULL, NULL);
            if (client < 0) {
                continue;
            }

            if (g_client_connected) {
                close(client);
                continue;
            }

            set_nonblocking(client);
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

            mutexLock(&g_client_lock);
            g_client_fd = client;
            mutexUnlock(&g_client_lock);

            g_client_connected = true;
            g_client_streaming = false;

            {
                char request[2048];
                size_t used = 0;

                while (g_running && g_client_connected) {
                    if (g_paused) {
                        break;
                    }
                    struct pollfd client_pfd = { .fd = client, .events = POLLIN };
                    int client_poll = poll(&client_pfd, 1, 1000);
                    if (client_poll < 0) {
                        break;
                    }
                    if (client_poll == 0) {
                        continue;
                    }
                    if ((client_pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                        break;
                    }
                    if ((client_pfd.revents & POLLIN) == 0) {
                        continue;
                    }

                    ssize_t received = recv(client, request + used, sizeof(request) - used - 1, 0);
                    if (received <= 0) {
                        if (received < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                            continue;
                        }
                        break;
                    }

                    used += (size_t) received;
                    request[used] = '\0';

                    char *end = strstr(request, "\r\n\r\n");
                    if (!end) {
                        if (used == sizeof(request) - 1) {
                            break;
                        }
                        continue;
                    }

                    end[4] = '\0';
                    if (!handle_rtsp_request(request)) {
                        break;
                    }

                    size_t remaining = used - ((size_t) (end - request) + 4);
                    memmove(request, end + 4, remaining);
                    used = remaining;
                    request[used] = '\0';
                }
            }

            mutexLock(&g_client_lock);
            close_socket(&g_client_fd);
            mutexUnlock(&g_client_lock);
            g_client_connected = false;
            g_client_streaming = false;
        }

        g_listener_ready = false;
        mutexLock(&g_client_lock);
        close_socket(&g_listener_fd);
        mutexUnlock(&g_client_lock);
    }
}

static void video_thread_main(void *arg) {
    (void) arg;

    while (g_running) {
        while (g_running && (!g_client_streaming || g_paused)) {
            svcSleepThread(100ULL * 1000ULL * 1000ULL);
        }
        if (!g_running) {
            break;
        }

        u64 first_ts = 0;
        while (g_running && g_client_streaming) {
            if (g_paused) {
                g_client_streaming = false;
                break;
            }

            if (!capture_read_video()) {
                screen_stream_log("video capture stopped");
                g_client_streaming = false;
                break;
            }

            if (first_ts == 0) {
                first_ts = g_video_packet.header.timestamp;
            }

            if (!packetize_h264(g_video_packet.data, g_video_packet.header.data_size,
                                (u32) ((g_video_packet.header.timestamp - first_ts) / 1000ULL))) {
                screen_stream_log("rtsp packetize/send stopped");
                g_client_streaming = false;
                break;
            }
        }
    }
}

bool screen_stream_start(AppState *state) {
    if (g_running) {
        return true;
    }

    g_state = state;
    mutexInit(&g_begin_mutex);
    mutexInit(&g_client_lock);
    g_paused = false;
    g_grcd_open_result = grcd_service_open(&g_grcd_video);
    if (R_FAILED(g_grcd_open_result)) {
        screen_stream_log("grcd open failed 0x%x", g_grcd_open_result);
        return false;
    }

    g_running = true;

    if (R_FAILED(threadCreate(&g_server_thread, server_thread_main, NULL, NULL, 0x6000, 0x2B, -2))) {
        g_running = false;
        return false;
    }
    if (R_FAILED(threadCreate(&g_video_thread, video_thread_main, NULL, NULL, 0x6000, 0x2B, -2))) {
        g_running = false;
        threadClose(&g_server_thread);
        serviceClose(&g_grcd_video);
        return false;
    }

    threadStart(&g_server_thread);
    threadStart(&g_video_thread);
    return true;
}

void screen_stream_stop(void) {
    g_running = false;
    g_paused = false;
    g_client_streaming = false;
    g_client_connected = false;
    g_listener_ready = false;

    mutexLock(&g_client_lock);
    close_socket(&g_client_fd);
    close_socket(&g_listener_fd);
    mutexUnlock(&g_client_lock);

    if (serviceIsActive(&g_grcd_video)) {
        serviceClose(&g_grcd_video);
    }
}

void screen_stream_set_paused(bool paused) {
    g_paused = paused;
}

u16 screen_stream_port(void) {
    return SCREEN_STREAM_RTSP_PORT;
}

void screen_stream_get_status(char *out, size_t out_size) {
    const char *value = "off";

    if (R_FAILED(g_grcd_open_result)) {
        value = "grcd_failed";
    } else if (g_paused) {
        value = "paused";
    } else if (g_client_streaming) {
        value = "streaming";
    } else if (g_client_connected) {
        value = "connected";
    } else if (g_listener_ready) {
        value = "listening";
    } else if (g_running) {
        value = "starting";
    }

    snprintf(out, out_size, "%s", value);
}

Result screen_stream_get_grcd_open_result(void) {
    return g_grcd_open_result;
}

Result screen_stream_get_grcd_begin_result(void) {
    return g_grcd_begin_result;
}
