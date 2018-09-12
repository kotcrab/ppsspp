#pragma once

#include "Common/CommonTypes.h"
#include <cstdint>

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

namespace SocketDbg
{
void breakpointPaused(u32 addr);

void breakpointLogged(u32 addr);

void startServer();
}
