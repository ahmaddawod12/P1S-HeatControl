#include "esp_transport_ssl.h"
#include "printer_deadline_transport.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
struct fake_transport {
  void *ctx;
  connect_func connect;
  io_read_func read;
  io_func write;
  trans_func close, destroy;
  poll_func poll_read, poll_write;
};
esp_transport_handle_t esp_transport_init(void) {
  return calloc(1, sizeof(struct fake_transport));
}
esp_transport_handle_t esp_transport_ssl_init(void) {
  return esp_transport_init();
}
esp_err_t esp_transport_destroy(esp_transport_handle_t t) {
  if (t->destroy)
    t->destroy(t);
  free(t);
  return ESP_OK;
}
int esp_transport_connect(esp_transport_handle_t t, const char *h, int p,
                          int ms) {
  return t->connect ? t->connect(t, h, p, ms) : 0;
}
int esp_transport_read(esp_transport_handle_t t, char *b, int n, int ms) {
  return t->read ? t->read(t, b, n, ms) : 0;
}
int esp_transport_write(esp_transport_handle_t t, const char *b, int n,
                        int ms) {
  return t->write ? t->write(t, b, n, ms) : n;
}
int esp_transport_poll_read(esp_transport_handle_t t, int ms) {
  return t->poll_read ? t->poll_read(t, ms) : 1;
}
int esp_transport_poll_write(esp_transport_handle_t t, int ms) {
  return t->poll_write ? t->poll_write(t, ms) : 1;
}
int esp_transport_close(esp_transport_handle_t t) {
  return t->close ? t->close(t) : 0;
}
int esp_transport_get_socket(esp_transport_handle_t t) {
  (void)t;
  return 42;
}
void *esp_transport_get_context_data(esp_transport_handle_t t) {
  return t->ctx;
}
esp_err_t esp_transport_set_context_data(esp_transport_handle_t t, void *ctx) {
  t->ctx = ctx;
  return ESP_OK;
}
esp_err_t esp_transport_set_default_port(esp_transport_handle_t t, int p) {
  (void)t;
  assert(p == 8883);
  return ESP_OK;
}
esp_err_t esp_transport_set_func(esp_transport_handle_t t, connect_func c,
                                 io_read_func r, io_func w, trans_func close,
                                 poll_func pr, poll_func pw, trans_func d) {
  t->connect = c;
  t->read = r;
  t->write = w;
  t->close = close;
  t->poll_read = pr;
  t->poll_write = pw;
  t->destroy = d;
  return ESP_OK;
}
void esp_transport_ssl_set_cert_data(esp_transport_handle_t t, const char *pem,
                                     int n) {
  (void)t;
  assert(n > 0 && strstr(pem, "BEGIN CERTIFICATE"));
}
void esp_transport_ssl_set_common_name(esp_transport_handle_t t,
                                       const char *name) {
  (void)t;
  assert(!strcmp(name, "SERIAL123"));
}
bool printer_socket_nonblocking(int fd) {
  assert(fd == 42);
  return true;
}
