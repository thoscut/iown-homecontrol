/**
  * @file    iown_defs.h
  * @author  iown-homecontrol
  * @brief   Basic Definitions and Functions
  */

#pragma once
/* Define to prevent recursive inclusion */
/* The guard macro was _IOWN_DEFS_H. A leading underscore followed by a capital
   is reserved for the implementation in both C and C++, so that name was the
   compiler's to use, not ours. */
#ifndef IOWN_DEFS_H
#define IOWN_DEFS_H
#ifdef __cplusplus
  extern "C" {
#endif


#include "iown_frame.h"
#include "iown_mac.h"

/* IOWN_MODE_1W / IOWN_MODE_2W were defined here, below the include of
   iown_frame.h that uses them in IOWN_LEN_PACKET(). They now live in
   iown_frame.h next to that macro; including this header still brings them
   in. */


#ifdef __cplusplus
}
#endif
#endif /* IOWN_DEFS_H */
