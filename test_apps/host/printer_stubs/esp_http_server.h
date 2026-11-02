#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef void *httpd_handle_t;
typedef enum { HTTP_GET, HTTP_PUT, HTTP_DELETE, HTTP_POST } httpd_method_t;
typedef struct httpd_req {
  size_t content_len;
  httpd_method_t method;
  void *user_ctx;
} httpd_req_t;
typedef struct {
  const char *uri;
  httpd_method_t method;
  esp_err_t (*handler)(httpd_req_t *);
  void *user_ctx;
} httpd_uri_t;
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t *);
esp_err_t httpd_resp_set_status(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_type(httpd_req_t *, const char *);
esp_err_t httpd_resp_set_hdr(httpd_req_t *, const char *, const char *);
esp_err_t httpd_resp_sendstr(httpd_req_t *, const char *);
esp_err_t httpd_resp_send(httpd_req_t *, const char *, int);
size_t httpd_req_get_hdr_value_len(httpd_req_t *, const char *);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *, const char *, char *,
                                      size_t);
int httpd_req_recv(httpd_req_t *, char *, size_t);
