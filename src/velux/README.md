# Velux-Specific Support for io-homecontrol

Comprehensive support for Velux roof windows (Dachfenster), blinds, and accessories with intelligent automation features.

## Features

### 🪟 Roof Windows (Dachfenster)

- **Predefined Ventilation Positions** (Lüftungsstellungen)
  - Position 1: 10% - Minimal ventilation
  - Position 2: 20% - Medium ventilation
  - Position 3: 30% - Maximum ventilation
  - Intelligent temperature-based recommendations

- **Rain Sensor Integration**
  - Automatic rain detection
  - Emergency closing on rain
  - Configurable protection zones

- **Supported Models**
  - GGL / GGU - Top-operated windows
  - GPL / GPU - Top-operated windows
  - GGL/GGU Solar - Solar powered
  - GGL/GGU Electric (KMX 100/200) - Electric motors

### 🎨 Blinds and Shutters

- **Interior Blinds**
  - DML - Blackout blind (Verdunkelungsrollo)
  - RML - Roller blind (Hitzeschutzrollo)
  - FML - Pleated blind (Faltenrollo)

- **Exterior Protection**
  - MML - Awning blind (Außenrollo)
  - SML - Roller shutter (Rolladen)

- **Smart Features**
  - Model-specific recommended positions
  - Tilt support (venetian blinds)
  - Heat protection automation

## Usage

### C++ API

```cpp
#include "velux/iohome_velux.h"

using namespace iohome::velux;

// Create Velux window controller
uint8_t node_id[3] = {0x64, 0x65, 0x75};
VeluxWindow window(node_id, VeluxModel::GGL_ELECTRIC);

// Enable rain protection
window.set_rain_protection(true);

// Set to ventilation position 2
frame::IoFrame frame;
window.create_ventilation_frame(&frame, src_node, 2);
controller.transmit_frame(&frame);

// Emergency close (on rain detection)
window.create_emergency_close_frame(&frame, src_node);
controller.transmit_frame(&frame);

// Check rain sensor
RainSensorStatus rain = VeluxWindow::parse_rain_sensor_status(&received_frame);
if (rain == RainSensorStatus::RAIN) {  // an Execute a rain sensor originated
  // Rain detected!
}
```

### ESPHome Configuration

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/velocet/iown-homecontrol
    components: [iohomecontrol]

iohomecontrol:
  cs_pin: GPIO18
  irq_pin: GPIO26
  rst_pin: GPIO14
  node_id: "0xABCDEF"
  system_key: "your_key_here"

cover:
  # Velux roof window with rain sensor
  - platform: iohomecontrol
    name: "Dachfenster Schlafzimmer"
    node_id: "0x646575"
    device_type: window_opener
    velux_model: GGL_ELECTRIC
    supports_rain_sensor: true
    rain_protection: true  # Auto-close on rain

  # Velux blackout blind
  - platform: iohomecontrol
    name: "Verdunkelungsrollo"
    node_id: "0x111111"
    device_type: roller_shutter
    velux_model: DML

# Ventilation buttons
button:
  - platform: template
    name: "Lüftungsstellung 1"
    on_press:
      - cover.control:
          id: bedroom_window
          position: 10%

  - platform: template
    name: "Lüftungsstellung 2"
    on_press:
      - cover.control:
          id: bedroom_window
          position: 20%

  - platform: template
    name: "Lüftungsstellung 3"
    on_press:
      - cover.control:
          id: bedroom_window
          position: 30%
