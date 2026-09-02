#include "application_manifest.h"

#include <furi_hal_version.h>
#include <furi.h>

bool flipper_application_manifest_is_valid(const FlipperApplicationManifest* manifest) {
    furi_check(manifest);

    if((manifest->base.manifest_magic != FAP_MANIFEST_MAGIC) ||
       (manifest->base.manifest_version != FAP_MANIFEST_SUPPORTED_VERSION)) {
        return false;
    }

    return true;
}

/* How many API generations back an app may have been built against.
 *
 * This firmware's API is a strict superset of the official firmware's: every
 * symbol the official release exports is present here with an identical
 * signature (checked against official 87.1 -- 3836 of its 3842 symbols match
 * exactly, none differ, and the six absent ones are the USB CCID smartcard
 * calls, furi_hal_region_init and mf_desfire_send_chunks). An app built
 * against that older API therefore finds everything it needs.
 *
 * That matters because Flipper's own app catalog only publishes builds for
 * official SDKs -- its newest is 87.1 while this firmware reports 88.x -- and
 * the exact-major rule below rejected every one of them. Accepting one
 * generation back lets those apps run, without opening the door to ancient
 * APIs whose signatures have since changed.
 *
 * Anything using one of those six missing symbols still fails, but it fails
 * cleanly at link time rather than misbehaving. */
#define FAP_API_MAJOR_BACK_COMPAT 1

bool flipper_application_manifest_is_too_old(
    const FlipperApplicationManifest* manifest,
    const ElfApiInterface* api_interface) {
    furi_check(manifest);
    furi_check(api_interface);

    uint16_t oldest_accepted = api_interface->api_version_major;
    if(oldest_accepted >= FAP_API_MAJOR_BACK_COMPAT) {
        oldest_accepted -= FAP_API_MAJOR_BACK_COMPAT;
    }

    if(manifest->base.api_version.major < oldest_accepted /* ||
       manifest->base.api_version.minor > app->api_interface->api_version_minor */) {
        return false;
    }

    return true;
}

bool flipper_application_manifest_is_too_new(
    const FlipperApplicationManifest* manifest,
    const ElfApiInterface* api_interface) {
    furi_check(manifest);
    furi_check(api_interface);

    if(manifest->base.api_version.major > api_interface->api_version_major /* ||
       manifest->base.api_version.minor > app->api_interface->api_version_minor */) {
        return false;
    }

    return true;
}

bool flipper_application_manifest_is_target_compatible(const FlipperApplicationManifest* manifest) {
    furi_check(manifest);

    const Version* version = furi_hal_version_get_firmware_version();
    return version_get_target(version) == manifest->base.hardware_target_id;
}