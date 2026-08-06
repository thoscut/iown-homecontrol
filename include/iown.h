/**
  * @file    iown.h
  * @author  iown-homecontrol
  * @brief   Header for the iown-homecontrol lib
  */

#pragma once
/* Define to prevent recursive inclusion */

/*
 * NOTE: `#pragma once` belongs above the includes, not below them - as written,
 * <Arduino.h> was re-processed on every inclusion of this header.
 *
 * That include is gone: nothing under here needs the Arduino framework, and
 * requiring it tied these definitions to one build. Without it the header and
 * everything it pulls in compile on the host, so the strict-warning CI job can
 * cover src/iown_mac.cpp too.
 */

#ifdef __cplusplus
  extern "C" {
#endif


#include "iown_defs.h" /* ioHC Definitions */


#ifdef __cplusplus
}
#endif
