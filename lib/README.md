<div align="center" width="100%">

# `lib`: Project Private Libraries

</div>

PlatformIO will compile these project specific private libs to static libraries and link into the executable.

The source code of each lib is placed in its own directory: `lib/library_name/[source files]`.

This directory is currently empty: iown-homecontrol has no private libraries.
Board pin maps, which used to come from an external `LoRa32` library, live in
`src/board_pins.h` - see the comment at the top of that file for why.

## Structure of a library `Foo`

``` ascii
|--lib
|  |
|  |--Foo
|  |  |--docs
|  |  |--examples
|  |  |--src
|  |     |- Foo.c
|  |     |- Foo.h
|  |  |- library.json - Build options, etc.: https://docs.platformio.org/page/librarymanager/config.html
|
|- platformio.ini
|--src
   |- main.cpp
   |- ...
```

### Contents of `main.cpp`

``` cpp
#include <Foo.h>

int main (void) {
  // ...
}
```

> [!NOTE]
> PlatformIOs [Library Dependency Finder (LDF)](https://docs.platformio.org/page/librarymanager/ldf.html) will automatically find dependent libs when scanning the project source files.
