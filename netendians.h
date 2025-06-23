#ifndef NETENDIANS_H
#define NETENDIANS_H

// #include <stdint.h>

// #if defined(_WIN32)
//     #include <winsock2.h>
// #elif defined(__linux__)
//     #include <arpa/inet.h>
// #else
//     #error "Unsupported platform"
// #endif

// static inline uint64_t htonll(uint64_t val) {
// #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
//     return ((uint64_t)htonl((uint32_t)(val >> 32)) |
//            ((uint64_t)htonl((uint32_t)(val & 0xFFFFFFFF)) << 32));
// #else
//     return val;
// #endif
// }

// static inline uint64_t ntohll(uint64_t val) {
// #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
//     return ((uint64_t)ntohl((uint32_t)(val >> 32)) |
//            ((uint64_t)ntohl((uint32_t)(val & 0xFFFFFFFF)) << 32));
// #else
//     return val;
// #endif
// }

#endif // NETENDIANS_H