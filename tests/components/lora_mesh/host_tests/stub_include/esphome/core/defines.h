#pragma once

// Minimal defines.h for host-side lora_mesh unit tests.
// Shadows esphome/core/defines.h (the static-analysis catch-all that enables
// every feature) via -I ordering, so the component builds with no optional
// integrations (sensors, wifi, radio adapters) on the host.
#define USE_HOST
#define LORA_MESH_HOST_TEST
