#ifndef NETENDIANS_H
#define NETENDIANS_H

#include <stdint.h> // Guaranteed to be available and provides fixed-width integers

// --- 1. Define standard byte order constants if not already defined ---
// These are common values used by GCC/Clang but are not standard C.
#ifndef __ORDER_LITTLE_ENDIAN__
#define __ORDER_LITTLE_ENDIAN__ 1234
#endif
#ifndef __ORDER_BIG_ENDIAN__
#define __ORDER_BIG_ENDIAN__ 4321
#endif
// No PDP_ENDIAN relevant for modern architectures

// --- 2. Determine host byte order at compile time ---
// We try to rely on compiler-specific predefined macros for robustness.
#ifndef __BYTE_ORDER__
#if defined(_WIN32)
    // Windows (MSVC on x86/x64/ARM) is universally little-endian.
    #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#elif defined(__linux__) || defined(__CYGWIN__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
    // For Unix-like systems, GCC/Clang often define __BYTE_ORDER__ directly.
    // If not, we try to deduce from other common macros.
    #if defined(__LITTLE_ENDIAN__) && !defined(__BIG_ENDIAN__)
        #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
    #elif defined(__BIG_ENDIAN__) && !defined(__LITTLE_ENDIAN__)
        #define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
    #elif defined(__BYTE_ORDER) && (__BYTE_ORDER == __LITTLE_ENDIAN) // Common for older glibc systems
        #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
    #elif defined(__BYTE_ORDER) && (__BYTE_ORDER == __BIG_ENDIAN)
        #define __BYTE_ORDER__ __ORDER_BIG_ENDIAN__
    #else
        // Fallback for systems where endianness macros are not cleanly defined.
        // Most modern CPUs (x86/x64/ARM) are little-endian.
        #warning "Could not precisely determine byte order for this platform. Assuming little-endian. Please verify for your target."
        #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
    #endif
#else
    // Generic fallback for any other unknown platform.
    #warning "Could not determine byte order for this platform. Assuming little-endian for _htonll/_ntohll implementation."
    #define __BYTE_ORDER__ __ORDER_LITTLE_ENDIAN__
#endif
#endif // __BYTE_ORDER__

// --- 3. Provide highly optimized byte-swapping functions (compiler intrinsics) ---
// These functions map directly to efficient CPU instructions (e.g., 'bswap').

#if defined(_MSC_VER)
    // For MSVC, declare the intrinsics. They are normally in <intrin.h>, but we declare them here
    // to avoid including any non-stdint.h headers. The compiler knows their signatures.
    // MSVC intrinsics expect specific C types, not always stdint.h types directly in declaration.
    extern uint64_t _byteswap_uint64(uint64_t _Val);
    extern unsigned long _byteswap_ulong(unsigned long _Val);
    extern unsigned short _byteswap_ushort(unsigned short _Val);
    #define LOCAL_BSWAP64(x) _byteswap_uint64(x)
    // Use explicit casts for type safety with MSVC intrinsics
    #define LOCAL_BSWAP32(x) ((uint32_t)_byteswap_ulong((unsigned long)(x)))
    #define LOCAL_BSWAP16(x) ((uint16_t)_byteswap_ushort((unsigned short)(x)))
#elif defined(__GNUC__) || defined(__clang__)
    // For GCC/Clang, use the __builtin_bswap intrinsics.
    // Check for specific builtin existence for maximum portability.
    #if defined(__has_builtin)
        #if __has_builtin(__builtin_bswap64)
            #define LOCAL_BSWAP64(x) __builtin_bswap64(x)
        #else
            #warning "Compiler supports __has_builtin but __builtin_bswap64 not found. Using generic C fallback for 64-bit byte swap."
        #endif
        #if __has_builtin(__builtin_bswap32)
            #define LOCAL_BSWAP32(x) __builtin_bswap32(x)
        #else
            #warning "Compiler supports __has_builtin but __builtin_bswap32 not found. Using generic C fallback for 32-bit byte swap."
        #endif
        #if __has_builtin(__builtin_bswap16)
            #define LOCAL_BSWAP16(x) __builtin_bswap16(x)
        #else
            #warning "Compiler supports __has_builtin but __builtin_bswap16 not found. Using generic C fallback for 16-bit byte swap."
        #endif
    #else // __has_builtin not defined (older GCC/Clang or other compiler)
        #warning "Compiler does not define __has_builtin. Assuming __builtin_bswapX are available for GCC/Clang or falling back to generic C if not."
        // We'll optimistically try to use them, relying on the compiler to error if they don't exist.
        // If compilation fails, the user will know their specific compiler doesn't have them
        // and they are forced to the generic fallback.
        #define LOCAL_BSWAP64(x) __builtin_bswap64(x)
        #define LOCAL_BSWAP32(x) __builtin_bswap32(x)
        #define LOCAL_BSWAP16(x) __builtin_bswap16(x)
    #endif
#endif // Compiler specific intrinsics

// Generic fallback for compilers that don't provide intrinsics or if intrinsics
// were explicitly not found/supported by __has_builtin.
#ifndef LOCAL_BSWAP64
    // Only define if not already defined by an intrinsic block above.
    #if ! (defined(__GNUC__) || defined(__clang__)) || ! defined(__has_builtin) || ! __has_builtin(__builtin_bswap64)
    #warning "Using generic C fallback for 64-bit byte swap (LOCAL_BSWAP64)."
    static inline uint64_t LOCAL_BSWAP64(uint64_t x) {
        return ((x >> 56) & 0x00000000000000FFUL) |
               ((x >> 40) & 0x000000000000FF00UL) |
               ((x >> 24) & 0x0000000000FF0000UL) |
               ((x >>  8) & 0x00000000FF000000UL) |
               ((x <<  8) & 0x000000FF00000000UL) |
               ((x << 24) & 0x0000FF0000000000UL) |
               ((x << 40) & 0x00FF000000000000UL) |
               ((x << 56) & 0xFF00000000000000UL);
    }
    #endif
