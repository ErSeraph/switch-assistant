#include <switch.h>
#include <switch/display/framebuffer.h>
#include <switch/display/native_window.h>
#include <switch/services/vi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define NOTIFICATION_PATH "sdmc:/switch/switch-ha/notification-current.ini"
#define LOG_PATH "sdmc:/switch/switch-ha/overlay.log"
#define STATUS_PATH "sdmc:/switch/switch-ha/overlay-status.ini"
#define LOG_MAX_BYTES 8192
#define LAYER_W 1280
#define LAYER_H 720
#define SCREEN_W 640
#define SCREEN_H 360
#define BANNER_MARGIN 28
#define BANNER_H 92
#define GRAPHICS_BOOT_DELAY_MS 25000

extern u64 __nx_vi_layer_id;

static ViDisplay g_display;
static ViLayer g_layer;
static NWindow g_window;
static Framebuffer g_framebuffer;
static Event g_vsync_event;
static bool g_graphics_ready = false;
static bool g_fs_ready = false;
static bool g_sdmc_ready = false;
static bool g_sm_ready = false;

static void exit_graphics(void);
static void append_log(const char *message, Result rc);
static void write_status(const char *state, Result rc, const char *detail);

u32 __nx_applet_type = AppletType_None;
u32 __nx_nv_transfermem_size = 0x40000;

typedef struct {
    char id[32];
    char mode[16];
    char title[96];
    char message[512];
    int duration_ms;
} Notification;

static u64 monotonic_ms(void) {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) {
        fatalThrow(rc);
    }
    g_sm_ready = true;

    rc = fsInitialize();
    if (R_FAILED(rc)) {
        fatalThrow(rc);
    }
    g_fs_ready = true;

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        fatalThrow(rc);
    }
    g_sdmc_ready = true;

}

void __appExit(void) {
    if (g_graphics_ready) {
        exit_graphics();
    }
    if (g_sdmc_ready) {
        fsdevUnmountAll();
    }
    if (g_fs_ready) {
        fsExit();
    }
    if (g_sm_ready) {
        smExit();
    }
}

static void append_log(const char *message, Result rc) {
    FILE *check = fopen(LOG_PATH, "rb");
    if (check) {
        if (fseek(check, 0, SEEK_END) == 0 && ftell(check) > LOG_MAX_BYTES) {
            fclose(check);
            check = fopen(LOG_PATH, "w");
            if (check) {
                fclose(check);
            }
        } else {
            fclose(check);
        }
    }

    FILE *file = fopen(LOG_PATH, "a");
    if (!file) {
        return;
    }
    fprintf(file, "%llu %s rc=0x%x\n", (unsigned long long) monotonic_ms(), message, rc);
    fclose(file);
}

static void write_status(const char *state, Result rc, const char *detail) {
    FILE *file = fopen(STATUS_PATH, "w");
    if (!file) {
        return;
    }
    fprintf(file, "uptime_ms=%llu\n", (unsigned long long) monotonic_ms());
    fprintf(file, "state=%s\n", state);
    fprintf(file, "result=0x%x\n", rc);
    fprintf(file, "detail=%s\n", detail ? detail : "");
    fclose(file);
}

static Result vi_add_to_layer_stack(ViLayer *layer, ViLayerStack stack) {
    const struct {
        u32 stack;
        u64 layer_id;
    } in = {stack, layer->layer_id};

    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

static void trim_eol(char *value) {
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r')) {
        value[--len] = '\0';
    }
}

