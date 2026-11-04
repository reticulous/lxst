/**
 * lxst_mem.h — where the session's large buffers come from. On the device the
 * session implements these with PSRAM; host tests implement them with malloc.
 */
#pragma once

#include <cstddef>

void* lxstAlloc(size_t n);
void  lxstFree(void* p);
