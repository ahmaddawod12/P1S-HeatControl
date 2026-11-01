#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"
#include "wifi_manager.h"

static void fill_ssid(char *buffer, size_t length)
{
    memset(buffer, 'a', length);
    buffer[length] = '\0';
}

TEST_CASE("ssid lengths one through thirty-two are accepted", "[wifi]")
{
    char ssid[34];
    for (size_t length = 1U; length <= 32U; ++length) {
        fill_ssid(ssid, length);
        TEST_ASSERT_TRUE(wifi_credentials_are_valid(ssid, ""));
    }
}

TEST_CASE("empty and oversized ssids are rejected", "[wifi]")
{
    char ssid[34];

    fill_ssid(ssid, 0U);
    TEST_ASSERT_FALSE(wifi_credentials_are_valid(ssid, ""));

    fill_ssid(ssid, 33U);
    TEST_ASSERT_FALSE(wifi_credentials_are_valid(ssid, ""));

    TEST_ASSERT_FALSE(wifi_credentials_are_valid(NULL, ""));
    TEST_ASSERT_FALSE(wifi_credentials_are_valid("home", NULL));
}

TEST_CASE("passwords are empty or eight through sixty-three bytes", "[wifi]")
{
    char password[65];

    TEST_ASSERT_TRUE(wifi_credentials_are_valid("home", ""));

    for (size_t length = 1U; length <= 7U; ++length) {
        fill_ssid(password, length);
        TEST_ASSERT_FALSE(wifi_credentials_are_valid("home", password));
    }

    fill_ssid(password, 8U);
    TEST_ASSERT_TRUE(wifi_credentials_are_valid("home", password));

    fill_ssid(password, 63U);
    TEST_ASSERT_TRUE(wifi_credentials_are_valid("home", password));

    fill_ssid(password, 64U);
    TEST_ASSERT_FALSE(wifi_credentials_are_valid("home", password));
}

TEST_CASE("unterminated fixed-size fields are rejected", "[wifi]")
{
    char ssid[33];
    memset(ssid, 'a', sizeof(ssid));
    TEST_ASSERT_FALSE(wifi_credentials_are_valid_n(ssid, sizeof(ssid), "", 1U));

    char password[64];
    memset(password, 'b', sizeof(password));
    TEST_ASSERT_FALSE(
        wifi_credentials_are_valid_n("home", 5U, password, sizeof(password)));

    TEST_ASSERT_TRUE(wifi_credentials_are_valid_n("home", 33U, "", 64U));
}

TEST_CASE("station credentials are copied only when valid", "[wifi]")
{
    wifi_credentials_t credentials;
    memset(&credentials, 0xa5, sizeof(credentials));

    TEST_ESP_OK(wifi_credentials_init(
        &credentials, "Second floor", "test-password"));
    TEST_ASSERT_EQUAL_STRING("Second floor", credentials.ssid);
    TEST_ASSERT_EQUAL_STRING("test-password", credentials.password);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_credentials_init(&credentials, "", "test-password"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_credentials_init(&credentials, "Second floor", "short"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_credentials_init(NULL, "Second floor", "test-password"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_credentials_init(&credentials, NULL, "test-password"));
}

/* An open network is configured by leaving CONFIG_P1S_WIFI_PASSWORD empty,
 * so the empty password has to survive the copy. */
TEST_CASE("an empty password configures an open network", "[wifi]")
{
    wifi_credentials_t credentials;
    memset(&credentials, 0xa5, sizeof(credentials));

    TEST_ESP_OK(wifi_credentials_init(&credentials, "Second floor", ""));
    TEST_ASSERT_EQUAL_STRING("Second floor", credentials.ssid);
    TEST_ASSERT_EQUAL_STRING("", credentials.password);
}

/* A board built without sdkconfig.secret must fail cleanly rather than try to
 * join a nameless network. Heat regulation is unaffected either way. */
TEST_CASE("an unset compile-time ssid is rejected", "[wifi]")
{
    wifi_credentials_t credentials;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      wifi_credentials_init(&credentials, "", ""));
}

TEST_CASE("Wi-Fi editing preserves leading and trailing spaces", "[wifi]")
{
    wifi_credentials_t credentials;
    TEST_ESP_OK(wifi_credentials_init(&credentials," network "," password with spaces "));
    TEST_ASSERT_EQUAL_STRING(" network ",credentials.ssid);
    TEST_ASSERT_EQUAL_STRING(" password with spaces ",credentials.password);
}

/* The router vanishing must not escalate into a tight retry loop, and the
 * backoff must never stop retrying. */
TEST_CASE("reconnect backoff climbs then holds at thirty seconds", "[wifi]")
{
    TEST_ASSERT_EQUAL_UINT32(1000U, wifi_manager_backoff_ms(0U));
    TEST_ASSERT_EQUAL_UINT32(2000U, wifi_manager_backoff_ms(1U));
    TEST_ASSERT_EQUAL_UINT32(4000U, wifi_manager_backoff_ms(2U));
    TEST_ASSERT_EQUAL_UINT32(8000U, wifi_manager_backoff_ms(3U));
    TEST_ASSERT_EQUAL_UINT32(30000U, wifi_manager_backoff_ms(4U));
    TEST_ASSERT_EQUAL_UINT32(30000U, wifi_manager_backoff_ms(5U));
    TEST_ASSERT_EQUAL_UINT32(30000U, wifi_manager_backoff_ms(1000U));
    TEST_ASSERT_EQUAL_UINT32(30000U, wifi_manager_backoff_ms(SIZE_MAX));
}