static bool utf8_next(const char **ptr, u32 *cp) {
    const unsigned char *s = (const unsigned char *) *ptr;
    if (s[0] == '\0') {
        return false;
    }

    if (s[0] < 0x80) {
        *cp = s[0];
        *ptr += 1;
        return true;
    }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((u32) (s[0] & 0x1F) << 6) | (u32) (s[1] & 0x3F);
        *ptr += 2;
        return true;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((u32) (s[0] & 0x0F) << 12) | ((u32) (s[1] & 0x3F) << 6) | (u32) (s[2] & 0x3F);
        *ptr += 3;
        return true;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((u32) (s[0] & 0x07) << 18) | ((u32) (s[1] & 0x3F) << 12) | ((u32) (s[2] & 0x3F) << 6) | (u32) (s[3] & 0x3F);
        *ptr += 4;
        return true;
    }

    *cp = '?';
    *ptr += 1;
    return true;
}

static void append_ascii(char *out, size_t out_size, size_t *used, const char *text) {
    while (*text && *used + 1 < out_size) {
        out[(*used)++] = *text++;
    }
}

static const char *latin_ascii(u32 cp) {
    switch (cp) {
        case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5: case 0x0100: case 0x0102: case 0x0104: return "A";
        case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5: case 0x0101: case 0x0103: case 0x0105: return "a";
        case 0x00C7: case 0x0106: case 0x0108: case 0x010A: case 0x010C: return "C";
        case 0x00E7: case 0x0107: case 0x0109: case 0x010B: case 0x010D: return "c";
        case 0x010E: case 0x0110: return "D";
        case 0x010F: case 0x0111: return "d";
        case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB: case 0x0112: case 0x0114: case 0x0116: case 0x0118: case 0x011A: return "E";
        case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB: case 0x0113: case 0x0115: case 0x0117: case 0x0119: case 0x011B: return "e";
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: case 0x0128: case 0x012A: case 0x012C: case 0x012E: return "I";
        case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF: case 0x0129: case 0x012B: case 0x012D: case 0x012F: return "i";
        case 0x00D1: case 0x0143: case 0x0147: return "N";
        case 0x00F1: case 0x0144: case 0x0148: return "n";
        case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8: case 0x014C: case 0x014E: return "O";
        case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8: case 0x014D: case 0x014F: return "o";
        case 0x0160: return "S";
        case 0x0161: return "s";
        case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC: case 0x0168: case 0x016A: case 0x016C: case 0x016E: return "U";
        case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC: case 0x0169: case 0x016B: case 0x016D: case 0x016F: return "u";
        case 0x00DD: case 0x0178: return "Y";
        case 0x00FD: case 0x00FF: return "y";
        case 0x017D: return "Z";
        case 0x017E: return "z";
        case 0x00C6: return "AE";
        case 0x00E6: return "ae";
        case 0x00DF: return "ss";
        default: return NULL;
    }
}

static const char *cyrillic_ascii(u32 cp) {
    switch (cp) {
        case 0x0410: return "A"; case 0x0430: return "a";
        case 0x0411: return "B"; case 0x0431: return "b";
        case 0x0412: return "V"; case 0x0432: return "v";
        case 0x0413: return "G"; case 0x0433: return "g";
        case 0x0414: return "D"; case 0x0434: return "d";
        case 0x0415: case 0x0401: return "E"; case 0x0435: case 0x0451: return "e";
        case 0x0416: return "Zh"; case 0x0436: return "zh";
        case 0x0417: return "Z"; case 0x0437: return "z";
        case 0x0418: return "I"; case 0x0438: return "i";
        case 0x0419: return "Y"; case 0x0439: return "y";
        case 0x041A: return "K"; case 0x043A: return "k";
        case 0x041B: return "L"; case 0x043B: return "l";
        case 0x041C: return "M"; case 0x043C: return "m";
        case 0x041D: return "N"; case 0x043D: return "n";
        case 0x041E: return "O"; case 0x043E: return "o";
        case 0x041F: return "P"; case 0x043F: return "p";
        case 0x0420: return "R"; case 0x0440: return "r";
        case 0x0421: return "S"; case 0x0441: return "s";
        case 0x0422: return "T"; case 0x0442: return "t";
        case 0x0423: return "U"; case 0x0443: return "u";
        case 0x0424: return "F"; case 0x0444: return "f";
        case 0x0425: return "Kh"; case 0x0445: return "kh";
        case 0x0426: return "Ts"; case 0x0446: return "ts";
        case 0x0427: return "Ch"; case 0x0447: return "ch";
        case 0x0428: return "Sh"; case 0x0448: return "sh";
        case 0x0429: return "Shch"; case 0x0449: return "shch";
        case 0x042A: case 0x044A: return "";
        case 0x042B: return "Y"; case 0x044B: return "y";
        case 0x042C: case 0x044C: return "";
        case 0x042D: return "E"; case 0x044D: return "e";
        case 0x042E: return "Yu"; case 0x044E: return "yu";
        case 0x042F: return "Ya"; case 0x044F: return "ya";
        default: return NULL;
    }
}

