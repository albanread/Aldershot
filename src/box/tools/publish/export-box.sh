#!/bin/sh
# export-box.sh DEST -- copy the source of BOX's /init to DEST (Aldershot's src/box).
#
# Run it from rosgd/. It copies the tracked files of the directories /init is built
# from, with the Makefile and the generators, and leaves out compiled RISC OS
# modules (,ffa files) the publication pass's briefs, and the ARM container's sprint log. DEST is
# emptied first, except its README.md (written by hand), so a file removed here is removed there. Nothing is committed or pushed.
set -eu
dest=${1:?usage: export-box.sh DEST}
[ -f Makefile ] && [ -d runtime ] || { echo "export-box: run from rosgd/" >&2; exit 2; }
case $dest in /|"") echo "export-box: bad DEST" >&2; exit 2;; esac
keep=$(mktemp)
[ -f "$dest/README.md" ] && cp "$dest/README.md" "$keep"
rm -rf "$dest"
mkdir -p "$dest"
[ -s "$keep" ] && cp "$keep" "$dest/README.md"
rm -f "$keep"
git ls-files runtime platform boot include modules api tools Makefile docs/publish \
    | grep -v ',ffa$' \
    | grep -v '^tools/publish/.*BRIEF' \
    | grep -v '^runtime/armrun/SPRINTS.md$' \
    | while IFS= read -r f; do
        mkdir -p "$dest/$(dirname "$f")"
        cp -p "$f" "$dest/$f"
    done
echo "export-box: $(find "$dest" -type f | wc -l | tr -d ' ') files in $dest"
