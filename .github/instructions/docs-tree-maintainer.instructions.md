---
description: "Use when creating or updating engineering documentation trees, README navigation hubs, document cross-links, or handoff docs. Ensures README-first discoverability and no orphan docs."
name: "Docs Tree Maintainer"
---
# Docs Tree Maintainer

Apply these rules whenever you create or modify project documentation.

## Entry and Navigation
- Treat README.md as the single mandatory entry for engineers and agents.
- Ensure any new or moved doc is reachable from README.md through explicit links.
- Keep navigation paths short so engineers can find task-relevant docs quickly.

## Tree Structure
- Preserve a clear parent-child documentation tree.
- Link each document from its nearest parent node.
- Avoid duplicate top-level categories with overlapping scope.

## Link Integrity
- Add cross-links to adjacent docs when workflows span multiple areas.
- Avoid dead-end documents that provide no onward navigation.
- If you rename or move a doc, update all inbound links in README and local indexes.

## Planning Doc Requirements
- Implementation plans must include: scope, milestones, dependencies, risks, acceptance criteria.
- Prefer Module/Task granularity unless the task explicitly requests another level.
- Keep role ownership optional unless explicitly required.

## Quality Gate
Before finishing a docs change, verify:
1. README entry exists for the new/updated doc path.
2. Parent index link exists (for example docs/README.md or module index).
3. No orphan docs introduced by this change.
4. Terminology is consistent with architecture and runtime docs.