static void normalize_display_text(char *value, size_t value_size) {
    char normalized[512];
    size_t used = 0;
    const char *ptr = value;
    u32 cp = 0;

    while (utf8_next(&ptr, &cp) && used + 1 < sizeof(normalized)) {
        if (cp == '\n' || cp == '\r' || cp == '\t') {
            append_ascii(normalized, sizeof(normalized), &used, " ");
        } else if (cp >= 0x20 && cp <= 0x7E) {
            char ch[2] = {(char) cp, '\0'};
            append_ascii(normalized, sizeof(normalized), &used, ch);
        } else {
            const char *mapped = latin_ascii(cp);
            if (!mapped) mapped = cyrillic_ascii(cp);
            if (mapped) {
                append_ascii(normalized, sizeof(normalized), &used, mapped);
            } else if (cp == 0x2018 || cp == 0x2019) {
                append_ascii(normalized, sizeof(normalized), &used, "'");
            } else if (cp == 0x201C || cp == 0x201D) {
                append_ascii(normalized, sizeof(normalized), &used, "\"");
            } else if (cp == 0x2013 || cp == 0x2014) {
                append_ascii(normalized, sizeof(normalized), &used, "-");
            } else if (cp == 0x2026) {
                append_ascii(normalized, sizeof(normalized), &used, "...");
            } else {
                append_ascii(normalized, sizeof(normalized), &used, "?");
            }
        }
    }

    normalized[used] = '\0';
    snprintf(value, value_size, "%s", normalized);
}

static bool read_notification(Notification *notification) {
    FILE *file = fopen(NOTIFICATION_PATH, "r");
    if (!file) {
        return false;
    }

    Notification current = {0};
    snprintf(current.title, sizeof(current.title), "Home Assistant");
    snprintf(current.mode, sizeof(current.mode), "popup");
    current.duration_ms = 4500;

    char line[768];
    while (fgets(line, sizeof(line), file)) {
        trim_eol(line);
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq++ = '\0';

        if (strcmp(line, "id") == 0) {
            snprintf(current.id, sizeof(current.id), "%s", eq);
        } else if (strcmp(line, "mode") == 0) {
            snprintf(current.mode, sizeof(current.mode), "%s", eq);
        } else if (strcmp(line, "title") == 0) {
            snprintf(current.title, sizeof(current.title), "%s", eq);
        } else if (strcmp(line, "message") == 0) {
            snprintf(current.message, sizeof(current.message), "%s", eq);
        } else if (strcmp(line, "duration_ms") == 0) {
            current.duration_ms = atoi(eq);
        }
    }

    fclose(file);
    if (current.id[0] == '\0' || current.message[0] == '\0') {
        return false;
    }

    normalize_display_text(current.title, sizeof(current.title));
    normalize_display_text(current.message, sizeof(current.message));

    *notification = current;
    return true;
}

static void consume_notification_file(void) {
    if (remove(NOTIFICATION_PATH) != 0 && errno != ENOENT) {
        append_log("notification consume failed", errno);
    }
}

