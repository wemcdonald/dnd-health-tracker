# Wire format — moved

The device wire format and the full firmware↔server contract (HP feed, OTA,
config) now live in one authoritative spec:

**→ [`../docs/firmware-contract.md`](../docs/firmware-contract.md)**

Note: the current feed line is 4 ints — `<cur> <max> <temp> <age>` — not the
older 2-int `<lit> <age>` this file used to describe. See the contract.
