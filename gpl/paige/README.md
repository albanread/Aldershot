# Paige and !Write

Paige (GNU LGPL 2.1) is the text engine of !Write. It is [HERMES-Paige](https://github.com/nmatavka/HERMES-Paige) at commit `cddd954`, plus:

- `ports/paige/patches/engine-32bit-and-platform.patch`: all of BOX's changes to Paige.
- `ports/paige/rosc/`, `ports/paige/PGPLATFO/`: BOX's platform layer.
- `ports/paige/build.sh`: clones, patches and builds Paige.

`ports/write/` is the source of !Write (BOX's code, MIT), which is linked with Paige.
