#pragma once

#include "types.h"

double computeEntropy(const Chunk& data);

double computeSmoothness(const Chunk& data);

Features extractFeatures(const Chunk& data);
