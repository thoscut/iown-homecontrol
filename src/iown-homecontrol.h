/**
 * @file iown-homecontrol.h
 * @brief Umbrella header - the one include an Arduino sketch needs
 *
 * The Arduino library specification asks for a header named after the library,
 * and both the IDE and arduino-lint look for it. This is that header: it pulls
 * in the public API and nothing else.
 *
 * @code{.cpp}
 * #include <RadioLib.h>
 * #include <iown-homecontrol.h>
 *
 * SX1276 radio = new Module(18, 26, 14, 35);
 * iohome::IoHomeControl controller(&radio);
 * @endcode
 *
 * The headers under `src/protocol/` and `src/velux/` remain includable on their
 * own; nothing here hides them. Board pin maps live in `src/board_pins.h`,
 * which is deliberately *not* included from here - it is for the firmware in
 * `src/main.cpp`, and a library has no business deciding a sketch's wiring.
 */

#ifndef IOWN_HOMECONTROL_H
#define IOWN_HOMECONTROL_H

// Protocol layer: frames, CRC, MAC, replay protection.
#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"
#include "protocol/iohome_frame.h"
#include "protocol/iohome_replay_guard.h"
#include "protocol/iohome_rolling_code_store.h"
#include "protocol/iohome_2w.h"

// High-level controller. Needs RadioLib, which the sketch must include first
// on some toolchains; including it here keeps that from being a surprise.
#include "IoHomeControl.h"

// Velux-specific helpers: window and blind models, recommended positions.
#include "velux/iohome_velux.h"

/// Library version, matching library.properties and library.json.
#define IOWN_HOMECONTROL_VERSION_MAJOR 0
#define IOWN_HOMECONTROL_VERSION_MINOR 7
#define IOWN_HOMECONTROL_VERSION_PATCH 0
#define IOWN_HOMECONTROL_VERSION "0.7.0"

#endif  // IOWN_HOMECONTROL_H
