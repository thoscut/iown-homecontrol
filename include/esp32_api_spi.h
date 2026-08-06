/**
  * @file    esp32_api_spi.h
  * @author  iown-homecontrol
  * @brief   ESP32 SPI functions
  *
  * Thin wrappers around the ESP-IDF SPI master driver, kept for register-level
  * bring-up of a radio module. The protocol stack itself does not use these -
  * it reaches the radio through RadioLib.
  *
  * NOTE: these take `spi_device_handle_t&`, a C++ reference, so despite the
  * `extern "C"` linkage block they can only be called from C++.
  */

#pragma once

#include <Arduino.h>

#ifdef __cplusplus
  extern "C" {
#endif

#include <driver/spi_master.h>

/** Low byte of the value read by the last fReadSPIdata16bits(). */
uint8_t GetLowBits();

/**
 * High byte of the value read by the last fReadSPIdata16bits().
 *
 * This returned `int8_t` while the byte it reports is a `uint8_t`, so every
 * register value from 0x80 up came back negative.
 */
uint8_t GetHighBits();

int fInitializeSPI_Channel( int spiCLK, int spiMOSI, int spiMISO, spi_host_device_t SPI_Host, bool EnableDMA);

/**
 * Attach a device to an already-initialised SPI host.
 *
 * @param SPI_Host must be the same host that was passed to
 *        fInitializeSPI_Channel(). It was hardcoded to HSPI_HOST here, so a
 *        caller that had brought up VSPI added its device to a bus that was
 *        never initialised.
 */
int fInitializeSPI_Devices( spi_device_handle_t &h, int csPin, spi_host_device_t SPI_Host);

int fReadSPIdata16bits( spi_device_handle_t &h, int address );
int fWriteSPIdata8bits( spi_device_handle_t &h, int address, int sendData );

#ifdef __cplusplus
}
#endif
