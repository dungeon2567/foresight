#include "ecs_common.h"

#define ECS_CRC64_POLY 0xC96C5795D7870F42ULL

uint64_t ecs_crc64_table[256];

void ecs_crc64_init(void) {
    for (int i = 0; i < 256; i++) {
        uint64_t c = (uint64_t)i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ ((c & 1) ? ECS_CRC64_POLY : 0);
        ecs_crc64_table[i] = c;
    }
}
