# Paige (and !Write)

!Write's text engine is Paige (GNU LGPL 2.1), from
[HERMES-Paige](https://github.com/nmatavka/HERMES-Paige) at commit **cddd954**,
with the BOX's changes as one patch:

* `ports/paige/patches/engine-32bit-and-platform.patch` -- every change the BOX
  makes to Paige's own files (32-bit and 64-bit fixes, the RTF and HTML codecs,
  the platform hooks).
* `ports/paige/rosc/`, `ports/paige/PGPLATFO/` -- the BOX's platform layer for
  Paige.
* `ports/paige/build.sh` -- clones Paige at that commit, applies the patch and
  builds the library.

!Write is linked with Paige into one program, so its own source is here too, so
that it can be built again with a changed Paige: `ports/write/` (the BOX's code,
MIT).
