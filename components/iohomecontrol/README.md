# io-homecontrol ESPHome Custom Component!

ESPHome custom component for controlling io-homecontrol devices (Velux, Somfy) using an ESP32 with 868MHz FSK-capable radio module.

## Features

- **1W Mode Support**: Control one-way devices (most Velux and Somfy actuators)
- **Cover Platform**: Native ESPHome cover entities for blinds, shutters, and windows
- **Automatic Encryption**: AES-128 encryption and HMAC authentication
- **RadioLib Integration**: Compatible with SX1276, RFM69, RFM95, and other FSK radios
- **Home Assistant Integration**: Automatic discovery and seamless integration

## Hardware Requirements

### ESP32 Boards

- **Heltec WiFi LoRa 32** (V2, V3) - Recommended
- **LilyGo LoRa32**, T-Beam, T3-S3
- **AdaFruit ESP32 Feather** + RFM69HCW/RFM95W FeatherWing
- **DFRobot FireBeetle ESP32** + LoRa 868MHz module
- Any ESP32 + SX1276/RFM69/RFM95 module with SPI connection

### Radio Module

Must support:
- **Frequency**: 868-870 MHz (European ISM band)
- **Modulation**: FSK (Frequency Shift Keying)
- **Data Rate**: 38.4 kbps
- **Frequency Deviation**: 19.2 kHz

Recommended modules:
- SX1276 (LoRa capable, but used in FSK mode)
- RFM69HCW
- RFM95W
- Si4463

## Installation

### 1. Add Component to ESPHome Configuration

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/velocet/iown-homecontrol
      ref: main
    components: [iohomecontrol]
```

Or for local development:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [iohomecontrol]
```

### 2. Configure SPI

```yaml
spi:
  clk_pin: GPIO5
  miso_pin: GPIO19
  mosi_pin: GPIO27
```

### 3. Configure io-homecontrol Component

```yaml
iohomecontrol:
  cs_pin: GPIO18      # SPI Chip Select
  irq_pin: GPIO26     # DIO0 / IRQ pin
  rst_pin: GPIO14     # Reset pin

  node_id: "0xABCDEF"  # Your controller's 3-byte node ID
  system_key: "E994BACFE6BED7667630EAE475BAAE95"  # 16-byte system key

  frequency: 868.95    # MHz (channel 2, primary 1W/2W)
  mode: 1W            # 1W or 2W
  verbose: false      # Enable debug logging
```

### 4. Add Cover Entities

```yaml
cover:
  - platform: iohomecontrol
    name: "Bedroom Window"
    node_id: "0x646575"  # Target device's node ID
    device_type: window_opener
```

## Configuration Reference

### Main Component (`iohomecontrol`)

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `cs_pin` | Pin | Yes | - | SPI Chip Select pin |
| `irq_pin` | Pin | Yes | - | Radio interrupt pin (DIO0) |
| `rst_pin` | Pin | Yes | - | Radio reset pin |
| `node_id` | String/Int | Yes | - | This controller's 3-byte node ID (hex string or integer) |
| `system_key` | String | Yes | - | 16-byte AES system key (32 hex characters) |
| `frequency` | Float | No | 868.95 | Operating frequency in MHz (868.0-870.0) |
| `mode` | String | No | "1W" | Protocol mode: "1W" or "2W" |
| `verbose` | Boolean | No | false | Enable verbose logging |

### Cover Platform

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `node_id` | String/Int | Yes | - | Target device's 3-byte node ID |
| `device_type` | Enum | No | roller_shutter | Device type (see below) |

### Device Types

- `roller_shutter` - Standard roller shutter
- `adjustable_slat_shutter` - Shutter with adjustable slats
- `screen` - Screen/mesh blind
- `window_opener` - Roof window or skylight
- `venetian_blind` - Venetian blind
- `exterior_blind` - Exterior sun blind
- `dual_shutter` - Dual roller shutter
- `garage_door` - Garage door
- `awning` - Retractable awning
- `curtain` - Motor curtain
- `pergola` - Pergola cover

## Finding Device Node IDs

### Method 1: Check Device Label

Velux devices have a label with QR code containing the node ID. Look for:
- Serial number format: `AABBCC-XXXX-YYYY-ZZZZ`
- The node ID is typically encoded in the serial number

### Method 2: Sniff RF Traffic

Use RTL-SDR to capture frames:

```bash
rtl_433 -R 189 -f 868.9M -s 1000k -g 42.1
```

Press a button on the device and look for the source address in the decoded frame.

### Method 3: Use Velux KLF 200 Gateway

If you have a KLF 200 gateway, you can find node IDs in the configuration files:

```
/apps/var/lib/io-homecontrol/lua-storage/io/stack/[node_id]
```

## Obtaining System Key

### During Pairing (1W Mode)

1. Enable pairing mode on the actuator (usually by holding setup button)
2. Send discovery command (0x28-0x2D)
3. Send key transfer command (0x30) with generated system key
4. Key is encrypted with transfer key and transmitted

### From Existing Controller

