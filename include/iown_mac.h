/**
  * @file    iown_mac.h
  * @author  iown-homecontrol
  * @brief   MAC layer definitions and functions
  *
  * #include <LibraryFile.h>
  * #include "LocalFile.h"
  *
  */

#pragma once
/* Define to prevent recursive inclusion */
#ifdef __cplusplus
  extern "C" {
#endif

#include <stdint.h>
#include <string.h>

/*
 * NOTE: this header must not include iown_frame.h. The two include each other,
 * and `#pragma once` resolves the cycle by expanding this file first - so any
 * macro defined in iown_frame.h is not yet visible here. The MAC-layer sizes
 * therefore live here, and iown_frame.h builds on them.
 */

/** Length of an io-homecontrol node ID in bytes. */
#define IOWN_LEN_NODEID 3

/** Length of the MAC header: destination and source node ID. */
#define IOWN_LEN_HEADER_MAC (IOWN_LEN_NODEID * 2)

/**
 * Broadcast node ID (docs/linklayer.md, "Destination Address").
 *
 * One node ID is IOWN_LEN_NODEID bytes. IOWN_LEN_HEADER_MAC is twice that -
 * it covers the source and destination pair - so sizing a single address with
 * it, as this used to, left three stray zero bytes and made the comparison
 * below read past the caller's address.
 */
extern const uint8_t IOWN_NODEID_BROADCAST[IOWN_LEN_NODEID];

/**
 * Test whether a node ID is the broadcast address.
 *
 * The symbol this expanded to (`iown_broadcast_nodeid`) never existed, so any
 * use of the macro failed to compile; <string.h> was missing too.
 */
#define IOWN_IS_BROADCAST_ADDR(addr) \
  (memcmp((addr), IOWN_NODEID_BROADCAST, IOWN_LEN_NODEID) == 0)


#ifdef __cplusplus
}
#endif
