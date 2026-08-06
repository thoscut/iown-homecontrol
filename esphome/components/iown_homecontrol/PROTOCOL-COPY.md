# The `iohome_*.{h,cpp}` files here are copies

They are copied verbatim from `src/protocol/` by
`tools/sync_esphome_protocol.py`. **Do not edit them here.** Change the
originals and re-run the script; CI fails the build if the two ever differ.

ESPHome's `external_components` takes a component directory and compiles the
files directly inside it. Subdirectories are skipped, and the component cannot
reach the repository root, so the copies have to sit alongside the component's
own sources rather than in a tidy subfolder.

The alternative was a second implementation of the same wire format, which this
project has already been bitten by: the 2W key transfer used an initial value
neither peer computed, and the round-trip test agreed with itself for months
because both directions were wrong in the same way. There is now one
implementation and a copy of it, not two implementations.

`iohome_nvs_store.*` is not copied - it includes Arduino's `<Preferences.h>`,
and the component uses ESPHome's preference API instead.

**After adding or removing a file here, run `esphome clean` before compiling.**
PlatformIO does not notice new sources in an existing build tree, and the link
fails with undefined references to functions whose `.cpp` is sitting right
there uncompiled.
