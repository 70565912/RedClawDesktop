# GitHub Directory Agent Guide

Applies to `.github/**`.

## Scope

This directory owns repository automation and AI-assist metadata:

- `workflows/`: GitHub Actions CI and packaging scaffolds.
- `instructions/`: Copilot/custom instruction files.
- `prompts/`: reusable planning prompts.
- `ISSUE_TEMPLATE/` and `PULL_REQUEST_TEMPLATE.md`: contribution intake.

## Workflow Rules

- Keep the Windows VS2022 + vcpkg + CTest unit baseline intact unless a task explicitly changes CI gates.
- Unit CI should build `redclaw_tests_unit` or otherwise exclude integration/e2e tests with `_integration_tests|_e2e_tests`.
- Do not require secrets for normal pull request unit validation.
- Keep workflow triggers narrow enough to avoid unnecessary installer or docs jobs.
- Prefer explicit PowerShell commands on Windows runners.

## Documentation Linkage

- If adding or removing Copilot agents, prompts, or instructions, update `README.md` and `docs/README.md` so their navigation matches actual files.
- If changing PR requirements, keep `.github/PULL_REQUEST_TEMPLATE.md`, `CONTRIBUTING.md`, and root `AGENTS.md` consistent.

## GitHub Operation Safety

- Do not run networked GitHub operations from local tooling unless the user has explicitly confirmed them in the current task.
- Do not introduce workflows that publish artifacts, upload logs, or expose runtime evidence containing secrets without redaction.
