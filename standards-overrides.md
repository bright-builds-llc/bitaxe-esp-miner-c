# Standards Overrides

Use this file to record deliberate deviations from the canonical coding and architecture standards.

## Active overrides

| Standard | Local decision | Rationale | Owner | Review date |
| --- | --- | --- | --- | --- |
| File length limit | Files inherited from upstream are exempt, listed in `.bright-builds-rules-checks.tsv` | Splitting them would widen the difference from upstream and turn merges into conflicts | Peter Ryszkiewicz | 2027-04-30 |
| All standards, in `main/` and `components/` | Upstream's conventions apply to upstream files and to hooks in them; these standards apply to fork-owned components | The fork's goal is the smallest difference from upstream (`AGENTS.md`, Bright Builds Fork) | Peter Ryszkiewicz | 2027-04-30 |

## Notes

- Prefer narrow, explicit exceptions over broad "this repo is different" statements.
- If local verification is intentionally hook-owned or leaves heavy suites to CI, record that explicitly here.
- Revisit overrides periodically instead of letting them become permanent by accident.
- If an override becomes common across many repos, move it back upstream into the canonical standards repo.
