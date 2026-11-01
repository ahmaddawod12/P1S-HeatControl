#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "firmware_policy.h"

static void test_stable_version_order(void)
{
    firmware_version_t a, b;
    assert(firmware_version_parse("v1.0.10", &a));
    assert(firmware_version_parse("1.0.9", &b));
    assert(firmware_version_compare(&a, &b) > 0);
    assert(firmware_version_parse("v1.0.0", &a));
    assert(firmware_version_parse("1.0.0", &b));
    assert(firmware_version_compare(&a, &b) == 0);
}

static void test_rejects_invalid_versions(void)
{
    const char *bad[] = {"", "1.0", "1.0.01", "1.0.0-rc.1", "1.0.0+build",
                         "4294967296.0.0", "vv1.0.0", "1.2.3 ", "-1.2.3"};
    firmware_version_t v = {9, 9, 9};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        assert(!firmware_version_parse(bad[i], &v));
        assert(v.major == 9 && v.minor == 9 && v.patch == 9);
    }
    assert(!firmware_version_parse(NULL, &v));
    assert(!firmware_version_parse("1.0.0", NULL));
}

static void test_identity_requires_exact_hardware_and_layout(void)
{
    firmware_identity_t a = {FIRMWARE_IDENTITY_MAGIC, FIRMWARE_IDENTITY_FORMAT, FIRMWARE_PRODUCT,
                             FIRMWARE_HARDWARE, FIRMWARE_LAYOUT, "1.0.0", 0};
    firmware_identity_t b = a;
    assert(firmware_identity_matches(&a, &b));
    b.image_flags = FIRMWARE_IMAGE_PERSONAL;
    assert(!firmware_identity_matches(&a, &b));
    b = a; b.image_flags = FIRMWARE_IMAGE_CREDENTIALS;
    assert(!firmware_identity_matches(&a, &b));
    b = a;
    strcpy(b.hardware, "other-esp32");
    assert(!firmware_identity_matches(&a, &b));
    b = a;
    strcpy(b.layout, "another-layout");
    assert(!firmware_identity_matches(&a, &b));
    b = a;
    memset(b.version, '1', sizeof(b.version));
    assert(!firmware_identity_matches(&a, &b));
}

int main(void)
{
    test_stable_version_order();
    test_rejects_invalid_versions();
    test_identity_requires_exact_hardware_and_layout();
    puts("firmware policy passed");
    return 0;
}
