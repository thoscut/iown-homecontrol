/**
  * @file    esp32_api_spi.cpp
  * @author  iown-homecontrol
  * @brief   ESP32 SPI functions
  *
  * See include/esp32_api_spi.h for what these are (and are not) for.
  */

#include "esp32_api_spi.h"

#if defined(ESP32) || defined(ESP_PLATFORM)

// A register read clocks out three bytes: the address, then two dummy bytes
// while the device shifts its answer back. The transmit buffer held only two,
// so the third byte came from whatever followed it in memory. Both buffers are
// word-aligned because the SPI driver requires that when DMA is in use.
static constexpr size_t SPI_XFER_MAX = 4;

// File-local: these had external linkage under names as generic as `low` and
// `high`, which is an invitation to collide with any other translation unit.
alignas(4) static uint8_t txData[SPI_XFER_MAX] = {};
alignas(4) static uint8_t rxData[SPI_XFER_MAX] = {};
static uint8_t low;
static uint8_t high;

uint8_t GetLowBits() {return low;}
uint8_t GetHighBits() {return high;}

int fInitializeSPI_Channel( int spiCLK, int spiMOSI, int spiMISO, spi_host_device_t SPI_Host, bool EnableDMA) {
  esp_err_t intError;
  spi_bus_config_t bus_config = { };
  bus_config.sclk_io_num = spiCLK; // CLK
  bus_config.mosi_io_num = spiMOSI; // MOSI
  bus_config.miso_io_num = spiMISO; // MISO
  bus_config.quadwp_io_num = -1; // Not used
  bus_config.quadhd_io_num = -1; // Not used
  bus_config.max_transfer_sz = SPI_XFER_MAX;
  // Use the host the caller asked for. This used to be hardcoded to HSPI_HOST,
  // so passing VSPI_HOST silently initialised the wrong bus.
  //
  // The DMA argument is a channel selector, not a flag: passing `true` picked
  // channel 1 by accident of it being 1.
  intError = spi_bus_initialize(SPI_Host, &bus_config,
                                EnableDMA ? SPI_DMA_CH_AUTO : SPI_DMA_DISABLED);
  return intError;
}

int fInitializeSPI_Devices( spi_device_handle_t &h, int csPin, spi_host_device_t SPI_Host) {
  esp_err_t intError;
  spi_device_interface_config_t dev_config = { };  // initializes all field to 0
  dev_config.address_bits     = 0;
  dev_config.command_bits     = 0;
  dev_config.dummy_bits       = 0;
  dev_config.mode             = 3 ;
  dev_config.duty_cycle_pos   = 0;
  dev_config.cs_ena_posttrans = 0;
  dev_config.cs_ena_pretrans  = 0;
  dev_config.clock_speed_hz   = 5000000;
  dev_config.spics_io_num     = csPin;
  dev_config.flags            = 0;
  dev_config.queue_size       = 1;
  dev_config.pre_cb           = nullptr;
  dev_config.post_cb          = nullptr;
  // Attach to the host the caller initialised. Hardcoding HSPI_HOST here meant
  // fInitializeSPI_Channel(VSPI_HOST, ...) brought up one bus and this added
  // the device to another.
  intError = spi_bus_add_device(SPI_Host, &dev_config, &h);
  return intError;
}

int fReadSPIdata16bits(spi_device_handle_t &h, int _address) {
  uint8_t address = _address;
  esp_err_t intError = 0;
  low = 0; high = 0;

  // Three bytes out (address plus two dummies), three bytes in. The dummy
  // bytes are cleared explicitly; they used to carry whatever the previous
  // write had left in the buffer.
  txData[0] = address | 0x80;
  txData[1] = 0;
  txData[2] = 0;

  spi_transaction_t trans_desc = { };
  trans_desc.addr = 0;
  trans_desc.cmd = 0;
  trans_desc.flags = 0;
  trans_desc.length = (8 * 3); // total data bits
  trans_desc.tx_buffer = txData;
  trans_desc.rxlength = (8 * 3); // Number of bits NOT number of bytes
  trans_desc.rx_buffer = rxData;

  intError = spi_device_transmit( h, &trans_desc);
  if ( intError != 0 ) {
    Serial.print( "Transmitting error = ");
    Serial.println ( esp_err_to_name(intError) );
    // Leave low/high at zero rather than publishing whatever the failed
    // transaction left in the buffer.
    return intError;
  }

  // rxData[0] is clocked in while the address goes out and carries nothing.
  // Reading the answer from rxData[0..1], as this did, reported that dead byte
  // as the low half and the real low half as the high half.
  low = rxData[1]; high = rxData[2];
  return intError;
}

int fWriteSPIdata8bits(spi_device_handle_t &h, int _address, int _sendData) {
  uint8_t address =  _address;
  uint8_t sendData = _sendData;
  esp_err_t intError;
  spi_transaction_t trans_desc = { };
  trans_desc.addr =  0;
  trans_desc.cmd = 0;
  trans_desc.flags = 0;
  trans_desc.length = (8 * 2); // total data bits
  trans_desc.tx_buffer = txData;
  trans_desc.rxlength = 0 ; // Number of bits NOT number of bytes
  trans_desc.rx_buffer = nullptr;
  txData[0] = address  & 0x7F;
  txData[1] = sendData;
  intError = spi_device_transmit( h, &trans_desc);
  if ( intError != 0 ) {
     Serial.print("Transmitting error = ");
     Serial.println(esp_err_to_name(intError));
   }
  return intError;
}

#endif  // ESP32 || ESP_PLATFORM
