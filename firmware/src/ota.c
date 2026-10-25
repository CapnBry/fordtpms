#include "ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "net.h"

static const char *TAG = "ota";

#define OTA_REBOOT_DELAY_MS  1000  /* between the HTTP reply and the reboot, so the uploader hears the result */
#define OTA_VERIFY_TIMEOUT_S 300   /* probation for a new image: Wi-Fi retries back off to 30 s */
#define OTA_RECV_RETRIES     6     /* httpd receive timeouts (5 s each) in a row before giving up */
#define OTA_BUF_SIZE         4096

static esp_timer_handle_t s_verify_timer;
static volatile bool s_pending_verify;

/* ------------------------------------------------------------ rollback --- */

static void verify_timeout(void *arg)
{
    ESP_LOGE(TAG, "new firmware never brought the web server up: rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    ESP_LOGI(TAG, "running from %s at 0x%06x", run->label, (unsigned)run->address);
    if (esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    s_pending_verify = true;
    if (!net_enabled()) {
        ota_mark_valid();  /* no Wi-Fi to test, and no way to update it over the air either */
        return;
    }
    ESP_LOGW(TAG, "new firmware on probation: rolls back unless the web server is up within %d s",
             OTA_VERIFY_TIMEOUT_S);
    esp_timer_create_args_t ta = {.callback = verify_timeout, .name = "ota_verify"};
    esp_timer_create(&ta, &s_verify_timer);
    esp_timer_start_once(s_verify_timer, OTA_VERIFY_TIMEOUT_S * 1000000ULL);
}

void ota_mark_valid(void)
{
    if (!s_pending_verify) return;
    s_pending_verify = false;
    if (s_verify_timer) esp_timer_stop(s_verify_timer);
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "new firmware confirmed");
}

bool ota_on_probation(void)
{
    return s_pending_verify;
}

/* -------------------------------------------------------------- update --- */

static void reboot(void *arg)
{
    ESP_LOGI(TAG, "rebooting into the new firmware");
    esp_restart();
}

/* Receives up to len bytes, riding out a few receive timeouts on a slow link. */
static int recv_some(httpd_req_t *req, char *buf, size_t len)
{
    for (int tries = 0; tries < OTA_RECV_RETRIES; tries++) {
        int n = httpd_req_recv(req, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return n;
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

/* 400 with the reason. httpd then reads and drops the rest of the body: closing
 * the socket on unread data would reset the connection and lose the reply. */
static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *fmt, ...)
{
    char msg[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", msg);
    if (h) esp_ota_abort(h);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static esp_err_t h_update(httpd_req_t *req)
{
    static char buf[OTA_BUF_SIZE];
    char ctype[128] = "", md5_hex[40] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ctype, sizeof(ctype));  /* truncated is fine */
    /* Multipart: skip the part headers and write the rest. The image is
     * self-delimiting, so the closing boundary after it is never read back. */
    bool multipart = !strncmp(ctype, "multipart/form-data", 19);
    if (multipart || httpd_req_get_hdr_value_str(req, "X-MD5", md5_hex, sizeof(md5_hex)) != ESP_OK ||
        strlen(md5_hex) != 32)
        md5_hex[0] = 0;

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return fail(req, 0, "no OTA partition");
    size_t remaining = req->content_len;
    if (!remaining) return fail(req, 0, "no firmware data received");
    if (!multipart && remaining > part->size)
        return fail(req, 0, "image is %u bytes, %s holds %u", (unsigned)remaining, part->label, (unsigned)part->size);

    char from[48] = "?";
    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);
    if (!getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&peer, &plen)) {
        if (peer.ss_family == AF_INET) inet_ntoa_r(((struct sockaddr_in *)&peer)->sin_addr, from, sizeof(from));
#if CONFIG_LWIP_IPV6
        /* httpd listens on IPv6, so IPv4 peers show up as ::ffff:a.b.c.d */
        if (peer.ss_family == AF_INET6) {
            struct in6_addr *a = &((struct sockaddr_in6 *)&peer)->sin6_addr;
            if (IN6_IS_ADDR_V4MAPPED(a)) inet_ntoa_r(a->un.u32_addr[3], from, sizeof(from));
            else inet6_ntoa_r(*a, from, sizeof(from));
        }
#endif
    }
    ESP_LOGI(TAG, "update started, %u bytes into %s, md5=%s from %s", (unsigned)remaining, part->label,
             md5_hex[0] ? md5_hex : "none", from);

    esp_ota_handle_t h = 0;
    md5_context_t md5;
    esp_rom_md5_init(&md5);
    size_t have = 0, written = 0;
    bool in_headers = multipart;
    int64_t started = esp_timer_get_time();
    while (remaining) {
        size_t want = sizeof(buf) - have < remaining ? sizeof(buf) - have : remaining;
        int n = recv_some(req, buf + have, want);
        if (n <= 0) {
            fail(req, h, "receive failed after %u bytes (%d)", (unsigned)written, n);
            return ESP_FAIL;  /* the connection is gone: close it */
        }
        remaining -= n;
        have += n;
        char *data = buf;
        if (in_headers) {
            char *end = memmem(buf, have, "\r\n\r\n", 4);
            if (!end) {
                if (have == sizeof(buf)) return fail(req, h, "multipart headers too long");
                continue;
            }
            in_headers = false;
            data = end + 4;
        }
        size_t len = have - (data - buf);
        have = 0;
        if (!len) continue;
        if (!h) {
            /* Catch the other board's image now rather than after the whole transfer */
            const esp_image_header_t *ih = (const esp_image_header_t *)data;
            if (len >= sizeof(*ih) && ih->magic == ESP_IMAGE_HEADER_MAGIC && ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
                return fail(req, 0, "image is for chip id %u, this is a %s (%u)", ih->chip_id, CONFIG_IDF_TARGET,
                            CONFIG_IDF_FIRMWARE_CHIP_ID);
            esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
            if (err != ESP_OK) {
                h = 0;
                return fail(req, 0, "begin failed: %s", esp_err_to_name(err));
            }
        }
        esp_err_t err = esp_ota_write(h, data, len);
        if (err != ESP_OK) return fail(req, h, "write failed at %u: %s", (unsigned)written, esp_err_to_name(err));
        esp_rom_md5_update(&md5, data, len);
        written += len;
    }
    if (!h) return fail(req, 0, "no firmware data received");

    if (md5_hex[0]) {
        uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
        char hex[33];
        esp_rom_md5_final(digest, &md5);
        for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++) sprintf(hex + i * 2, "%02x", digest[i]);
        if (strcasecmp(hex, md5_hex)) {
            return fail(req, h, "MD5 mismatch: got %s, expected %s", hex, md5_hex);
        }
    }
    /* Checks the image (chip, segments, SHA-256) before it can be made bootable */
    esp_err_t err = esp_ota_end(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) return fail(req, 0, "image rejected: %s", esp_err_to_name(err));

    ESP_LOGI(TAG, "update complete, %u bytes in %.1f s", (unsigned)written,
             (esp_timer_get_time() - started) / 1e6);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "Update successful, rebooting...");
    static esp_timer_handle_t t;
    esp_timer_create_args_t ta = {.callback = reboot, .name = "ota_reboot"};
    if (t || esp_timer_create(&ta, &t) == ESP_OK) esp_timer_start_once(t, OTA_REBOOT_DELAY_MS * 1000ULL);
    return ESP_OK;
}

void ota_register(httpd_handle_t server)
{
    static const httpd_uri_t uri = {.uri = "/update", .method = HTTP_POST, .handler = h_update};
    httpd_register_uri_handler(server, &uri);
}
