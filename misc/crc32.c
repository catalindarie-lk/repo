#include <stdio.h>
#include <stdint.h>

#define POLYNOMIAL 0xEDB88320

void generate_crc32_table(uint32_t table[256]) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ POLYNOMIAL;
            else
                crc >>= 1;
        }
        table[i] = crc;
    }
}

int main() {
    uint32_t table[256];
    generate_crc32_table(table);

    printf("uint32_t crc32_table[256] = {\n");
    for (int i = 0; i < 256; i++) {
        printf("    0x%08X%s", table[i], (i < 255) ? "," : "");
        if ((i + 1) % 8 == 0)
            printf("\n");
    }
    printf("};\n");

    return 0;
}