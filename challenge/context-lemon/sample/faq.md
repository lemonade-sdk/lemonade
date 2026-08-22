# FAQ

**Q: What port does the Nightingale gateway listen on by default?**
A: 7913. It can be overridden with `NIGHTINGALE_GATEWAY_PORT`.

**Q: How long can a device be offline before Nightingale falls back to
cloud inference?**
A: 90 seconds.

**Q: Who is the lead engineer on Project Nightingale?**
A: Priya Okonkwo-Reyes.

**Q: When did the Talon Cache ship, and what bug was fixed in the
release right after it went public?**
A: Talon shipped in `nightingale-0.4.0` (2025-02-10). The first bug fix
release after the public beta was `nightingale-0.6.2` (2025-09-18),
which fixed a double-free race in the Talon eviction thread
(INCIDENT-2241).

**Q: Is Project Nightingale open source?**
A: No — it ships under the Aeroflux Systems Internal Use License v2.
