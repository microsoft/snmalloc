# Plan: Fix benchmark GitHub Actions job

## Context

The benchmark job with check run ID `94399445543` fails while Docker Buildx is
building `benchmark/Dockerfile`. The failing step is:

```text
RUN ./build-bench-env.sh packages
```

The Dockerfile clones `daanx/mimalloc-bench` at pinned commit
`a4ce904286365c7adfba54f0eea3a2df3fc95bd1`. At that commit,
`build-bench-env.sh` installs Bazel unconditionally on Ubuntu during the
`packages` phase by downloading `https://bazel.build/bazel-release.pub.gpg`,
which now returns HTTP 404. The failed download causes
`gpg: no valid OpenPGP data found` and exits the Docker build.

The current branch has been checked against `origin/main` and is one commit
ahead, zero commits behind.

Bazel is only needed by the external script when building Google's tcmalloc
(`tcg`). This Dockerfile does not build or benchmark `tcg`; it builds benchmark
workloads and runs them only against `/snmalloc/build/libsnmallocshim.so` via
`/allocs.txt`.

## Relevant principles

- Read the affected benchmark Docker path before modifying it.
- Make the smallest repository-local change that fixes the failing CI setup.
- Do not modify unrelated benchmark behavior or update the pinned
  `mimalloc-bench` revision unless necessary.
- Remove unused fragile setup from this benchmark image instead of replacing it
  with another external network dependency.
- Baseline the failing path before implementation and validate the fixed path
  after implementation as far as this environment permits.
- Delegate build/test validation to a testing subagent using
  `.github/skills/building_and_testing.md`.
- Do not commit changes until the user has reviewed and explicitly approved.

## Steps and gates

1. Baseline the relevant failure.
   - Gate: use the CI check-run log, and any locally reproducible evidence if
     network access permits, to confirm the pre-fix Docker setup failure is in
     `./build-bench-env.sh packages` on the Bazel install path.
2. Patch the cloned pinned `mimalloc-bench` script during the Docker build to
   skip the unnecessary Bazel install.
   - Gate: `benchmark/Dockerfile` still clones the same pinned commit and adds
     only a narrow, fail-loud patch in a new `RUN` layer after the pinned
     `git reset --hard` and before `./build-bench-env.sh packages`.
   - Gate: the patch targets only the standalone Ubuntu/Debian
     `aptinstallbazel` call site, not the `function aptinstallbazel`
     definition. Use an anchored pattern equivalent to
     `^[[:space:]]*aptinstallbazel[[:space:]]*$`.
   - Gate: the patch verifies the expected post-condition with correct exit
     semantics so future silent no-op substitutions fail immediately: if the
     standalone call pattern is still present, the Docker build prints a clear
     error and exits non-zero; otherwise it continues.
3. Decide focused-test coverage.
   - Gate: if existing test infrastructure can cheaply validate the Dockerfile
     patch, add a focused test; otherwise document why no repository test is
     appropriate for this CI-environment-only Docker setup fix.
4. Run targeted validation.
   - Gate: delegate a targeted Docker validation to a testing subagent and
     confirm the benchmark image progresses past the previous Bazel install
     failure, or capture any new failure separately. If local network
     restrictions prevent full Docker validation, document that the self-hosted
     benchmark runner is the final validation point. Later benchmark steps also
     fetch/build external projects, so additional upstream fragility may only
     surface on that runner.
5. Final checks.
   - Gate: run secret scanning on changed files, request automated code review,
     run CodeQL checker, address applicable findings, and report final status.
