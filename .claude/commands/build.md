Run the project build. Follow these steps exactly:

1. Check that a Makefile exists in the project root. If it does not, say so and stop.

2. Run the build with warnings enabled:
   ```
   make -j$(nproc) CFLAGS="-Wall -Wextra -O2"
   ```
   If the Makefile defines its own CFLAGS, run `make -j$(nproc)` without overriding.

3. If the build produces errors or warnings, show them as a compact table:
   - Column 1: file:line
   - Column 2: warning/error type
   - Column 3: message (truncated to 60 chars)
   Fix all warnings before declaring the build clean.

4. If the build succeeds with zero warnings, run the smoke test:
   ```
   make test
   ```
   or, if no `test` target exists, run the primary binary with `--selftest` or `n=256` as a minimal correctness check.

5. Report result in one line: `BUILD OK — zero warnings` or `BUILD FAILED — N warnings, M errors`.

6. Update STATUS.md Component Status for any file that changed status.

Target architectures: gfx942 (MI300A) for production, gfx1100 (6900XT) for GPU dev.
Use `--offload-arch=gfx942` or `--offload-arch=gfx1100` as appropriate.
