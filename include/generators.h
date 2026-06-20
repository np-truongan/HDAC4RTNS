#pragma once

#include "types.h"
#include <random>
#include <cstddef>

Chunk generateTelemetry(size_t size, unsigned seed = 42);
Chunk generateJSON(size_t size);
Chunk generateBinary(size_t size, unsigned seed = 123);
Chunk generateNibble(size_t size, unsigned seed = 77);
Chunk generateBoundaryTelemetry(size_t size, unsigned seed, float noise_scale);
Chunk generateBoundary(size_t size);