```

## Supported Velux Models

### Roof Windows

| Model | Description | Rain Sensor | io-homecontrol |
|-------|-------------|-------------|----------------|
| GGL | Top-operated roof window | Optional | 1W |
| GGU | Top-operated roof window | Optional | 1W |
| GPL | Top-operated roof window | Optional | 1W |
| GPU | Top-operated roof window | Optional | 1W |
| GGL Solar | Solar powered window | Yes | 1W |
| GGU Solar | Solar powered window | Yes | 1W |
| GGL Electric (KMX) | Electric motor upgrade | Yes | 1W |
| GGU Electric (KMX) | Electric motor upgrade | Yes | 1W |

### Blinds

| Model | Type | Location | Tilt Support |
|-------|------|----------|--------------|
| DML | Blackout blind | Interior | No |
| RML | Roller blind | Interior | No |
| FML | Pleated blind | Interior | Limited |
| MML | Awning blind | Exterior | No |
| SML | Roller shutter | Exterior | No |

### Controllers

| Model | Description | Type |
|-------|-------------|------|
| KLR 200 | Remote control pad | 2W Controller |
| KLI 310 | Wall switch | 1W Controller |
| KLF 200 | Internet gateway | Gateway |

## Intelligent Automations

### Rain Protection

Automatically close windows when rain is detected:

```yaml
automation:
  - id: auto_close_on_rain
    trigger:
      - platform: state
        entity_id: binary_sensor.rain_detected
        to: "on"
    action:
      - cover.close: all_windows
```

### Temperature-Based Ventilation

Open windows based on indoor temperature:

```cpp
uint8_t level = velux::get_recommended_ventilation(indoor_temp);
// Returns 0-3 based on temperature:
// < 18°C: 0 (closed)
// 18-22°C: 1 (minimal)
// 22-25°C: 2 (medium)
// > 25°C: 3 (maximum)
```

### Heat Protection

Close exterior blinds during hot weather:

```yaml
automation:
  - id: heat_protection
    trigger:
      - platform: numeric_state
        entity_id: sensor.outdoor_temp
        above: 28
    action:
      - cover.close: exterior_blinds
```

### Night Mode

Close windows and blackout blinds at sunset:

```yaml
automation:
  - id: night_mode
    trigger:
      - platform: sun
        event: sunset
    action:
      - cover.close: all_windows
      - delay: 2min
      - cover.close: blackout_blinds
```

### Security Check

Alert if windows are open when leaving home:

```yaml
automation:
  - id: security_check
    trigger:
      - platform: state
        entity_id: binary_sensor.home_occupied
        to: "off"
    condition:
      - condition: state
        entity_id: binary_sensor.windows_open
        state: "on"
    action:
      - service: notify.mobile_app
        data:
          message: "Windows still open!"
```

## Ventilation Positions (Lüftungsstellungen)

Velux windows have standardized ventilation positions for optimal air exchange without wide opening:

| Position | Opening | Use Case | German |
|----------|---------|----------|---------|
| Closed | 0% | Closed | Geschlossen |
| Ventilation 1 | 10% | Light air exchange | Lüftungsstellung 1 |
| Ventilation 2 | 20% | Normal ventilation | Lüftungsstellung 2 |
| Ventilation 3 | 30% | Maximum ventilation | Lüftungsstellung 3 |
| Half Open | 50% | Moderate opening | Halb offen |
| Fully Open | 100% | Complete opening | Ganz offen |

### Recommended Usage

- **Night**: Position 1 (10%) for fresh air without cold drafts
- **Day (Winter)**: Position 2 (20%) for room air exchange
- **Day (Summer)**: Position 3 (30%) for maximum cooling
- **Cleaning**: Fully open (100%)

## Rain Sensor Integration

### Automatic Rain Detection

Windows with integrated rain sensors can automatically close when precipitation is detected:

A rain sensor is an input, not something a controller polls. There is no query
and no answer frame - what you see is the sensor *acting*: an ordinary Execute
whose Command Originator is `0x02` (RAIN). So a controller learns about rain by
watching the traffic, and the absence of rain is simply the absence of such a
frame. There is no "dry" report to wait for.

```cpp
if (VeluxWindow::parse_rain_sensor_status(&received_frame) == RainSensorStatus::RAIN) {
  // A rain sensor has just driven an actuator. The window it belongs to is
  // already closing on its own - this is a notification, not a request.
}
```

To close a window *because* of rain, name the sensor as the originator so it
outranks a user command that asked for the window to be open:

```cpp
window.create_rain_close_frame(&frame, src_node);
```

### ESPHome Rain Protection

```yaml
binary_sensor:
  - platform: template
    name: "Rain Detected"
    lambda: |-
      // Query rain sensor from window
      return check_rain_sensor();
    filters:
      - delayed_on: 10s    # Avoid false positives
      - delayed_off: 300s  # Stay active 5min after rain

