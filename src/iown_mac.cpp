/**
  * @file    iown_mac.cpp
  * @author  iown-homecontrol
  * @brief   MAC header and NodeID
  *
  * #include <LibraryFile.h>
  * #include "LocalFile.h"
  *
  */

#include "iown.h"

/**
 * Broadcast node ID, per docs/linklayer.md "Destination Address".
 *
 * Declared non-static and exported: as a file-local static it was unreferenced
 * dead weight that every build warned about. It was also sized with
 * IOWN_LEN_HEADER_MAC, which is two node IDs, not one.
 */
const uint8_t IOWN_NODEID_BROADCAST[IOWN_LEN_NODEID] = { 0x00, 0x00, 0x3F };
