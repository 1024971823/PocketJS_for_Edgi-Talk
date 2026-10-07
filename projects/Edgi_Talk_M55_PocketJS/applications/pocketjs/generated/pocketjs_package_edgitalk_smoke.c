#include "pocketjs_package_edgitalk_smoke.h"

extern const uint8_t pocketjs_package_edgitalk_smoke_start[];

const pocketjs_embedded_package_t pocketjs_package_edgitalk_smoke = {
    .data = pocketjs_package_edgitalk_smoke_start,
    .size = 700680U,
};

const pocketjs_package_host_contract_t pocketjs_package_edgitalk_smoke_contract = {
    .struct_size = sizeof(pocketjs_package_edgitalk_smoke_contract),
    .target_id = "edgitalk-m55",
    .host_abi = 1U,
    .tick_hz = 30U,
    .logical_width = 400U,
    .logical_height = 240U,
    .physical_width = 800U,
    .physical_height = 480U,
    .raster_density = 2U,
    .presentation = (pocketjs_presentation_t)3U,
    .profile_hash = { 0x99, 0x70, 0xa6, 0x20, 0xf3, 0x59, 0x7b, 0x8a, 0xe2, 0xfe, 0x55, 0xbe, 0xdd, 0xd0, 0xef, 0x35, 0xa7, 0x29, 0x9d, 0xbd, 0xc2, 0x8f, 0x18, 0x34, 0xc5, 0x12, 0xf1, 0xaf, 0xc4, 0x70, 0xdd, 0xc3 },
};
