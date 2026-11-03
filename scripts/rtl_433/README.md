# rtl_433 configuration

`rtl_433.conf` configures [rtl_433](https://github.com/merbanan/rtl_433) to
receive and decode io-homecontrol with its built-in decoder (`-R 189`). Place it
in the rtl_433 folder and run rtl_433 without arguments; the comment block at
the top of the file describes the protocol and the settings it uses. See also
[`docs/radio-rtlsdr.md`](../../docs/radio-rtlsdr.md).

`rtl_433-msvc-nightly-rtlsdrblog.7z` used to sit here: a Windows nightly build
of rtl_433 bundled with the RTL-SDR Blog driver. It was a third-party binary
that this project did not build and that nothing here could verify, so it was
removed. Get rtl_433 from its upstream releases instead:
<https://github.com/merbanan/rtl_433/releases> (Windows builds, including the
nightly, are attached to each release). If you have an RTL-SDR Blog dongle, its
driver is at <https://github.com/rtlsdrblog/rtl-sdr-blog/releases>.
