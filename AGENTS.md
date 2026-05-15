# Project Rules

- **All settings must persist to JSON** — every new setting added must be saved in `SaveSettings()` and loaded in `LoadSettings()` so it survives game reloads.
- Build with MinGW: `cmake -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=toolchain-mingw.cmake"` then `cmake --build build --config Release`
- **Git push:** Use `git push` — credentials are stored in `.git/config` of this repo.
