# Development Workflow

This page defines the expected local workflow before opening a pull request.

## 1) Fork and create a focused branch

- Fork the repository to your own GitHub account
- Clone your fork locally and add the upstream repository if needed
- Enable repo hooks once per clone: `git config core.hooksPath .githooks && chmod +x .githooks/pre-commit`

- Branch from `develop`
- Keep each PR focused on one fix or feature area

## 2) Implement with scope in mind

- Confirm your idea is in project scope: [SCOPE.md](../../SCOPE.md)
- Prefer incremental changes over broad refactors

## 3) Run local checks

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

CI enforces formatting, static analysis, and build checks.
Use clang-format 21+ locally to match CI.
If `clang-format` is missing or too old locally, see [Getting Started](./getting-started.md).

## 4) Open the PR

- Target `develop` (the repository's default branch)
- Use a Conventional Commit title with a release type prefix. The prefix is
  required by the release tooling; a title without one is rejected.
- Supported release types are: `feat`, `fix`, `docs`, `style`, `refactor`,
  `perf`, `test`, `build`, `ci`, `chore`, and `revert`.
- Use the form `<type>: <short description>`, for example:
  `fix: avoid crash when opening malformed epub`
- Fill out `.github/PULL_REQUEST_TEMPLATE.md`
- Describe the problem, approach, and any tradeoffs
- Include reproduction and verification steps for bug fixes
- Be transparent about AI assistance by completing the **AI Usage** section
  in the pull request template (`YES`, `PARTIALLY`, or `NO`).
- When applicable, use wording such as:
  `YES — an AI coding agent assisted with diagnosis, implementation,
  verification, and PR drafting; the behavior was verified on X4 Pro
  hardware.` If hardware verification is still pending, say so explicitly.

## 5) Review etiquette

- Be explicit and concise in responses
- Keep discussions technical and respectful
- Assume good intent and focus on code-level feedback

For community expectations, see [GOVERNANCE.md](../../GOVERNANCE.md).
