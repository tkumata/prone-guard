#include "edge_sender.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "mbedtls/platform_util.h"
#include "monitor_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static esp_http_client_handle_t client;
static char destination[MONITOR_URL_MAX + 1];
static size_t response_bytes;
static int64_t deadline;
static char tls_hostname[256];
static struct
{
    char hostname[256];
    ip_addr_t address;
    SemaphoreHandle_t done;
    atomic_bool pending;
    bool found;
} dns_query;

static void resolved(const char *name, const ip_addr_t *address, void *arg)
{
    (void)name;
    (void)arg;
    dns_query.found = address != NULL;
    if (address)
        dns_query.address = *address;
    xSemaphoreGive(dns_query.done);
    atomic_store(&dns_query.pending, false);
}

static void start_dns(void *arg)
{
    (void)arg;
    err_t err = dns_gethostbyname_addrtype(dns_query.hostname, &dns_query.address, resolved, NULL,
                                           LWIP_DNS_ADDRTYPE_DEFAULT);
    if (err == ERR_OK)
        resolved(NULL, &dns_query.address, NULL);
    else if (err != ERR_INPROGRESS)
        resolved(NULL, NULL, NULL);
}

static esp_err_t resolve_destination(const char *url, char *resolved_url, size_t capacity,
                                     char *host_header, size_t header_capacity)
{
    monitor_destination_t target;
    if (!monitor_destination_parse(url, &target))
        return ESP_ERR_INVALID_ARG;
    strlcpy(tls_hostname, target.hostname, sizeof(tls_hostname));
    ip_addr_t address;
    if (!ipaddr_aton(tls_hostname, &address))
    {
        if (atomic_load(&dns_query.pending))
            return ESP_ERR_TIMEOUT;
        if (!dns_query.done)
            dns_query.done = xSemaphoreCreateBinary();
        if (!dns_query.done)
            return ESP_ERR_NO_MEM;
        (void)xSemaphoreTake(dns_query.done, 0);
        strlcpy(dns_query.hostname, tls_hostname, sizeof(dns_query.hostname));
        atomic_store(&dns_query.pending, true);
        if (tcpip_try_callback(start_dns, NULL) != ERR_OK)
        {
            atomic_store(&dns_query.pending, false);
            return ESP_FAIL;
        }
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0 ||
            xSemaphoreTake(dns_query.done, pdMS_TO_TICKS((remaining + 999) / 1000)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        if (!dns_query.found)
            return ESP_ERR_NOT_FOUND;
        address = dns_query.address;
    }
    char ip[64];
    if (!ipaddr_ntoa_r(&address, ip, sizeof(ip)))
        return ESP_FAIL;
    unsigned port = target.port;
    const char *ip_format =
        IP_IS_V6(&address) ? "https://[%s]:%u/v1/measurements" : "https://%s:%u/v1/measurements";
    int length = snprintf(resolved_url, capacity, ip_format, ip, port);
    if (length < 0 || (size_t)length >= capacity)
        return ESP_ERR_INVALID_SIZE;
    length = snprintf(host_header, header_capacity, strchr(tls_hostname, ':') ? "[%s]:%u" : "%s:%u",
                      tls_hostname, port);
    return length > 0 && (size_t)length < header_capacity ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t receive_event(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0)
    {
        response_bytes += event->data_len;
    }
    if ((event->event_id == HTTP_EVENT_ON_DATA || event->event_id == HTTP_EVENT_ON_CONNECTED) &&
        (response_bytes > 1024 || esp_timer_get_time() >= deadline))
    {
        int socket = esp_http_client_get_socket(event->client);
        if (socket >= 0)
            shutdown(socket, SHUT_RDWR);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t edge_sender_send(const char *url, const char *key, const char *jsonl, const char *ca)
{
    if (!key[0] || !url[0])
        return ESP_ERR_INVALID_STATE;
    deadline = esp_timer_get_time() + 5000000;
    if (client && strcmp(destination, url))
    {
        esp_http_client_cleanup(client);
        client = NULL;
    }
    if (!client)
    {
        char resolved_url[MONITOR_URL_MAX + 1], host_header[280];
        esp_err_t err = resolve_destination(url, resolved_url, sizeof(resolved_url), host_header,
                                            sizeof(host_header));
        if (err != ESP_OK)
            return err;
        // Preserve the original TLS identity and HTTP Host while connecting to the resolved IP.
        const esp_http_client_config_t config = {.url = resolved_url,
                                                 .method = HTTP_METHOD_POST,
                                                 .cert_pem = ca,
                                                 .common_name = tls_hostname,
                                                 .timeout_ms = 100,
                                                 .is_async = true,
                                                 .disable_auto_redirect = true,
                                                 .max_authorization_retries = -1,
                                                 .keep_alive_enable = true,
                                                 .event_handler = receive_event};
        client = esp_http_client_init(&config);
        if (!client)
            return ESP_ERR_NO_MEM;
        err = esp_http_client_set_header(client, "Host", host_header);
        if (err != ESP_OK)
        {
            esp_http_client_cleanup(client);
            client = NULL;
            return err;
        }
        strlcpy(destination, url, sizeof(destination));
    }
    char authorization[80];
    snprintf(authorization, sizeof(authorization), "Bearer %s", key);
    esp_err_t err = esp_http_client_set_header(client, "Authorization", authorization);
    mbedtls_platform_zeroize(authorization, sizeof(authorization));
    if (err == ESP_OK)
        err = esp_http_client_set_header(client, "Content-Type", "application/x-ndjson");
    if (err == ESP_OK)
        err = esp_http_client_set_post_field(client, jsonl, strlen(jsonl));
    response_bytes = 0;
    if (err == ESP_OK)
    {
        do
        {
            err = esp_http_client_perform(client);
            if (esp_timer_get_time() >= deadline)
            {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            if (response_bytes > 1024)
            {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (err != ESP_ERR_HTTP_EAGAIN)
                break;
            vTaskDelay(pdMS_TO_TICKS(10));
        } while (true);
    }
    if (err == ESP_OK && esp_http_client_get_status_code(client) != 204)
        err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        client = NULL;
    }
    return err;
}
