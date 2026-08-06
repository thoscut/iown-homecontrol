# SkitterIO

ActionScript sources recovered from Somfy's *SkitterIO* USB gateway service.
They are the interesting part of this directory: `applicationProxy/CRC16.as`
and `applicationProxy/ProxyFrame.as` describe the framing and checksum from the
vendor's own side, which is a useful second opinion on
[`../../linklayer.md`](../../linklayer.md).

The binary these came from, `SomfyUsbGwSrv.exe`, is no longer kept here. A
tracked Windows executable blocks the library from the Arduino Library Manager
index, and its only value was as the input to a decompiler that has already
been run. The extracted sources next to this file are that output.
