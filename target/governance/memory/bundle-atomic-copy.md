---
name: bundle-atomic-copy
description: copy items into a running hold's bundle dir atomically (cp7 to ~/aac7q/ then mv into bundle7); a direct scp can be picked up half-written
metadata:
  type: project
---
A session hold polls ~/aac7q/bundleN/ and runs any *.sh it finds. A direct `bin/cp7 aac7 item.sh '~/aac7q/bundle7/'` into an
idle hold can be picked up while scp is still writing: 9z2_vmm3b (2026-10-22 08:01) ran as an empty file, "rc 0 after 0 s",
and its name was burned.

**Why:** names are never reused ([[aac7q-no-overwrite-running]]), so a race costs a name and a queue slot.
**How to apply:** `bin/cp7 aac7 item.sh '~/aac7q/'` then `bin/c7 aac7 'mv ~/aac7q/item.sh ~/aac7q/bundle7/'`; tell agents the same.
