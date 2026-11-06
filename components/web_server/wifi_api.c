#include "wifi_api.h"
#include "wifi_api_policy.h"
#include "firmware_api.h"
#include "http_request.h"
#include "cJSON.h"
#include <string.h>

static web_server_ops_t s_ops;
static esp_err_t respond(httpd_req_t *req,const char *status,const char *body)
{
    httpd_resp_set_status(req,status); httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    return httpd_resp_sendstr(req,body);
}
static esp_err_t get_wifi(httpd_req_t *req)
{
    if (!http_request_require_empty_body(req)) return ESP_FAIL;
    if (!s_ops.get_wifi_status) return respond(req,"503 Service Unavailable","{\"error\":\"unavailable\"}");
    wifi_manager_status_t s; s_ops.get_wifi_status(&s);
    cJSON *root=cJSON_CreateObject();
    if (!root) return respond(req,"503 Service Unavailable","{\"error\":\"memory\"}");
    cJSON_AddStringToObject(root,"ssid",s.ssid);
    cJSON_AddStringToObject(root,"password",s.password);
    cJSON_AddStringToObject(root,"ip_addr",s.ip_addr);
    cJSON_AddNumberToObject(root,"rssi",s.rssi);
    cJSON_AddBoolToObject(root,"configured",s.configured);
    cJSON_AddBoolToObject(root,"connected",s.connected);
    cJSON_AddBoolToObject(root,"applying",s.applying);
    cJSON_AddBoolToObject(root,"can_save",s.can_save);
    cJSON_AddBoolToObject(root,"ap_active",s.ap_active);
    cJSON_AddStringToObject(root,"ap_ssid",s.ap_ssid);
    cJSON_AddStringToObject(root,"ap_ip",s.ap_ip);
    cJSON_AddStringToObject(root,"result",wifi_manager_result_name(s.result));
    cJSON_AddStringToObject(root,"csrf_token",firmware_api_csrf_token());
    char body[1536];
    const bool printed=cJSON_PrintPreallocated(root,body,sizeof(body),false);
    cJSON_Delete(root);
    return printed?respond(req,"200 OK",body):
        respond(req,"503 Service Unavailable","{\"error\":\"memory\"}");
}
static esp_err_t save_wifi(httpd_req_t *req)
{
    if (!firmware_api_request_trusted(req)) return http_request_close_json(req,"403 Forbidden","{\"error\":\"request_origin\"}");
    char type[64];
    const size_t type_length=httpd_req_get_hdr_value_len(req,"Content-Type");
    if (!type_length || type_length>=sizeof(type) ||
        httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type))!=ESP_OK ||
        (strcmp(type,"application/json")!=0 && strncmp(type,"application/json;",17)!=0) ||
        !req->content_len || req->content_len>WIFI_API_MAX_BODY)
        return http_request_close_json(req,"400 Bad Request","{\"error\":\"invalid_body\"}");
    char body[WIFI_API_MAX_BODY+1];
    const esp_err_t read_err=http_request_receive_body(req,body,sizeof(body));
    if (read_err==ESP_ERR_TIMEOUT)
        return http_request_close_json(req,"408 Request Timeout","{\"error\":\"body_timeout\"}");
    if (read_err!=ESP_OK)
        return http_request_close_json(req,"400 Bad Request","{\"error\":\"invalid_body\"}");
    wifi_credentials_t credentials;
    if (!wifi_request_parse(body,req->content_len,&credentials)) return respond(req,"400 Bad Request","{\"error\":\"invalid_body\"}");
    if (!s_ops.request_wifi_change) return respond(req,"503 Service Unavailable","{\"error\":\"unavailable\"}");
    const esp_err_t err=s_ops.request_wifi_change(&credentials);
    if (err==ESP_OK) return respond(req,"202 Accepted","{\"accepted\":true}");
    if (err==ESP_ERR_NOT_ALLOWED) return respond(req,"409 Conflict","{\"error\":\"operation_busy\"}");
    if (err==ESP_ERR_INVALID_ARG) return respond(req,"400 Bad Request","{\"error\":\"invalid_body\"}");
    return respond(req,"503 Service Unavailable","{\"error\":\"unavailable\"}");
}
esp_err_t wifi_api_register(httpd_handle_t server,const web_server_ops_t *ops)
{
    if (!server || !ops) return ESP_ERR_INVALID_ARG;
    s_ops=*ops;
    const httpd_uri_t get={.uri="/api/wifi",.method=HTTP_GET,.handler=get_wifi};
    const httpd_uri_t put={.uri="/api/wifi",.method=HTTP_PUT,.handler=save_wifi};
    const esp_err_t err=httpd_register_uri_handler(server,&get);
    return err==ESP_OK?httpd_register_uri_handler(server,&put):err;
}
