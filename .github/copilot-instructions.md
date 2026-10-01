# AI Guidelines for snmalloc

## Working Style

- Read the relevant code before proposing or making changes. Keep changes
  focused, preserve existing structure where practical, and avoid unrelated
  renames or refactoring.
- Use a written plan for genuinely multi-stage work or when the user requests
  one. Store repository plans in `PLAN.md`, and obtain approval before
  implementing a plan that introduces significant design choices.
- Match validation effort to the risk and scope of the change. Establish a
  relevant baseline before broad or high-risk work; small changes may use
  targeted validation. Use CI or `origin/main` when failure attribution is
  unclear.
- Complete approved work through validation and a bounded self-review before
  reporting it done. Use independent review when the breadth, risk, or
  complexity of the change warrants it.
- Report incidental findings separately unless they are necessary to make the
  requested change correct. Challenge instructions or assumptions when
  evidence contradicts them.

## Debugging

- Allocator tracing must use `write()` directly to stderr or a file rather
  than `printf` or `message`, which may recurse through the allocator.
- Verify hypotheses before implementing a workaround. Inspect the actual data,
  write a minimal reproducer, or compare against a known-good revision.
- Start by checking new or changed code, but do not claim that `main` is broken
  from a local failure alone. Check CI and, when useful, reproduce on
  `origin/main`.
- When a bug pattern may occur more than once, search the complete change for
  other instances and fix the relevant occurrences together.

## Code Quality

- Prefer designs simple enough that their correctness is evident. Do not
  mistake the absence of an obvious bug in a complicated design for evidence
  that the design is correct.
- Use the cross-platform macros from `ds_core/defines.h` rather than raw
  compiler attributes such as `__attribute__((used))` or `__forceinline`.
- Do not encode platform assumptions such as a fixed virtual-address width or
  maximum allocation derived from one current platform.
- Trust existing API-boundary bounds checks. Internal code should defer edge
  cases to the backend rather than adding redundant checks.
- New caches and intermediate data structures must safely bypass inputs outside
  the range they handle.
- Keep headers minimal and include only direct dependencies.
- Do not depend directly on the C++ standard library. Use C headers such as
  `<stdint.h>` and `<stddef.h>`, or the wrappers in `src/snmalloc/stl/`.
- Prefer explicit wiring and conversions over hidden dependencies or
  convention-based behaviour.
- Document behavioural coupling at the component whose modification could
  break the dependency.
- Design code to be easy to change, but do not add extension points or
  abstractions for hypothetical requirements.

## Change Discipline

- Avoid over-engineering and review churn. Preserve names and function
  boundaries unless changing them improves correctness or clarity.
- Evaluate copied implementation patterns in their new context rather than
  reproducing incidental choices.
- Update comments, tests, and documentation made stale by the change.
- Comments and documentation describe current behaviour, not the history of
  the change.

## Building, Testing, and Benchmarking

Follow `.github/skills/building_and_testing.md`. Select validation based on the
affected behaviour and report commands and failures factually.
