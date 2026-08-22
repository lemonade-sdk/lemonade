# Project Nightingale

Project Nightingale is the internal codename for Aeroflux Systems'
edge-inference gateway. It routes model requests across a fleet of
on-device accelerators and falls back to cloud inference only when a
device is offline for more than 90 seconds.

- **Lead engineer:** Priya Okonkwo-Reyes
- **First internal release:** March 14, 2024 (build tag `nightingale-0.1.0-alpha`)
- **Public GA date:** November 3, 2025
- **License:** Aeroflux Systems Internal Use License v2 (not open source)
- **Default gateway port:** 7913
- **Supported accelerator families:** Falcon-NPU, Kestrel-GPU, and the
  legacy Sparrow-CPU fallback path

See `architecture.md` for the routing design, `changelog.md` for the
release history, and `faq.md` for common support questions.
