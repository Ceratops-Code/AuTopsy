# AuTopsy

## Upstream updates

- [FORK-01] When adding or changing AuTopsy functionality, deliver all requested
  behavior while keeping feature state and logic in owned modules behind narrow
  adapters to stable Audacity interfaces; reuse native editing contracts rather
  than duplicating editing engines.
- [FORK-02] Before accepting an AuTopsy feature, test its behavior independently
  of Audacity internals, test required adapter contracts against audacity3
  separately, and fix avoidable coupling exposed by plausible upstream changes.
- [FORK-03] Confine upstream-specific names and structural assumptions to
  integration adapters; select targets by semantic identity rather than
  position and reject missing or ambiguous contracts.
