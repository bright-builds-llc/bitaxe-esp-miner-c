# Reference image inputs

`tools/build_reference_image.sh` builds the reference image, the one the owner's signed firmware
package carries, from a pushed commit. Everything it reads is in this tree, so the commit is the
image's Corresponding Source.

- `trust/`: the deployment trust and the two boards' signed capabilities that the image embeds. They
  hold only public keys and owner-signed claims. The capabilities name the firmware version, the
  board and the Worker Serial manifest, not a commit, so they can sit in the tree they describe. A
  new firmware version needs new capabilities, signed by the owner and committed here, before its
  build.
- `notices/`: the MIT notice for content moved from the Rust implementation, and the npm dev
  packages whose code still ships in the web UI. `tools/third_party_notices.py` reads both.

Before an image goes to anyone but the owner, publish its source release: tag the commit, and put
the build's `SOURCE.txt`, `dependencies.lock` and `THIRD_PARTY_NOTICES.txt` beside the image, where
whoever receives the image can find them. Keep the commit and the release reachable for as long as
the image is offered.
