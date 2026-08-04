/**
 * @file IoHome.cpp
 * @brief Thin RadioLib-oriented io-homecontrol node (legacy API)
 */

#include "IoHome.h"

#include "protocol/iohome_crypto.h"

#include <string.h>

IoHomeNode::IoHomeNode(PhysicalLayer* phy, const IoHomeChannel_t* chan)
  : phyLayer(phy),
    channel(chan) {
}

bool IoHomeNode::begin(const IoHomeChannel_t* chan,
                       NodeId source_node_id,
                       NodeId destination_node_id,
                       const uint8_t* stack_key,
                       const uint8_t* system_key) {
  if (phyLayer == nullptr) {
    return false;
  }

  this->channel = chan;
  this->sourceNodeId = source_node_id;
  this->destinationNodeId = destination_node_id;

  if (stack_key != nullptr) {
    memcpy(stackKey_, stack_key, sizeof(stackKey_));
    stackKeySet_ = true;
  }

  if (system_key != nullptr) {
    memcpy(systemKey_, system_key, sizeof(systemKey_));
    systemKeySet_ = true;
  }

  return true;
}

int16_t IoHomeNode::setPhyProperties() {
  if (phyLayer == nullptr) {
    return RADIOLIB_ERR_INVALID_RADIO;
  }

  // Start at the highest power and step down until the module accepts a value.
  // The original loop had no lower bound and spun forever on a module that
  // rejects every level.
  int8_t pwr = 20;
  int16_t state = RADIOLIB_ERR_INVALID_OUTPUT_POWER;
  do {
    state = phyLayer->setOutputPower(pwr);
  } while (state == RADIOLIB_ERR_INVALID_OUTPUT_POWER && --pwr >= -3);
  RADIOLIB_ASSERT(state);

  // RadioLib expresses the FSK data rate in kbps and the deviation in kHz.
  DataRate_t io_home_fskrate;
  io_home_fskrate.fsk.bitRate = 38.4f;
  io_home_fskrate.fsk.freqDev = 19.2f;

  state = phyLayer->setDataRate(io_home_fskrate);
  RADIOLIB_ASSERT(state);
  state = phyLayer->setDataShaping(RADIOLIB_SHAPING_NONE);
  RADIOLIB_ASSERT(state);
  state = phyLayer->setEncoding(RADIOLIB_ENCODING_NRZ);
  RADIOLIB_ASSERT(state);

  uint8_t sync_word[IOHOME_SYNC_WORD_LEN] = {0};
  sync_word[0] = static_cast<uint8_t>(IOHOME_SYNC_WORD >> 16);
  sync_word[1] = static_cast<uint8_t>(IOHOME_SYNC_WORD >> 8);
  sync_word[2] = static_cast<uint8_t>(IOHOME_SYNC_WORD);

  state = phyLayer->setSyncWord(sync_word, IOHOME_SYNC_WORD_LEN);
  RADIOLIB_ASSERT(state);

  // RadioLib takes the FSK preamble length in bits, so pass it through as-is.
  // Dividing by 8 here configured a 64-bit preamble instead of 512.
  state = phyLayer->setPreambleLength(IOHOME_PREAMBLE_LEN);
  RADIOLIB_ASSERT(state);

  return state;
}

uint16_t IoHomeNode::crc16(const uint8_t* data, size_t len) {
  return iohome::crypto::compute_crc16(data, len);
}
