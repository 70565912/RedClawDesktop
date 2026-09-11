---
name: "Module Task Plan Template"
description: "Generate execution-ready Module/Task development plans and update README-first documentation navigation for engineers."
argument-hint: "Provide module id, objective, constraints, and target milestone"
agent: "Software Project Architecture Planner"
tools: [read, search, edit, todo]
---
Generate an execution-ready Module/Task plan for this project and keep docs navigable from README.md.

Inputs you should infer or ask for if missing:
- Module ID and objective
- Scope boundaries (in / out)
- Constraints and dependencies
- Target milestone or iteration

Required output structure:
1. Plan Summary
2. Scope (In / Out)
3. Task Breakdown (Module/Task granularity)
4. Dependency and Risk Register
5. Acceptance Criteria and Test Gates
6. Documentation Tree Update
7. Exact Files to Create or Edit
8. Review Checklist for handoff

Hard requirements:
- Keep README.md as navigation root.
- Include concrete file paths for documentation updates.
- Avoid strategy-only content; provide actionable engineering tasks.
- Keep role ownership optional unless user asks for role binding.
