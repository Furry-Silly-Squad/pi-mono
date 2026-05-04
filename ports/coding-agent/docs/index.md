# C++ Port Documentation

## Overview

This directory contains documentation for the `coding-agent` C++ port.

## Documents

### Analysis & Roadmap

- [C++ Port vs TypeScript Agent — Analysis & Roadmap](c++-port-agent-src-analysis.md) — Feature parity status, comparison with `packages/agent`, recommended implementation order

### Known Issues

- [Known Issues & Fix Directions](issues.md) — Tracked issues, their severity, and resolution status

### Phase Plans

Implementation phases for the C++ port, in order:

- [Phase 1: Compaction Foundation](phases/PHASE-1-compaction-foundation.md) — Stable entry IDs, iterative boundaries, structured summary prompts
- [Phase 2: Compaction Context Integrity](phases/PHASE-2-compaction-context-integrity.md) — Valid cut points, split-turn summaries, configurable failure policy
- [Phase 3: File Operation Tracking](phases/PHASE-3-fileops-and-summary-quality.md) — File-op extraction, carry-forward, branch summary footers
- [Phase 4: Branch Summary Model Parity](phases/PHASE-4-branch-model-parity.md) — Session tree model, branch traversal, LLM handoff
- [Phase 5: Interactive Token Feedback](phases/PHASE-5-interactive-token-feedback.md) — Token budget display, per-turn breakdown, `/stats` command
- [Phase 6: Agent Interruption & TUI Feedback](phases/PHASE-6-agent-interruption-and-tui-feedback.md) — Cancel flag, TUI animation, interrupt feedback
- [Phase 7: AgentSession Runtime Integration](phases/PHASE-7.md) — `AgentSession` class, removal of free-function loop, event system
- [Phase 8: SessionManager Tree Model](phases/PHASE-8.md) — Full tree model, 10 entry types, branching, labels, migration
- [Phase 9: Queue Management + Retry](phases/PHASE-9.md) — `steer()`/`followUp()` queues, auto-retry with exponential backoff
- [Phase 10: Self-Rebuild & Restart](phases/PHASE-10.md) — `/rebuild` command for in-place binary replacement
