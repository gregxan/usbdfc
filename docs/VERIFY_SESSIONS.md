# Verification round 2: how to run it

Round 1 (`docs/VERIFY_PROMPT.md`, areas 1–3) is done. Its findings F1–F11
are fixed in `69e2796` (docs/REQUIREMENTS.md section 8b). Round 2 is split
into two independent sessions.

| Session | Prompt | Covers | Suggested model |
|---|---|---|---|
| A | `docs/VERIFY_PROMPT_A.md` | Re-check of F1–F11; SOF generation; dual-role TinyUSB; mailbox; clocks, watchdog and boot | Fable: cross-core concurrency and timing |
| B | `docs/VERIFY_PROMPT_B.md` | Magnetometer state machine; test target; Python tools | A lower-cost model is likely enough; Fable if you prefer |

To keep token use low:
- **Start each session fresh.** Don't continue the round-1 session: its
  context is large, and its copy of `src/usbh.c` is out of date.
- **Run A and B in any order, or both at once.** They don't depend on each
  other.
- **Paste the starting message below as the first message,** with nothing
  else. The prompt files hold the scope, the file lists, what is already
  verified and the report format.

## Starting message for session A

```text
Clone https://github.com/gregxan/usbdfc (branch main, commit 69e2796 or
later). Do not clone submodules recursively; fetch only:
  git submodule update --init --depth 1 lib/pico-sdk lib/Pico-PIO-USB
  git -C lib/pico-sdk submodule update --init --depth 1 lib/tinyusb
Then read docs/VERIFY_PROMPT_A.md and carry it out exactly. Stay within its
file list and its "Already verified" section, and don't install a toolchain
or build unless that prompt allows it. Don't modify or push anything.
Give the report in this chat, in the format the prompt specifies.
```

## Starting message for session B

```text
Clone https://github.com/gregxan/usbdfc (branch main, commit 69e2796 or
later). Do not clone submodules recursively; fetch only:
  git submodule update --init --depth 1 lib/pico-sdk
  git -C lib/pico-sdk submodule update --init --depth 1 lib/tinyusb
Then read docs/VERIFY_PROMPT_B.md and carry it out exactly. Stay within its
file list and its "Already verified" section. Don't install a toolchain or
build the firmware. Python host tests are allowed only where the prompt
says. Don't modify or push anything. Give the report in this chat, in the
format the prompt specifies.
```

## Afterwards

Bring both reports back to the implementing session. Findings are numbered
A-n and B-n, so they won't clash with round 1's F-n.