automation:
  - trigger:
      - platform: state
        entity_id: binary_sensor.rain_detected
        to: "on"
    action:
      - cover.close: all_velux_windows
```

## Finding Node IDs

### Method 1: Device Label

Velux devices have a label with product info:

```
VELUX [Model]
S/N: AABBCC-1234-5678-90AB

Node ID = AA BB CC (first 3 bytes of serial)
```

### Method 2: KLF 200 Gateway

If you have a Velux KLF 200 gateway, connect via:
- IP: http://velux-klf-XXXX.local (check sticker)
- Default password: on device label

Navigate to device list to see all node IDs.

### Method 3: RTL-SDR Sniffing

Use RTL-SDR to capture frames:

```bash
rtl_433 -R 189 -f 868.9M -s 1000k -g 42.1
```

Press button on device and note source address in decoded frame.

## Model Detection

Automatic model detection from discovery responses:

```cpp
// Discover devices
controller.start_discovery(0xFF, 10000);

// Get discovered device
mode2w::DiscoveredDevice device;
controller.get_discovered_device(0, &device);

// What kind of product it is. The node type says "window opener"; which
// window opener - GGL, GGU, GPL - is not on the wire, so this returns a
// category, not a model number.
VeluxCategory category = velux::detect_category(
  device.node_type,
  device.manufacturer
);
```

## Troubleshooting

### Window Not Responding

1. **Check node ID**: Verify 3-byte node ID is correct
2. **Check system key**: Must match paired key
3. **Check rain sensor**: May prevent opening if malfunction
4. **Check limits**: Window may have position limits set
5. **Battery level**: Solar/battery windows need sufficient charge

### Rain Protection Issues

1. **Sensor calibration**: May need recalibration
2. **Sensor cleaning**: Clean sensor area on window frame
3. **False positives**: Adjust delay in automation
4. **Timing**: Allow 10-30s delay for sensor response

### Position Feedback

Note: Most Velux windows use 1W mode and don't report position. Use `assumed_state: true` in ESPHome.

For 2W devices (like KLF 200), position feedback is available.

## Advanced Features

### Custom Ventilation Schedules

```yaml
# Summer night cooling schedule
automation:
  - id: summer_night_cooling
    trigger:
      - platform: time
        at: "22:00:00"
    condition:
      - condition: numeric_state
        entity_id: sensor.outdoor_temp
        below: 20
      - condition: numeric_state
        entity_id: sensor.indoor_temp
        above: 24
    action:
      # Open windows for night cooling
      - cover.control:
          id: bedroom_window
          position: 30%
      - cover.control:
          id: living_window
          position: 20%

  - id: summer_night_cooling_off
    trigger:
      - platform: time
        at: "06:00:00"
    action:
      - cover.close: bedroom_window
      - cover.close: living_window
```

### Multi-Room Climate Control

```yaml
script:
  - id: climate_balance
    then:
      - lambda: |-
          // Open windows in hot rooms, close in cool rooms
          if (id(bedroom_temp).state > 25) {
            id(bedroom_window).make_call().set_position(0.3).perform();
          }
          if (id(living_temp).state < 20) {
            id(living_window).make_call().set_position(0.0).perform();
          }
```

## Contributing

For Velux-specific features and improvements:
- Test with real Velux hardware
- Document model-specific behaviors
- Share node IDs and device types
- Report rain sensor protocols

## References

- Velux Product Documentation: https://www.velux.com
- io-homecontrol Protocol: ../docs/linklayer.md
- Device Database: ../docs/devices/velux/
- Example Configuration: ../../examples/esphome-velux-dachfenster.yaml

## License

CC-BY-SA-4.0 - See LICENSE file

## Disclaimer

This is an unofficial implementation. Velux and io-homecontrol are registered trademarks of their respective owners.
