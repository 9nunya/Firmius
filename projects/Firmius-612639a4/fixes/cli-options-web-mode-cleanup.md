# CLI Options and Web Mode Transition

## The Problem
The `cli` package was failing to compile with a "no match for operator=" error in `main.cpp`. This occurred because the `parseCliOptions` function returned a `CliOptions` struct that was structurally different from the one defined in `CliOptions.hpp` due to incomplete removal of "Web mode" logic.

Specifically, the `CliOptions` struct still contained a `Mode` enum with `Tui` and `Web` variants, but the parser was being updated to remove web-related flags, leading to an architectural mismatch.

## The Investigation
- Inspected `packages/cli/include/CliOptions.hpp` and found the `Mode` enum.
- Inspected `packages/cli/src/main.cpp` and noticed it was trying to assign the result of `parseCliOptions` to a `CliOptions` instance.
- Found that while the TUI is the primary interface, residual code for a web server mode (likely from a legacy architecture or abandoned feature) was cluttering the CLI entrypoint.

## The Plan
1. **Remove `Mode::Web`**: Simplify the `CliOptions` struct to only support TUI mode.
2. **Cleanup Parser**: Remove all references to `--web` or web-related flags in `parseCliOptions`.
3. **Align `main.cpp`**: Ensure the entrypoint only initializes and runs the TUI.
4. **Decommission Web Path**: Acknowledge that the CLI package is now TUI-exclusive.

## The Fix
- Modified `CliOptions.hpp` to remove `Mode::Web`.
- Simplified the `Mode` enum to only contain `Tui`.
- Cleaned up `parseCliOptions` to remove the unreachable web mode switch.
- Verified that `main.cpp` correctly passes options to `firmius::tui::runTui`.

## Verification
- Built the `firmius` target using CMake.
- Confirmed the "no match for operator=" error was resolved.
- Verified that the CLI correctly parses `-c`, `--prompt`, and `--permission-mode` flags.
