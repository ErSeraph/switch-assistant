#include "http_client.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

struct CurlBuffer {
    char data[256];
    size_t used;
};

static size_t on_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
    struct CurlBuffer *buffer = userdata;
    size_t total = size * nmemb;
    size_t remaining = sizeof(buffer->data) - buffer->used - 1;
    size_t to_copy = total < remaining ? total : remaining;
    memcpy(buffer->data + buffer->used, ptr, to_copy);
    buffer->used += to_copy;
    buffer->data[buffer->used] = '\0';
    return total;
}

bool http_validate_home_assistant(AppState *state) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        app_state_push_log(state, "curl init failed");
        app_state_set_status(state, "HTTP init failed");
        return false;
    }

    char url[SHA_MAX_FIELD + 16];
    snprintf(url, sizeof(url), "%s/api/", state->config.ha_url);

    char auth_header[SHA_MAX_FIELD + 32];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", state->config.ha_token);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");

    struct CurlBuffer buffer = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    CURLcode res = curl_easy_perform(curl);
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    bool ok = (res == CURLE_OK && status_code >= 200 && status_code < 300);

    mutexLock(&state->lock);
    state->ha_validated = ok;
    mutexUnlock(&state->lock);

    if (ok) {
        app_state_push_log(state, "Home Assistant OK");
        app_state_set_status(state, "HA token valid");
        return true;
    }

    app_state_push_log(state, "HA validation failed (%ld)", status_code);
    app_state_set_status(state, "HA validation failed");
    return false;
}
