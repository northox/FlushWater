# Host-side decision tests

`decideFlush()` is pure logic - level, rise rate, clock and flags in, a pump
decision out - so it runs on a laptop with no ESP32 attached.

**Windows** (PowerShell) - finds whichever compiler you have:

```powershell
.\run.ps1
```

**Linux / macOS / WSL / Git Bash with gcc:**

```sh
./run.sh
```

`run.ps1` tries g++, then clang++, then WSL, then MSVC `cl`, and tells you what
to install if it finds none. If you would rather not install a compiler at all,
Claude can run the suite in its own sandbox - the repo is mounted there.

Exit codes: `0` all good, `1` a regression, `2` known bugs still present,
`3` build failure.

`stubs/` fakes just enough of Arduino, WiFi, PubSubClient, ezTime and the ESP
IDF headers to compile `src/main.cpp` on the host. The clock is fake and
test-controlled (`g_millis`, `g_now`), so time-dependent behaviour is
deterministic - no sleeping, no flakiness.

Tests marked **KNOWN BUG** encode real misbehaviour observed in Home Assistant
history and are expected to fail until the logic is fixed. They are the
regression net for that fix; when one starts passing it reports as `FIXED`.

`config.h` here is only a fallback for a fresh clone - if you have a real
`src/config.h` the quote-include finds that first. No credentials are printed.
