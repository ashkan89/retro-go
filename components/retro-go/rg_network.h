#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RG_EVENT_NETWORK_DISCONNECTED (RG_EVENT_TYPE_NETWORK | 1)
#define RG_EVENT_NETWORK_CONNECTED    (RG_EVENT_TYPE_NETWORK | 2)

typedef struct
{
    char ssid[32 + 1];
    char password[64 + 1];
    int channel;
    bool ap_mode;
} rg_wifi_config_t;

typedef enum
{
    RG_NETWORK_DISABLED,
    RG_NETWORK_DISCONNECTED,
    RG_NETWORK_CONNECTING,
    RG_NETWORK_CONNECTED,
} rg_network_state_t;

typedef struct
{
    char name[32 + 1];
    char ip_addr[16];
    int channel, rssi;
    int state;
} rg_network_t;

bool rg_network_init(void);
void rg_network_deinit(void);
bool rg_network_wifi_set_config(const rg_wifi_config_t *config);
bool rg_network_wifi_start(void);
void rg_network_wifi_stop(void);
rg_network_t rg_network_get_info(void);

// Configuration slots management
bool rg_network_wifi_read_config(int slot, rg_wifi_config_t *out);
bool rg_network_wifi_write_config(int slot, const rg_wifi_config_t *config);
bool rg_network_wifi_delete_config(int slot);

typedef struct
{
    const char *name;
    const char *value;
} rg_http_header_t;

typedef struct
{
    int max_redirections;
    int timeout_ms;
    // Size of the buffer the HTTP client reads the socket into. It was hardcoded at 1 KB, which makes
    // a bulk transfer pay the per-read cost (and, over TLS, per-record work) a thousand times per
    // megabyte. 0 means use the default.
    int buffer_size;
    // Perform POST request
    const void *post_data;
    int post_len;
    // Extra request headers, terminated by an entry with a NULL name. Needed for things like
    // Range (partial reads) and Icy-MetaData (shoutcast/icecast titles).
    const rg_http_header_t *headers;
    // Called once per response header. esp_http_client does not retain them, so a caller
    // that needs Content-Type, Accept-Ranges or icy-* has to capture them as they arrive.
    void (*on_header)(const char *name, const char *value, void *arg);
    void *on_header_arg;
} rg_http_cfg_t;

#define RG_HTTP_DEFAULT_CONFIG() \
    {                            \
        .max_redirections = 5,   \
        .timeout_ms = 30000,     \
        .buffer_size = 4096,     \
        .post_data = NULL,       \
        .post_len = 0,           \
        .headers = NULL,         \
        .on_header = NULL,       \
        .on_header_arg = NULL,   \
    }

typedef struct
{
    rg_http_cfg_t config;
    int status_code;
    int content_length;
    int received_bytes;
    int redirections;
    void *client;
} rg_http_req_t;

rg_http_req_t *rg_network_http_open(const char *url, const rg_http_cfg_t *cfg);
int rg_network_http_read(rg_http_req_t *req, void *buffer, size_t buffer_len);
void rg_network_http_close(rg_http_req_t *req);
