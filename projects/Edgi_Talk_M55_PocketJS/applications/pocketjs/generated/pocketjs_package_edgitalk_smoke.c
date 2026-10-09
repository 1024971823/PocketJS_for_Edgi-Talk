#include "pocketjs_package_edgitalk_smoke.h"

extern const uint8_t pocketjs_package_edgitalk_smoke_start[];

const pocketjs_embedded_package_t pocketjs_package_edgitalk_smoke = {
    .data = pocketjs_package_edgitalk_smoke_start,
    .size = 2072424U,
};

const pocketjs_package_host_contract_t pocketjs_package_edgitalk_smoke_contract = {
    .struct_size = sizeof(pocketjs_package_edgitalk_smoke_contract),
    .target_id = "edgitalk-m55",
    .host_abi = 1U,
    .tick_hz = 60U,
    .logical_width = 400U,
    .logical_height = 240U,
    .physical_width = 800U,
    .physical_height = 480U,
    .raster_density = 2U,
    .presentation = (pocketjs_presentation_t)3U,
    .profile_hash = { 0xeb, 0x52, 0xdf, 0x1d, 0x40, 0x0d, 0xe2, 0xb6, 0x69, 0xf6, 0x94, 0xa4, 0x25, 0x8d, 0x39, 0xff, 0xc6, 0x83, 0x45, 0x6b, 0xe1, 0x0f, 0xd3, 0x2d, 0x05, 0x2b, 0x38, 0xbd, 0xeb, 0xec, 0x00, 0x16 },
};