static Result init_graphics(void) {
    Result rc = 0;
    append_log("graphics init begin", 0);
    write_status("graphics_initializing", 0, "viInitialize");
    rc = viInitialize(ViServiceType_Manager);
    append_log("viInitialize", rc);
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viInitialize");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viOpenDefaultDisplay");
        rc = viOpenDefaultDisplay(&g_display);
        append_log("viOpenDefaultDisplay", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viOpenDefaultDisplay");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viGetDisplayVsyncEvent");
        rc = viGetDisplayVsyncEvent(&g_display, &g_vsync_event);
        append_log("viGetDisplayVsyncEvent", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viGetDisplayVsyncEvent");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viCreateManagedLayer");
        rc = viCreateManagedLayer(&g_display, (ViLayerFlags) 0, 0, &__nx_vi_layer_id);
        append_log("viCreateManagedLayer", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viCreateManagedLayer");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viCreateLayer");
        rc = viCreateLayer(&g_display, &g_layer);
        append_log("viCreateLayer", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viCreateLayer");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viSetLayerScalingMode");
        rc = viSetLayerScalingMode(&g_layer, ViScalingMode_FitToLayer);
        append_log("viSetLayerScalingMode", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viSetLayerScalingMode");
    if (R_SUCCEEDED(rc)) {
        s32 z = 0;
        Result z_rc = viGetZOrderCountMax(&g_display, &z);
        append_log("viGetZOrderCountMax", z_rc);
        if (R_SUCCEEDED(z_rc) && z > 0) {
            Result z_set_rc = viSetLayerZ(&g_layer, z);
            append_log("viSetLayerZ", z_set_rc);
        }
        append_log("viAddToLayerStack Default", vi_add_to_layer_stack(&g_layer, ViLayerStack_Default));
        append_log("viAddToLayerStack Screenshot", vi_add_to_layer_stack(&g_layer, ViLayerStack_Screenshot));
        append_log("viAddToLayerStack Recording", vi_add_to_layer_stack(&g_layer, ViLayerStack_Recording));
        append_log("viAddToLayerStack Arbitrary", vi_add_to_layer_stack(&g_layer, ViLayerStack_Arbitrary));
        append_log("viAddToLayerStack LastFrame", vi_add_to_layer_stack(&g_layer, ViLayerStack_LastFrame));
        append_log("viAddToLayerStack Null", vi_add_to_layer_stack(&g_layer, ViLayerStack_Null));
        append_log("viAddToLayerStack ApplicationForDebug", vi_add_to_layer_stack(&g_layer, ViLayerStack_ApplicationForDebug));
        append_log("viAddToLayerStack Lcd", vi_add_to_layer_stack(&g_layer, ViLayerStack_Lcd));
    }
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viSetLayerSize");
        rc = viSetLayerSize(&g_layer, LAYER_W, LAYER_H);
        append_log("viSetLayerSize", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viSetLayerSize");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "viSetLayerPosition");
        rc = viSetLayerPosition(&g_layer, 0, 0);
        append_log("viSetLayerPosition", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "viSetLayerPosition");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "nwindowCreateFromLayer");
        rc = nwindowCreateFromLayer(&g_window, &g_layer);
        append_log("nwindowCreateFromLayer", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "nwindowCreateFromLayer");
    if (R_SUCCEEDED(rc)) {
        write_status("graphics_initializing", 0, "framebufferCreate");
        rc = framebufferCreate(&g_framebuffer, &g_window, SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_4444, 2);
        append_log("framebufferCreate", rc);
    }
    if (R_FAILED(rc)) write_status("graphics_failed", rc, "framebufferCreate");

    g_graphics_ready = R_SUCCEEDED(rc);
    append_log(g_graphics_ready ? "graphics ready" : "graphics failed", rc);
    if (g_graphics_ready) {
        write_status("ready", rc, "graphics ready");
    }
    return rc;
}

