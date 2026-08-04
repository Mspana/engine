# Docs

We should document what we're doing in the docs folder. Emphasis on
principles, risks, and important knowledge for the future, rather than a
play-by-play of what we are doing.

A reader six months from now should learn *why* things are the way they are
and what will bite them if they change something — not a diary of edits (git
history already is one).

## Layout

- `architecture/` — how shipped systems work, the decisions behind them, and
  their known risks. Update these when behavior changes conceptually.
- `design/` — plans and RFCs for things being built or considered.
- `ideas/` — early, uncommitted exploration.
- `backlog/`, `notes/` — running thoughts and future work.
- `guides/` — practical how-tos (API keys, setup).
- `archive/` — superseded docs kept for history; don't update, replace.
- `claude-memory/` — snapshot of assistant memory taken for the PC migration;
  archival, the live copy lives outside the repo.
- `external-references/` — captured third-party material (sample requests,
  protocol schemas).
