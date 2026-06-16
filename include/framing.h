#pragma once

#include "types.h"
#include <cstddef>

bool sendFrame(int fd,
               const FrameHeader& header,
               const Chunk& payload);


bool recvFrame(int fd,
               FrameHeader& header,
               Chunk& payload);

uint64_t nowMicros();