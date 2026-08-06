# SVD / peripheral descriptions

Peripheral descriptions for the microcontrollers found in io-homecontrol
devices, for loading into IDA Pro or Ghidra so register accesses in a
disassembly show up by name.

| File | Device |
|---|---|
| `ATmega1284P-iown.atdf` / `.svd` | Atmel ATmega1284P |
| `STM32F101-iown.svd` / `.json` / `.mmap` | STMicroelectronics STM32F101 |
| `CMSIS-SVD.xsd` | the schema those `.svd` files follow |

`SVDConv.exe` used to sit here. It is ARM's CMSIS SVD converter, not something
this project wrote, and a tracked Windows executable blocks the library from
the Arduino Library Manager index. Get it from the CMSIS distribution when you
need it: <https://github.com/ARM-software/CMSIS_5> (`CMSIS/Utilities/`).
