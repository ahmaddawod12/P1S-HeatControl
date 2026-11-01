#include "firmware_manifest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char good[] = "{\"format\":1,\"product\":\"p1s-heat-controller\","
    "\"hardware\":\"esp32-wroom-32\",\"layout\":\"p1s-4m-ota-v1\","
    "\"version\":\"1.0.1\",\"filename\":\"p1s-heat-controller-esp32-4mb.bin\","
    "\"size\":512,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
static const char release[] = "{\"tag_name\":\"v1.0.1\",\"draft\":false,\"prerelease\":false,\"assets\":["
    "{\"name\":\"firmware.json\",\"browser_download_url\":\"https://github.com/owner/repo/releases/download/v1.0.1/firmware.json\"},"
    "{\"name\":\"p1s-heat-controller-esp32-4mb.bin\",\"browser_download_url\":\"https://github.com/owner/repo/releases/download/v1.0.1/p1s-heat-controller-esp32-4mb.bin\"}]}";
int main(void)
{
    firmware_manifest_t manifest;
    assert(firmware_manifest_parse(good, strlen(good), &manifest) == ESP_OK);
    assert(manifest.size == 512 && strcmp(manifest.identity.version, "1.0.1") == 0);
    assert(manifest.sha256[0] == 0 && manifest.sha256[31] == 0);
    const char *bad[] = {"{}", "[]", "<html>error</html>", "{\"format\":1,\"format\":1}",
        "{\"format\":1}trailing", "{\"format\":1,\"version\":\"1.0.0\\u0000x\"}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        assert(firmware_manifest_parse(bad[i], strlen(bad[i]), &manifest) != ESP_OK);
    char changed[sizeof(good)];
    strcpy(changed, good); *strstr(changed, "esp32-wroom") = 'x';
    assert(firmware_manifest_parse(changed, strlen(changed), &manifest) != ESP_OK);
    strcpy(changed, good); *strstr(changed, "0000000000") = 'z';
    assert(firmware_manifest_parse(changed, strlen(changed), &manifest) != ESP_OK);
    strcpy(changed, good); *strstr(changed, "512") = '-';
    assert(firmware_manifest_parse(changed, strlen(changed), &manifest) != ESP_OK);
    firmware_release_t parsed;
    assert(firmware_release_parse(release, strlen(release), &parsed) == ESP_OK);
    assert(strcmp(parsed.version, "1.0.1") == 0);
    assert(firmware_release_parse("{\"tag_name\":\"v1.0.1\",\"draft\":false,\"prerelease\":false,\"assets\":[]}", 75, &parsed) != ESP_OK);
    assert(firmware_release_parse(release, 32769, &parsed) != ESP_OK);
    puts("firmware manifest passed");
    return 0;
}
