/**
 * @file legacy_phy.ino
 * @brief Minimal physical-layer bring-up for io-homecontrol
 * @author Velocet
 *
 * Brings up a LoRa32 radio and prints what the PhysicalLayer interface
 * reports. This is the smallest starting point for driving the radio
 * directly; for the actual protocol - frames, authentication, replay
 * protection - use IoHomeControl, as src/main.cpp does.
 *
 * MIT License
 * Copyright (c) Velocet
 */

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

#include <IoHome.h>
#include <board_pins.h>

// RadioLib: load the module using this board's pin map (src/board_pins.h)
IOHC_RADIO_CLASS radio = IOHC_MAKE_RADIO();

// RadioLib: Get common layer pointer "phy"
PhysicalLayer* phy = (PhysicalLayer*)&radio;

// function declarations
// ...
void dummyISR() {} // TODO configure interrupt actions

void setup() { // setup code to run once
  Serial.begin(115200);

  int state = 0;

  iohc_board_spi_begin();

  state = radio.begin();
  if(state) {Serial.print(F("[RADIO] Init: "));Serial.println(state);while(true);}

  // access common configuration layer through the "phy" pointer
  state = phy->setFrequency(868.95);
  if(state) {Serial.print(F("[PHY] Frequency: "));Serial.println(state);while(true);}

  // interrupt-driven Rx/Tx
  // phy->setPacketReceivedAction(dummyISR);
  // phy->setPacketSentAction(dummyISR);
  // phy->clearPacketReceivedAction(); // clear interrupt actions
  // phy->clearPacketSentAction();     // clear interrupt actions

  // Common SNR/RSSI measurement functions and also a "true" random number generator
  Serial.print(F("[PHY] SNR     (dBm): "));Serial.println(phy->getSNR());
  Serial.print(F("[PHY] RSSI    (dBm): "));Serial.println(phy->getRSSI());
  Serial.print(F("[PHY] RNG (0 - 100): "));Serial.println(phy->random(100));

  state = phy->standby(); // change mode to standby ...
  if(state) {Serial.print(F("[PHY] Standby: "));Serial.println(state);while(true);}

}

void loop() {Serial.println("l00p");} // code to run repeatedly

// function definitions
// ...
