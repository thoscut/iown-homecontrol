# Security Policy

## Reporting a Vulnerability

Please report vulnerabilities in the authentication or frame-parsing paths
privately, using GitHub's
[private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
on this repository. Do not open a public issue for those.

For anything else - a build problem, a protocol question, a device that will
not pair - open an issue or join the Telegram or Discord chat.

## Scope

This project implements a protocol it does not control. A good deal of what
looks like a weakness is a property of io-homecontrol itself: frames are
authenticated but never encrypted, and the key used to distribute system keys
during pairing is a constant every device on the market shares.

[`docs/SECURITY-MODEL.md`](../docs/SECURITY-MODEL.md) sets out what the
authentication does and does not protect against, and which of the gaps are
ours to close. Please read it before reporting - it will tell you quickly
whether you have found a bug in this implementation or a property of the
protocol.

Findings in these areas are always in scope:

- Frame parsing: out-of-bounds reads or writes from received bytes
- MAC verification that can be bypassed or short-circuited
- Replay protection that accepts a recorded frame
- Challenge generation that is predictable
- Key material that leaks into logs or persists after use