If you have an existing controller (KLF 200, KLR 200, etc.), you may be able to extract the system key from:
- Configuration files
- Flash memory dumps
- Network traffic (if using TCP/IP gateway)

**Note**: Each actuator has its own unique system key established during pairing.

## Pin Mapping for Common Boards

### Heltec WiFi LoRa 32 V2

```yaml
spi:
  clk_pin: GPIO5
  miso_pin: GPIO19
  mosi_pin: GPIO27

iohomecontrol:
  cs_pin: GPIO18
  irq_pin: GPIO26   # DIO0
  rst_pin: GPIO14
```

### LilyGo LoRa32 V2.1

```yaml
spi:
  clk_pin: GPIO5
  miso_pin: GPIO19
  mosi_pin: GPIO27

iohomecontrol:
  cs_pin: GPIO18
  irq_pin: GPIO26   # DIO0
  rst_pin: GPIO23
```

### Adafruit ESP32 Feather + RFM95W FeatherWing

```yaml
spi:
  clk_pin: GPIO5
  miso_pin: GPIO19
  mosi_pin: GPIO18

iohomecontrol:
  cs_pin: GPIO33
  irq_pin: GPIO27   # DIO0
  rst_pin: GPIO32
```

## Troubleshooting

### Device Not Responding

1. **Verify node ID**: Ensure the node ID is correct (3 bytes)
2. **Check system key**: Key must match the one programmed during pairing
3. **Verify frequency**: Most devices use 868.95 MHz (channel 2)
4. **Check mode**: Ensure 1W/2W mode matches device capability
5. **Check wiring**: Verify SPI pins and radio module connections
6. **Signal strength**: Device must be within range (~20m indoors)

### Radio Initialization Failed

1. **Check pin assignments**: Verify CS, IRQ, and RST pins
2. **Verify SPI bus**: Use `i2cdetect` or SPI scanner
3. **Power supply**: Ensure adequate 3.3V power for radio module
4. **Module compatibility**: Confirm radio chip is supported (SX1276, RFM69, etc.)

### Commands Not Working

1. **Enable verbose logging**: Set `verbose: true` to see detailed protocol info
2. **Monitor with RTL-SDR**: Use `rtl_433` to verify frames are transmitted
3. **Check rolling code**: Ensure rolling code is incrementing (1W mode)
4. **Verify CRC**: Check logs for CRC calculation errors
5. **HMAC validation**: Ensure system key is correct

### Home Assistant Integration

If device doesn't appear in Home Assistant:

1. **Check API encryption key**: Must be configured and match
2. **Restart Home Assistant**: Sometimes needed for discovery
3. **Check ESPHome logs**: Look for connection errors
4. **Verify network**: Ensure ESP32 is on same network as HA
5. **Check firewall**: Port 6053 must be open for API

## Advanced Usage

### Custom Commands

Send custom commands using lambdas:

```yaml
button:
  - platform: template
    name: "Send Custom Command"
    on_press:
      - lambda: |-
          auto controller = id(iohomecontrol_component).get_controller();
          uint8_t node[3] = {0x64, 0x65, 0x75};
          uint8_t params[2] = {0x50, 0x00};
          controller->send_command(node, 0x60, params, 2);
```

### Reading RSSI/SNR

```yaml
sensor:
  - platform: template
    name: "RF Signal Strength"
    lambda: |-
      auto controller = id(iohomecontrol_component).get_controller();
      return controller->get_rssi();
    update_interval: 60s
    unit_of_measurement: "dBm"
```

### Multiple Frequencies (2W Mode)

For 2W mode with frequency hopping (future enhancement):

```yaml
iohomecontrol:
  mode: 2W
  channels:
    - 868.25  # Channel 1
    - 868.95  # Channel 2
    - 869.85  # Channel 3
  hop_interval: 2.7  # ms
```

## Known Limitations

- **1W Mode Only**: Currently only 1-way mode is fully implemented
- **No Position Feedback**: 1W devices don't report position, state is assumed
- **No Pairing**: Pairing must be done separately, component requires pre-shared keys
- **Single Controller**: Multiple controllers on same frequency may interfere

## Future Enhancements

- [ ] 2W mode with frequency hopping
- [ ] Automatic device discovery
- [ ] Pairing workflow via ESPHome services
- [ ] Position feedback for 2W devices
- [ ] Velux KLF 200 API emulation
- [ ] Challenge-response authentication (2W)
- [ ] Multiple channel support

## Contributing

Contributions are welcome! Please see the main repository for guidelines:
https://github.com/velocet/iown-homecontrol

## License

This project is licensed under CC-BY-SA-4.0. See LICENSE for details.

## Disclaimer

This is an unofficial reverse-engineered implementation. Use at your own risk.
io-homecontrol is a registered trademark of Somfy SAS.

## Support

- **Telegram**: https://t.me/iownHomecontrol
- **Discord**: https://discord.gg/MPEb7dTNdN
- **GitHub Issues**: https://github.com/velocet/iown-homecontrol/issues
