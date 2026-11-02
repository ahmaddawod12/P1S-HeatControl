#include <stddef.h>
#include <string.h>

#include "unity.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char styles_css_start[] asm("_binary_styles_css_start");
extern const char styles_css_end[] asm("_binary_styles_css_end");
extern const char app_js_start[] asm("_binary_app_js_start");
extern const char app_js_end[] asm("_binary_app_js_end");
extern const char printer_js_start[] asm("_binary_printer_js_start");
extern const char printer_js_end[] asm("_binary_printer_js_end");

TEST_CASE("dashboard asset contains required controls", "[web]")
{
    const size_t length = (size_t) (index_html_end - index_html_start);
    TEST_ASSERT_GREATER_THAN_UINT32(200, length);

    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"current-temperature\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"target-temperature\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"enable-button\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"fan-toggle-button\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"fault-panel\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start,"id=\"printer-preheat\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start,"id=\"printer-confirm\""));
}
TEST_CASE("printer script is self-hosted without embedded NUL", "[web]") {
    TEST_ASSERT_NOT_NULL(strstr(printer_js_start,"/api/printer"));
    TEST_ASSERT_NULL(strstr(printer_js_start,"innerHTML"));
    TEST_ASSERT_EQUAL_UINT32((size_t)(printer_js_end-printer_js_start)-1,strlen(printer_js_start));
}

TEST_CASE("dashboard asset is accessible and self-hosted", "[web]")
{
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "<main"));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "aria-live=\"polite\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"target-slider\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"disable-button\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"acknowledge-button\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"heater-badge\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"fan-badge\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "id=\"high-target-warning\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "\"/styles.css\""));
    TEST_ASSERT_NOT_NULL(strstr(index_html_start, "\"/app.js\""));

    /* Loaded resources stay local. Explicit navigation links may use this
     * controller's AP IP or its .local discovery name. */
    TEST_ASSERT_NULL(strstr(index_html_start, "src=\"http"));
    TEST_ASSERT_NULL(strstr(index_html_start, "src=\"//"));
    TEST_ASSERT_NULL(strstr(index_html_start, "https://"));
    const char *link=index_html_start;
    while ((link=strstr(link,"<link"))!=NULL) {
        const char *end=strchr(link,'>');
        TEST_ASSERT_NOT_NULL(end);
        const char *href=strstr(link,"href=\"");
        TEST_ASSERT_TRUE(href!=NULL && href<end);
        href+=6;
        TEST_ASSERT_EQUAL_CHAR('/',href[0]);
        TEST_ASSERT_NOT_EQUAL('/',href[1]);
        link=end+1;
    }
}

TEST_CASE("stylesheet is responsive and motion-aware", "[web]")
{
    const size_t length = (size_t) (styles_css_end - styles_css_start);
    TEST_ASSERT_GREATER_THAN_UINT32(200, length);

    TEST_ASSERT_NOT_NULL(strstr(styles_css_start, "prefers-reduced-motion"));
    TEST_ASSERT_NOT_NULL(strstr(styles_css_start, "@media"));
    TEST_ASSERT_NOT_NULL(strstr(styles_css_start, ":focus-visible"));
}

TEST_CASE("script speaks the documented REST and SSE contract", "[web]")
{
    const size_t length = (size_t) (app_js_end - app_js_start);
    TEST_ASSERT_GREATER_THAN_UINT32(200, length);

    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/status"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "EventSource"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/events"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/target"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/control/enable"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/control/disable"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/control/fan/enable"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/control/fan/disable"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "/api/fault/acknowledge"));

    /* Server-provided text is never parsed as markup. */
    TEST_ASSERT_NULL(strstr(app_js_start, "innerHTML"));
    TEST_ASSERT_NOT_NULL(strstr(app_js_start, "textContent"));
}