static void exit_graphics(void) {
    if (!g_graphics_ready) {
        return;
    }
    framebufferClose(&g_framebuffer);
    nwindowClose(&g_window);
    viDestroyManagedLayer(&g_layer);
    viCloseDisplay(&g_display);
    eventClose(&g_vsync_event);
    viExit();
}

static void clear_frame(void) {
    u32 stride = 0;
    u16 *pixels = framebufferBegin(&g_framebuffer, &stride);
    memset(pixels, 0, g_framebuffer.fb_size);
    eventWait(&g_vsync_event, 20ULL * 1000ULL * 1000ULL);
    framebufferEnd(&g_framebuffer);
}

static u32 swizzled_pixel_offset(int x, int y) {
    u32 tmp = ((y & 127) / 16) + (x / 32 * 8) + ((y / 16 / 8) * (((SCREEN_W / 2) / 16 * 8)));
    tmp *= 16 * 16 * 4;
    tmp += ((y % 16) / 8) * 512 + ((x % 32) / 16) * 256 + ((y % 8) / 2) * 64 + ((x % 16) / 8) * 32 + (y % 2) * 16 + (x % 8) * 2;
    return tmp / 2;
}

static u16 rgba4_from_8(u8 r, u8 g, u8 b, u8 a) {
    return RGBA4_FROM_RGBA8(r, g, b, a);
}

static void draw_rect(u16 *pixels, int x, int y, int w, int h, u16 color) {
    for (int yy = 0; yy < h; ++yy) {
        int py = y + yy;
        if (py < 0 || py >= SCREEN_H) continue;
        for (int xx = 0; xx < w; ++xx) {
            int px = x + xx;
            if (px < 0 || px >= SCREEN_W) continue;
            pixels[swizzled_pixel_offset(px, py)] = color;
        }
    }
}

static const u8 FONT_DIGITS[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},{0x18,0x14,0x12,0x7f,0x10},
    {0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e},
};

