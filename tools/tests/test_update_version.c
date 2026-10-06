#include <assert.h>
#include "../../launcher/main/update_version.h"

int main(void)
{
    assert(update_version_is_newer("v4.5", "4.4"));
    assert(update_version_is_newer("4.10", "v4.9"));
    assert(update_version_is_newer("4.5.1", "4.5"));
    assert(update_version_is_newer("4.5", "4.5-rc.1"));
    assert(update_version_is_newer("4.6", "4.5-3-gabc-dirty"));
    assert(!update_version_is_newer("4.5", "v4.5.0"));
    assert(!update_version_is_newer("4.5", "4.5-3-gabc"));
    assert(!update_version_is_newer("4.5", "4.5-dirty"));
    assert(!update_version_is_newer("4.5", "4.6"));
    assert(!update_version_is_newer("4.5-rc.1", "4.4"));
    assert(!update_version_is_newer("4.5", "unknown"));
    assert(!update_version_is_newer("4.5garbage", "4.4"));
    assert(!update_version_is_newer("42949672960.0", "4.4"));
    assert(!update_version_is_newer("4.5.0.1", "4.4"));
    assert(!update_version_is_newer("4.", "4.4"));
    const char *target = "esp32-s3-n16r8-ili9341";
    assert(update_image_asset_matches("retro-go_4.5_esp32-s3-n16r8-ili9341.img", "4.5", target));
    assert(update_image_asset_matches("retro-go_4.5_esp32-s3-n16r8-ili9341.img", "4.5", "ESP32-S3-N16R8-ILI9341"));
    assert(!update_image_asset_matches("retro-go_4.5_esp32-s3-n8r2-ili9341.img", "4.5", target));
    assert(!update_image_asset_matches("retro-go_4.5_esp32-s3-n16r8-ili9341.fw", "4.5", target));
    assert(!update_image_asset_matches("retro-go_4.5_esp32-s3-n16r8-ili9341.img.sha256", "4.5", target));
    assert(!update_image_asset_matches("retro-go_4.5.1_esp32-s3-n16r8-ili9341.img", "4.5", target));
    assert(!update_image_asset_matches("retro-go_4.5", "4.5", target));
    assert(!update_image_asset_matches(NULL, "4.5", target));
    return 0;
}
