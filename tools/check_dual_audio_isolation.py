#!/usr/bin/env python3
"""Structural check for the "dual-audio-server" test category (see finlink's
test-categorization project notes): confirms finlink's GamePad-audio
forwarding hook lives *only* inside the DRC (GamePad)-specific audio path
in ax_out.cpp, never in the TV-specific path -- i.e. a connected finlink
client only ever intercepts the Wii U GamePad's own audio output, the TV
output keeps playing locally completely untouched.

This isn't something a runtime unit test can usefully prove: WiiuGamepad
Stream.cpp pulls in Cafe/HW/Latte/Renderer.h (the GPU renderer) and the rest
of Cemu's HLE audio mixer, so exercising it for real needs a fully booted
Wii U title (see finlink's own [[finlink-rom-boot-test-initiative]] memory
note -- deferred as a separate initiative). The separation this check cares
about is architectural, not data-dependent, though: AX_DEV_DRC and AX_DEV_TV
are two structurally distinct audio devices in Cemu's own AX HLE, submitted
via two entirely separate functions (AIInitDRCDMA vs.
AIInitDMA/AXOut_SubmitTVFrame) -- so a static source check of *which
function* references the finlink hook is a complete, sound proof of the
"only GamePad audio, TV unaffected" property, no execution required.

Exit code is non-zero (with the offending line printed) if a finlink/
Cemu::FinlinkStream reference is ever found inside a TV-audio function, or
if the DRC-audio function stops referencing finlink at all (the mirror-image
mistake: the hook silently regressing away).
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
AX_OUT_CPP = REPO_ROOT / "src" / "Cafe" / "OS" / "libs" / "snd_core" / "ax_out.cpp"

# Deliberately the specific load-bearing symbol, not a loose "Finlink"
# substring match: AIInitDRCDMA's own "consumedByFinlink" local variable
# name contains "Finlink" too, which would make a substring match falsely
# still pass even with the actual g_wiiuGamepadStream/SubmitGamepadAudio()
# call removed and only that now-inert bool left behind (caught by this
# script's own regression-injection self-test during development).
FINLINK_RE = re.compile(r"g_wiiuGamepadStream|SubmitGamepadAudio")

# (function name, must reference finlink?) -- ax_out.cpp's own function
# names for each audio device's submit path, see the module docstring.
FUNCTIONS = [
    ("AIInitDMA", False),        # TV audio (mono/stereo path)
    ("AXOut_SubmitTVFrame", False),
    ("AIInitDRCDMA", True),      # GamePad audio -- finlink's hook belongs here
    ("AXOut_SubmitDRCFrame", False),  # feeds AIInitDRCDMA, no hook of its own
]

FUNC_START_RE = re.compile(r"^\s*void (\w+)\(")


LINE_COMMENT_RE = re.compile(r"//.*$", re.MULTILINE)
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)


def strip_comments(code: str) -> str:
    """Comments must not count as a "reference" either way: ax_out.cpp's own
    prose (e.g. "see AIInitDRCDMA's own comment") can easily still mention
    SubmitGamepadAudio()/g_wiiuGamepadStream by name even after the actual
    call is removed, which would otherwise make this check pass right
    through the exact regression it exists to catch (found via this
    script's own regression-injection self-test during development, not
    theoretical)."""
    return LINE_COMMENT_RE.sub("", BLOCK_COMMENT_RE.sub("", code))


def extract_function_bodies(text: str) -> dict:
    """Returns {function_name: body_text} for every brace-delimited function
    in FUNCTIONS, by tracking brace depth from the function's opening line.
    Deliberately simple (no full C++ parser) -- ax_out.cpp's functions here
    are plain, un-nested-in-anything-else top-level functions, so naive
    brace counting is enough and stays easy to follow."""
    lines = text.splitlines()
    wanted = {name for name, _ in FUNCTIONS}
    bodies = {}
    i = 0
    while i < len(lines):
        match = FUNC_START_RE.match(lines[i])
        if match and match.group(1) in wanted:
            name = match.group(1)
            depth = 0
            started = False
            body_lines = []
            j = i
            while j < len(lines):
                depth += lines[j].count("{") - lines[j].count("}")
                if "{" in lines[j]:
                    started = True
                body_lines.append(lines[j])
                j += 1
                if started and depth == 0:
                    break
            bodies[name] = "\n".join(body_lines)
            i = j
        else:
            i += 1
    return bodies


def main() -> int:
    if not AX_OUT_CPP.is_file():
        sys.exit(f"error: {AX_OUT_CPP} does not exist")

    text = AX_OUT_CPP.read_text(encoding="utf-8")
    bodies = extract_function_bodies(text)

    failures = []
    for name, should_reference_finlink in FUNCTIONS:
        body = bodies.get(name)
        if body is None:
            failures.append(f"{name}: function not found in {AX_OUT_CPP} (renamed/removed upstream?)")
            continue
        references_finlink = bool(FINLINK_RE.search(strip_comments(body)))
        if references_finlink and not should_reference_finlink:
            failures.append(
                f"{name}: references finlink but is a TV-audio path -- "
                f"a connected finlink client must never intercept TV audio, only GamePad/DRC audio"
            )
        if should_reference_finlink and not references_finlink:
            failures.append(
                f"{name}: expected to reference finlink (this is where GamePad-audio forwarding "
                f"hooks in) but doesn't -- did the hook get moved or dropped?"
            )

    if failures:
        print("Dual-audio-server isolation check failed:\n")
        for failure in failures:
            print(f"  - {failure}")
        return 1

    print(f"OK: finlink's GamePad-audio hook is confined to AIInitDRCDMA, TV audio path is untouched.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
