# Repo Hygiene And Release Plan

Goal:
Turn the current extension-template-derived repo into a clean project that can be shared, built, tested, and released without confusing template residue.

## Current Hygiene Gaps

- `docs/README.md` still documents the generic DuckDB extension template.
- `docs/NEXT_README.md` still references the old `quack` example.
- `.github/workflows/ExtensionTemplate.yml` is template self-test residue.
- `README.md` includes a local testing path for `test/data/events.parquet`, which does not currently exist.
- Some project docs are untracked in the current worktree.
- Large local data files exist under `data/`; only the intended fixtures should be tracked.
- The DuckDB submodule and workflow target versions need to stay aligned.

## Documentation Cleanup

Keep:

- `README.md`: short user-facing overview, install/load/use, V1 support matrix, limitations.
- `docs/duckdb_ast_parsing_plan.md`: implementation plan for AST analysis.
- `docs/v1_contract_plan.md`: V1 behavior and semantics.
- `docs/validation_and_benchmarking_plan.md`: validation approach.
- `docs/repo_hygiene_and_release_plan.md`: this file.
- `docs/UPDATING.md`: only if it remains accurate for this project.

Remove or rewrite:

- Generic template docs that describe `waddle`, `quack`, or creating a new extension.
- Template-only workflow docs.
- Any README examples that reference missing files.

## Data Policy

Decide which fixture files belong in git:

- Small deterministic fixtures generated inside SQLLogic tests should be preferred.
- One realistic Parquet file can be checked in if it catches bugs that synthetic data misses.
- Large exploratory datasets should remain ignored.

Document each tracked file under `data/`:

- Why it exists.
- Approximate size.
- Which tests or benchmarks use it.
- How to regenerate or replace it.

## Workflow Cleanup

Review GitHub workflows:

- Keep the main DuckDB extension distribution workflow if release artifacts are planned.
- Remove template self-tests that only apply to `duckdb/extension-template`.
- Ensure DuckDB version in workflow matches the checked-out submodule target.
- Keep code quality checks if they are stable for this extension.

## Release Readiness

Before a V1 tag:

- README documents V1 accurately.
- SQLLogic tests cover the declared support matrix.
- Remote benchmark script has at least one documented successful run.
- Extension builds against the pinned DuckDB version on supported platforms.
- No template example names remain in project docs.
- No accidental large local data files are staged.
- License and attribution are clear.

## Distribution Options

Short term:

- Local loadable extension from `build/release/extension/dryrun/dryrun.duckdb_extension`.
- GitHub Actions artifacts from the distribution workflow.

Later:

- DuckDB community extension submission if the API and behavior are stable enough.
- Custom extension repository only if there is a concrete need.

## Implementation Sequence

1. Replace or remove template docs.
2. Fix README examples to use real files or generated test data.
3. Remove `.github/workflows/ExtensionTemplate.yml` if it is not needed.
4. Add a short `docs/data.md` if any data files are tracked.
5. Verify `.gitignore` allows only intentional fixtures.
6. Confirm submodule and workflow DuckDB versions match.
7. Create a release checklist in `docs/release_checklist.md` once V1 scope is locked.

## Open Questions

- Should `docs/README.md` be deleted, or rewritten as contributor build docs?
- Which Parquet fixture should be the canonical realistic local test file?
- Is community extension distribution a V1 goal or a post-V1 goal?
- Which platforms are worth supporting before the extension has external users?
