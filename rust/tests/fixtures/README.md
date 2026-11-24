# Test fixtures

A copy of the C# program's `win/tests/DDM32Plus.Core.Tests/Fixtures` (2026-11-24), so the Rust tests keep working
once `win/` is deleted. The Rust tests read only this folder. The `make_*.py` scripts recreate the files from the
Python tools (synthetic inputs; no radio, no serial port). Do not hand-edit the result files.