static const u8 FONT_UPPER[26][5] = {
    {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},
    {0x7f,0x09,0x09,0x09,0x01},{0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},{0x00,0x41,0x7f,0x41,0x00},{0x20,0x40,0x41,0x3f,0x01},
    {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},{0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
    {0x7f,0x09,0x09,0x09,0x06},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
    {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
};

static const u8 FONT_LOWER[26][5] = {
    {0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7e,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3c},{0x7f,0x08,0x04,0x04,0x78},{0x00,0x44,0x7d,0x40,0x00},{0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00},{0x00,0x41,0x7f,0x40,0x00},{0x7c,0x04,0x18,0x04,0x78},{0x7c,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7c,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7c},{0x7c,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},{0x04,0x3f,0x44,0x40,0x20},
    {0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},{0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},
};

static const u8 *glyph_for(char ch, u8 scratch[5]) {
    if (ch >= 'A' && ch <= 'Z') return FONT_UPPER[ch - 'A'];
    if (ch >= 'a' && ch <= 'z') return FONT_LOWER[ch - 'a'];
    if (ch >= '0' && ch <= '9') return FONT_DIGITS[ch - '0'];
    memset(scratch, 0, 5);
    switch (ch) {
        case '.': scratch[2] = 0x60; break;
        case ',': scratch[2] = 0xa0; break;
        case ':': scratch[2] = 0x36; break;
        case '!': scratch[2] = 0x5f; break;
        case '?': scratch[1] = 0x01; scratch[2] = 0x51; scratch[3] = 0x09; scratch[4] = 0x06; break;
        case '-': scratch[1] = 0x08; scratch[2] = 0x08; scratch[3] = 0x08; break;
        case '/': scratch[0] = 0x20; scratch[1] = 0x10; scratch[2] = 0x08; scratch[3] = 0x04; scratch[4] = 0x02; break;
        case '_': scratch[0] = 0x40; scratch[1] = 0x40; scratch[2] = 0x40; scratch[3] = 0x40; scratch[4] = 0x40; break;
        case '\'': scratch[2] = 0x03; break;
        case '"': scratch[1] = 0x03; scratch[3] = 0x03; break;
        case '(': scratch[2] = 0x3e; scratch[3] = 0x41; break;
        case ')': scratch[1] = 0x41; scratch[2] = 0x3e; break;
        default: break;
    }
    return scratch;
}

static void draw_char(u16 *pixels, int x, int y, char ch, int scale, u16 color) {
    u8 scratch[5];
    const u8 *glyph = glyph_for(ch, scratch);
    for (int col = 0; col < 5; ++col) {
        for (int row = 0; row < 7; ++row) {
            if (glyph[col] & (1 << row)) {
                draw_rect(pixels, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void draw_text(u16 *pixels, int x, int y, const char *text, int scale, int max_chars) {
    int cx = x;
    int cy = y;
    int start_x = x;
    int line_h = 9 * scale;
    u16 text_color = rgba4_from_8(245, 250, 252, 255);
    for (int i = 0; text[i] && i < max_chars; ++i) {
        if (text[i] == '\n' || cx + 6 * scale > SCREEN_W - BANNER_MARGIN - 14) {
            cx = start_x;
            cy += line_h;
            if (cy > SCREEN_H - BANNER_MARGIN - line_h) break;
            if (text[i] == '\n') continue;
        }
        draw_char(pixels, cx, cy, text[i], scale, text_color);
        cx += 6 * scale;
    }
}

static void render_notification(const Notification *notification, u64 started_ms) {
    (void) started_ms;

    u32 stride = 0;
    u16 *pixels = framebufferBegin(&g_framebuffer, &stride);
    memset(pixels, 0, g_framebuffer.fb_size);

    int x = BANNER_MARGIN;
    int y = BANNER_MARGIN;
    int w = SCREEN_W - (BANNER_MARGIN * 2);
    int h = BANNER_H;

    draw_rect(pixels, x, y, w, h, rgba4_from_8(14, 17, 19, 230));
    draw_rect(pixels, x, y, w, 5, rgba4_from_8(0, 188, 212, 255));
    draw_text(pixels, x + 18, y + 18, notification->title, 2, 42);
    draw_text(pixels, x + 18, y + 50, notification->message, 2, 120);

    eventWait(&g_vsync_event, 20ULL * 1000ULL * 1000ULL);
    framebufferEnd(&g_framebuffer);
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;

    append_log("overlay main", 0);
    write_status("starting", 0, "main");
    append_log("boot delay begin", 0);
    write_status("waiting_for_home", 0, "boot delay before vi");
    svcSleepThread(GRAPHICS_BOOT_DELAY_MS * 1000ULL * 1000ULL);
    append_log("boot delay end", 0);

    Result rc = init_graphics();
    if (R_FAILED(rc)) {
        while (true) {
            svcSleepThread(5ULL * 1000ULL * 1000ULL * 1000ULL);
        }
    }

    char last_id[32] = {0};
    Notification active = {0};
    bool showing = false;
    u64 show_started_ms = 0;

    while (true) {
        Notification current;
        if (read_notification(&current) && strcmp(current.id, last_id) != 0) {
            active = current;
            snprintf(last_id, sizeof(last_id), "%s", current.id);
            consume_notification_file();
            showing = true;
            show_started_ms = monotonic_ms();
            append_log("notification displayed", 0);
            write_status("displaying", 0, active.id);
        }

        if (showing) {
            u64 elapsed = monotonic_ms() - show_started_ms;
            if (elapsed < (u64) active.duration_ms) {
                render_notification(&active, show_started_ms);
            } else {
                clear_frame();
                showing = false;
                write_status("ready", 0, "idle");
            }
        } else {
            clear_frame();
            svcSleepThread(200ULL * 1000ULL * 1000ULL);
        }
    }

    exit_graphics();
    return 0;
}
