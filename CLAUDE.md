# Riffsmith FM-1 firmware (fork of M-PYRE)

Project context, history, decisions and TODO: `~/Library/Mobile Documents/com~apple~CloudDocs/Projetos/riffsmith/CONTEXT.md`
(iCloud Drive > Projetos > riffsmith). Read it before working here; update it when you finish.

- Riffsmith's changes: `firmware/src/eng_guitar.c`, the JANGL/SKANK amps (`tools/gen_mpyre_amp.py`, `firmware/src/amp.c`),
  `tools/prep_riffsmith_kit.py`, `tests/riffsmith_demos.py`; see CHANGELOG.md
- Never flashed yet: build per BUILDING.md (JieLi toolchain + Docker on macOS), keep M-UPGRADE + official V15 for recovery
- Keep the GPL notices: add "Modified for Riffsmith" to the header of any upstream file you change
- Sister repo: `~/Dev/riffsmith` (the JUCE plugin)
