# Reference image inputs

`tools/build_reference_image.sh` builds the reference image, the one the owner's signed firmware
package carries, from a pushed commit. Everything it reads is in this tree, so the commit is the
image's Corresponding Source.

- `trust/`: the deployment trust and the two boards' signed capabilities that the image embeds. They
  hold only public keys and owner-signed claims. The capabilities name the firmware version, the
  board and the Worker Serial manifest, not a commit, so they can sit in the tree they describe. A
  new firmware version needs new capabilities, signed by the owner and committed here, before its
  build.
- `notices/`: the MIT notice for content moved from the Rust implementation, the terms of code and
  fonts vendored into the tree, the npm dev packages whose code still ships in the web UI, and the
  sections every notices file must hold. `tools/third_party_notices.py` reads them all.

Before an image goes to anyone but the owner, publish its source release: tag the commit, and put
the build's `SOURCE.txt`, `dependencies.lock` and `THIRD_PARTY_NOTICES.txt` beside the image, where
whoever receives the image can find them. Keep the commit and the release reachable for as long as
the image is offered.

## Inherited fonts

Two fonts the image carries come from upstream, and their terms may not allow the image to be
redistributed with them. The owner accepted this as a known risk on 2026-11-20, inheriting
upstream's position, since upstream's releases ship both the same way:

- Nippo (`main/http_server/axe-os/src/assets/fonts/Nippo-Regular.woff2`), the web UI's typeface, is
  under the ITF Free Font License v2.0, which permits use but forbids redistributing the font files
  or converting their format without the foundry's consent. The image carries the file itself.
- Angel Wish draws the "Bitaxe" logo on the display from seven glyphs converted to a bitmap
  (`main/images/fonts/ui_font_AngelWish40.c`). Its file (`main/images/fonts_originals/AngelWish.ttf`)
  says "All rights reserved" by Stringlabs and names no licence. A source release carries that file
  whole.

`notices/vendored.txt` records both, which grants nothing. `task-upstream-font-licences` in
`TASKS.md` tracks revisiting them: replace them (Angel Wish needs only seven glyphs; Nippo changes
the web UI's look, so it belongs upstream), ask the rights holders, or take advice.
