# License

This repository, **lxst** (the LXST voice session on spangap: LXST frames,
msgpack, capture, encoding, batching, jitter, mixing and talk keys), is
released under the **Apache License, Version 2.0**.

Full license text: <https://www.apache.org/licenses/LICENSE-2.0>

Copyright (c) 2026 by reticulous project contributors.

## Third-party software

### Vendored in this repository

None.

### Build-time dependencies

Declared in `esp-idf/idf_component.yml` and `straddle.yaml`:

| Component / package | Source | License |
|---|---|---|
| ESP-IDF (platform) | espressif/esp-idf | Apache-2.0 |

The Opus and Codec2 coders are not in this repository. They come from the
spangap/audio straddle, which builds libopus (BSD-3-Clause) and libcodec2
(LGPL-2.1, as a library of its own so it can be relinked); their terms are
carried there.

LXST is developed by Mark Qvist (`markqvist/LXST`) under CC BY-NC-ND 4.0. No
LXST source code is used or adapted here: this implementation is written from
the LXST wire format and from partyline (`RFnexus/partyline`, Unlicense),
which documents that format in its own source.

The host test vectors are generated: `vectors.h` with the umsgpack encoder
vendored in RNS (MIT), `frame_vectors.h` by running partyline 1.0.5's frame
rules. They contain encoded data and numbers, not source.