#endif
#ifndef LOCAL_BSWAP32
    // Only define if not already defined by an intrinsic block above.
    #if ! (defined(__GNUC__) || defined(__clang__)) || ! defined(__has_builtin) || ! __has_builtin(__builtin_bswap32)
    #warning "Using generic C fallback for 32-bit byte swap (LOCAL_BSWAP32)."
    static inline uint32_t LOCAL_BSWAP32(uint32_t x) {
        return ((x >> 24) & 0x000000FFUL) |
               ((x >>  8) & 0x0000FF00UL) |
               ((x <<  8) & 0x00FF0000UL) |
               ((x << 24) & 0xFF000000UL);
    }
    #endif
#endif
#ifndef LOCAL_BSWAP16
    // Only define if not already defined by an intrinsic block above.
    #if ! (defined(__GNUC__) || defined(__clang__)) || ! defined(__has_builtin) || ! __has_builtin(__builtin_bswap16)
    #warning "Using generic C fallback for 16-bit byte swap (LOCAL_BSWAP16)."
    static inline uint16_t LOCAL_BSWAP16(uint16_t x) {
        return ((x >> 8) & 0x00FFU) | ((x << 8) & 0xFF00U);
    }
    #endif
#endif

// --- 4. Host-to-network (big-endian) and network-to-host 64-bit conversions ---

/**
 * @brief Converts a 64-bit unsigned integer from host byte order to network byte order (big-endian).
 * @param val The 64-bit value in host byte order.
 * @return The 64-bit value in network byte order.
 */
static inline uint64_t _htonll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap to big-endian (network order).
    return LOCAL_BSWAP64(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as host order is already network order.
    return val;
#else
    // This branch should ideally not be reached if __BYTE_ORDER__ is properly defined above.
    // As a fallback, assume little-endian (most common) and perform swap.
    #warning "Undefined __BYTE_ORDER__ during _htonll. Assuming little-endian host."
    return LOCAL_BSWAP64(val);
#endif
}

/**
 * @brief Converts a 64-bit unsigned integer from network byte order (big-endian) to host byte order.
 * @param val The 64-bit value in network byte order.
 * @return The 64-bit value in host byte order.
 */
static inline uint64_t _ntohll(uint64_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap from big-endian (network order).
    return LOCAL_BSWAP64(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as network order is already host order.
    return val;
#else
    // This branch should ideally not be reached.
    #warning "Undefined __BYTE_ORDER__ during _ntohll. Assuming little-endian host."
    return LOCAL_BSWAP64(val);
#endif
}

// --- 5. Host-to-network (big-endian) and network-to-host 32-bit conversions ---

/**
 * @brief Converts a 32-bit unsigned integer from host byte order to network byte order (big-endian).
 * @param val The 32-bit value in host byte order.
 * @return The 32-bit value in network byte order.
 */
static inline uint32_t _htonl(uint32_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap to big-endian (network order).
    return LOCAL_BSWAP32(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as host order is already network order.
    return val;
#else
    // Fallback for undefined __BYTE_ORDER__.
    #warning "Undefined __BYTE_ORDER__ during _htonl. Assuming little-endian host."
    return LOCAL_BSWAP32(val);
#endif
}

/**
 * @brief Converts a 32-bit unsigned integer from network byte order (big-endian) to host byte order.
 * @param val The 32-bit value in network byte order.
 * @return The 32-bit value in host byte order.
 */
static inline uint32_t _ntohl(uint32_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap from big-endian (network order).
    return LOCAL_BSWAP32(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as network order is already host order.
    return val;
#else
    // Fallback for undefined __BYTE_ORDER__.
    #warning "Undefined __BYTE_ORDER__ during _ntohl. Assuming little-endian host."
    return LOCAL_BSWAP32(val);
#endif
}

// --- 6. Host-to-network (big-endian) and network-to-host 16-bit conversions ---

/**
 * @brief Converts a 16-bit unsigned integer from host byte order to network byte order (big-endian).
 * @param val The 16-bit value in host byte order.
 * @return The 16-bit value in network byte order.
 */
static inline uint16_t _htons(uint16_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap to big-endian (network order).
    return LOCAL_BSWAP16(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as host order is already network order.
    return val;
#else
    // Fallback for undefined __BYTE_ORDER__.
    #warning "Undefined __BYTE_ORDER__ during _htons. Assuming little-endian host."
    return LOCAL_BSWAP16(val);
#endif
}

/**
 * @brief Converts a 16-bit unsigned integer from network byte order (big-endian) to host byte order.
 * @param val The 16-bit value in network byte order.
 * @return The 16-bit value in host byte order.
 */
static inline uint16_t _ntohs(uint16_t val) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // On a little-endian host, we need to swap from big-endian (network order).
    return LOCAL_BSWAP16(val);
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    // On a big-endian host, no swap is needed as network order is already host order.
    return val;
#else
    // Fallback for undefined __BYTE_ORDER__.
    #warning "Undefined __BYTE_ORDER__ during _ntohs. Assuming little-endian host."
    return LOCAL_BSWAP16(val);
#endif
}

#endif // NETENDIANS_H