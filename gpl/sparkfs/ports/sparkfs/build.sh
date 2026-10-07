#!/bin/sh
# build.sh -- SparkFS for the box: RISC OS's SparkFS module, SparkLib, and
# its ten codecs (FileSys/ImageFS/SparkFS/Codecs: SparkZip, SparkTar,
# SparkSpark, SparkLzh, SparkARJ, SparkCab, SparkCPIO, SparkZoo,
# SparkMcStuffit, SparkPackdDir), from third_party/SparkFS, as C modules
# for either box.
#
#   ARCH=x86_64  (default) the Intel box: x32 -- the C with capps.mk's x32
#                table, roscc's x32 link
#   ARCH=aarch64 the Apple Silicon box: A64X32 -- capps.mk's A64X32 table,
#                roscc's A64X32 link and its lint
#
# Run from rosgd/ (make sparkfs, make ARCH=aarch64 sparkfs).  The sources
# are third_party's (THIRD_PARTY, else beside the toolchain), read in place:
# each component's c/ and h/ staged under the build directory as the DDE
# would see them (c/name -> name.c, h/name -> name.h; SparkFS's h/SparkFS
# as Interface/SparkFS.h and SparkLib's h/ as SparkLib/, as their
# Makefiles export them), with this port's patches (patches/*.patch)
# applied there.  The ObjAsm each has is C here: runlink.c (SparkFS's
# s/runlink), sinterface.c (SparkLib's s/sinterface), zipinfo.c,
# tarinfo.c, sparkinfo.c, lzhinfo.c, arjinfo.c, cabinfo.c, cpioinfo.c,
# zooinfo.c, mcstuffitinfo.c, packddirinfo.c (each codec's s/info),
# rminfo.c (AsmUtils' Image_RO_Base).
# cmhg descriptions go through roscc's cmhg.py, without international
# help (CMDHELP None: the help text in the module).
#
# Output, under build/ports/sparkfs or build/aarch64/ports/sparkfs:
#   modules/SparkFS,ffa  and each codec's: Zip Tar Spark Lzh ARJ Cab CPIO
#                        Zoo McStuffit PackdDir (,ffa)
#   rom/Apps/!SparkFS    the application, for the ROM's Resources:$.Apps (the
#                        Makefile's resources rule; the modules are ROM images)
#   obj/                 the objects; src/ the staged sources
set -eu

# the codecs, as RISC OS Open's FileSys/ImageFS/SparkFS/Codecs
COMPS="SparkZip SparkTar SparkSpark SparkLzh SparkARJ SparkCab SparkCPIO SparkZoo SparkMcStuffit SparkPackdDir"

ROSGD=$(pwd)
HERE=$ROSGD/ports/sparkfs
case "${ARCH:-x86_64}" in
x86_64)  B=build;         ABI=x32 ;;
aarch64) B=build/aarch64; ABI=a64x32 ;;
*) echo "build.sh: ARCH is x86_64 or aarch64"; exit 1 ;;
esac
# roscc link --clib's runtime objects: the box's own, as capps.mk gives
# them (CAPPS_RT) -- roscc's default directory may hold older ones, whose
# module stub registers with the library but never initialises it
if [ $ABI = x32 ]; then
    export ROSCC_X32_RT=$ROSGD/build/gen/capps/rt
    RT=$ROSCC_X32_RT/modclib_x32.o
else
    export ROSCC_A64X32_RT=$ROSGD/build/a64x32/capps/rt
    RT=$ROSCC_A64X32_RT/modclib_a64x32.o
fi
W=$ROSGD/$B/ports/sparkfs
CAPPS=$ROSGD/build/$( [ $ABI = x32 ] && echo capps || echo a64x32/capps )
ROSCC=$ROSGD/../roscc/target/release/roscc
CMHG=$ROSGD/../roscc/tools/cmhg.py
TP=${THIRD_PARTY:-$ROSGD/../../third_party}
SRC=$TP/SparkFS

[ -d "$SRC/SparkFS" ] || { echo "build.sh: no SparkFS sources ($SRC; THIRD_PARTY names third_party)"; exit 1; }
[ -f "$CAPPS/inc/C/kernel.h" ] || { echo "build.sh: no $CAPPS/inc/C (make ARCH=${ARCH:-x86_64} capps)"; exit 1; }
[ -x "$ROSCC" ] || { echo "build.sh: no roscc ($ROSCC)"; exit 1; }
[ -f "$RT" ] || { echo "build.sh: no $RT (make ARCH=${ARCH:-x86_64})"; exit 1; }

# the C compiler and its flags: capps.mk's table for the ABI
LINE=$(make -s -f capps.mk ABI=$ABI capps-cflags)
case "$LINE" in
"zig clang "*) CC="zig clang"; CFLAGS=${LINE#zig clang } ;;
*)             CC=$ROSGD/${LINE%% *}; CFLAGS=${LINE#* } ;;
esac
# SparkFS's C is 1990s C: implicit int and declarations, K&R casts.  The
# warnings are its own, not the port's; only errors stop the build.
CFLAGS="$CFLAGS -w -Wno-error -Wno-implicit-function-declaration -Wno-implicit-int -Wno-int-conversion \
    -Wno-incompatible-pointer-types -Wno-return-type"

# SPARKFS_KEEP=1 builds the staged tree as it stands (to try a change in
# it before it is a patch)
if [ -z "${SPARKFS_KEEP:-}" ]; then
rm -rf "$W/src" "$W/obj"
mkdir -p "$W/src/inc/Interface" "$W/src/inc/SparkLib" "$W/obj" "$W/modules"

# ---- stage: c/x -> x.c, h/x -> x.h, the exports, then the patches ----
for comp in SparkFS SparkLib $COMPS SparkFSApp; do
    mkdir -p "$W/src/$comp"
    for f in "$SRC/$comp"/c/*; do cp "$f" "$W/src/$comp/$(basename "$f").c"; done
    for f in "$SRC/$comp"/h/*; do cp "$f" "$W/src/$comp/$(basename "$f").h"; done
    [ -d "$SRC/$comp/cmhg" ] && for f in "$SRC/$comp"/cmhg/*; do cp "$f" "$W/src/$comp/$(basename "$f").cmhg"; done
    [ -f "$SRC/$comp/VersionNum" ] && cp "$SRC/$comp/VersionNum" "$W/src/$comp/VersionNum"
done
cp "$W/src/SparkFS/SparkFS.h" "$W/src/inc/Interface/SparkFS.h"
for f in "$SRC"/SparkLib/h/*; do cp "$f" "$W/src/inc/SparkLib/$(basename "$f").h"; done
for p in "$HERE"/patches/*.patch; do
    [ -f "$p" ] || continue
    (cd "$W/src" && patch -s -p1 < "$p") || { echo "build.sh: $(basename "$p") does not apply"; exit 1; }
done
fi
mkdir -p "$W/obj/SparkFSApp" "$W/modules"

INC="-I$W/src/inc -I$HERE/inc -I$CAPPS/inc/C"

cc1() {     # cc1 <component> <source> <object>
    $CC $CFLAGS -I"$W/src/$1" -I"$W/obj/$1" $INC -c "$2" -o "$3"
}

cmhg1() {   # cmhg1 <component> <description>
    mkdir -p "$W/obj/$1"
    python3 "$CMHG" "$W/src/$1/$2.cmhg" -I "$W/src/$1" -DNO_INTERNATIONAL_HELP \
        -o "$W/obj/$1/$2.c" -d "$W/obj/$1/$2.h"
    cc1 "$1" "$W/obj/$1/$2.c" "$W/obj/$1/$2.o"
}

objs() {    # objs <component> <names...>: compiled, their objects echoed
    comp=$1; shift
    for n in "$@"; do
        cc1 "$comp" "$W/src/$comp/$n.c" "$W/obj/$comp/$n.o" >&2
        echo "$W/obj/$comp/$n.o"
    done
}

echo "=== SparkLib"
mkdir -p "$W/obj/SparkLib"
LIB_OBJS=$(objs SparkLib sarcfs sfs smsdos2 zbuffer zchunk zfile zflex zgeterror zmsdos zxxstr)
cc1 SparkLib "$HERE/sinterface.c" "$W/obj/SparkLib/sinterface.o"
# an archive, as the DDE's SparkLib is: a codec's own unit of a name
# (SparkTar's zfile) is taken before the library's
rm -f "$W/obj/SparkLib.a"
zig ar --format=gnu rcsD "$W/obj/SparkLib.a" $LIB_OBJS "$W/obj/SparkLib/sinterface.o"
LIB_OBJS="$W/obj/SparkLib.a"

echo "=== SparkFS"
cmhg1 SparkFS SparkFSHdr
FS_OBJS=$(objs SparkFS arcf arcs args atexit buffer callback cat close code crc32 \
    des2 dir ext file flex fs func fx gbpb getbytes image link mem modulewrap noco \
    open putbytes pw scrap sff sfi sfs sfserrs upcall xstr)
cc1 SparkFS "$HERE/runlink.c" "$W/obj/SparkFS/runlink.o"
cc1 SparkFS "$HERE/rminfo.c" "$W/obj/SparkFS/rminfo.o"
"$ROSCC" link --module --clib -o "$W/modules/SparkFS,ffa" "$W/obj/SparkFS/SparkFSHdr.o" $FS_OBJS \
    "$W/obj/SparkFS/runlink.o" "$W/obj/SparkFS/rminfo.o"

# codec <component> <module> <cmhg description> <info.c> <objects...>: a
# codec as its Makefile builds it (CModule: OBJS, ASMUTILS, SPARKLIB), its
# s/info the C file here
codec() {
    comp=$1 mod=$2 hdr=$3 info=$4; shift 4
    echo "=== $comp"
    cmhg1 $comp $hdr
    C_OBJS=$(objs $comp "$@")
    cc1 $comp "$HERE/$info" "$W/obj/$comp/info.o"
    cc1 $comp "$HERE/rminfo.c" "$W/obj/$comp/rminfo.o"
    "$ROSCC" link --module --clib -o "$W/modules/$mod,ffa" "$W/obj/$comp/$hdr.o" $C_OBJS \
        "$W/obj/$comp/info.o" "$W/obj/$comp/rminfo.o" $LIB_OBJS
}

codec SparkZip Zip ZipHdr zipinfo.c arcs cat convert deflate explode gzip inflate main pack trees \
    unpack unreduce unshrink
codec SparkTar Tar TarHdr tarinfo.c arcs cat convert main zfile
codec SparkSpark Spark SparkHdr sparkinfo.c arcfscat arcs buffer cat convert main pack sp1 spdata \
    sq unpack xpack
codec SparkLzh Lzh LzhHdr lzhinfo.c arcs cat convert crcio decode dhuf huf larc main maketbl \
    maketree pack shuf unpack
codec SparkARJ ARJ ARJHdr arjinfo.c arcs arjcat cat convert main pack unpack
codec SparkCab Cab CabHdr cabinfo.c arcs buffer cabcat cat convert gzip inflate main pack unpack
codec SparkCPIO CPIO CPIOHdr cpioinfo.c arcs cat convert main zfile
codec SparkZoo Zoo ZooHdr zooinfo.c arcs cat convert huf main pack unpack
codec SparkMcStuffit McStuffit McStuffitHdr mcstuffitinfo.c arcs cat com convert cpt crc16 \
    decompress de_lzah huffman maccat mactime main pack pit sit transname unpack
codec SparkPackdDir PackdDir PackdDirHdr packddirinfo.c arcs cat convert main pack PackCat unpack

CODECS="Zip Tar Spark Lzh ARJ Cab CPIO Zoo McStuffit PackdDir"
if [ $ABI = a64x32 ]; then
    "$ROSCC" a64x32-lint "$W/modules/SparkFS,ffa" $(for m in $CODECS; do echo "$W/modules/$m,ffa"; done)
fi

echo "=== SparkFSApp"
# SparkFS's desktop front end, Apps/SparkFSApp: a C application (CApp,
# CFLAGS -c90) compiled against RISC_OSLib's headers (RINC) but linked with
# the C library alone -- its Makefile gives no LIBS -- and its two ObjAsm
# files, s/swi and s/poll, which bring the RISC_OSLib veneers it calls,
# here appswi.c and apppoll.c.  roscc link --clib, as the box's other C
# applications: an image (&FF8) on the ROM's SharedCLibrary.
RLIBINC=$CAPPS/inc/RISCOSLIB
ABIINC=$ROSGD/abi/$ABI
APP_OBJS=""
for n in code codea2b codeuu codehqx compress deboo demime file fileract flex fs fsx info main md5 \
    memory mimecode mkboo mkmime mym ram sparkfs task trans wos wz; do
    $CC $CFLAGS -I"$W/src/SparkFSApp" $INC -I"$RLIBINC" -c "$W/src/SparkFSApp/$n.c" -o "$W/obj/SparkFSApp/$n.o"
    APP_OBJS="$APP_OBJS $W/obj/SparkFSApp/$n.o"
done
for n in appswi apppoll; do
    $CC $CFLAGS -I"$ABIINC" -I"$RLIBINC" -I"$CAPPS/inc/C" -c "$HERE/$n.c" -o "$W/obj/SparkFSApp/$n.o"
    APP_OBJS="$APP_OBJS $W/obj/SparkFSApp/$n.o"
done
"$ROSCC" link --clib -o "$W/obj/SparkFSApp/!RunImage,ff8" $APP_OBJS
if [ $ABI = a64x32 ]; then
    "$ROSCC" a64x32-lint "$W/obj/SparkFSApp/!RunImage,ff8"
fi

echo "=== !SparkFS"
# The application as SparkFSApp's Makefile installs it (INSTAPP_FILES and
# the four translations, each Messages with its _Version from VersionNum,
# Build:AwkVers), laid out as RISC OS 5.30's Utilities.!SparkFS, for the
# ROM: Resources:$.Apps.!SparkFS (the Makefile's resources rule takes
# rom/ as a root).  SparkFS and the codecs are ROM modules too (the
# Makefile's rom_spark*.c, made from modules/), so the application holds
# none: where 5.30's has SparkFS's module (Resources.SparkFS) there is
# nothing, !Run's RMEnsure starting the ROM's; where it has the codecs
# (Modules), an Obey file a codec, named as it is -- SparkFSApp lists the
# directory for Choices and starts each codec its Choices installs with
# WimpTask <SparkFS$Dir>.Modules.<codec>, which RMReInits the ROM's (a
# codec declares its archive types as image filing systems as it starts,
# once SparkFSImage 1 has said to).  This port's files (app/!SparkFS):
# !Run -- SparkFSApp's, with the lines that run ARM code or load a module
# changed, each saying why; !Help; Load, SparkFS set up at the command line
# without the desktop (as !Run and !RunImage set it up: test/codectest.py),
# and what it obeys, Resources.Extensions and Resources.NoCo --
# Config.Extensions and Config.NoCo as the *SparkFSExtension and
# *SparkFSNoCo commands !RunImage makes of them.  Config.Choices installs
# all ten codecs (SparkFSApp's installs Spark, Tar and Zip; the box's
# !SparkFS reads every archive type without a visit to Choices).  Every
# file is dated SPARKFS_EPOCH (the Makefile's: the port's last commit), so
# the ROM is the same from one build to the next.
rm -rf "$W/!SparkFS"                    # the disc application this used to make
APP="$W/rom/Apps/!SparkFS"
AR="$SRC/SparkFSApp/Resources"
rm -rf "$W/rom"
mkdir -p "$APP/Modules" "$APP/Resources" "$APP/Config" "$APP/AutoRun" "$APP/Licences"
cp "$AR/!Boot,feb" "$APP/"
cp "$HERE/app/!SparkFS/"* "$APP/"
cp "$W/obj/SparkFSApp/!RunImage,ff8" "$APP/"
cp "$AR/Sprites,ff9" "$AR/Sprites22,ff9" "$APP/"
cp "$AR/Sprites,ff9" "$AR/Sprites22,ff9" "$AR/ResFind,ffb" "$APP/Resources/"
cp "$AR/UK/Menu,ffd" "$APP/Resources/"
cp -R "$AR/Themes" "$APP/Themes"
cp "$AR/AutoRun/Zip,feb" "$APP/AutoRun/"
cp "$AR/Config/AtExit" "$AR/Config/Extensions" "$AR/Config/NoCo" "$APP/Config/"
tr -d '\r' < "$AR/Config/Choices" | awk '
    /^Install / { if (!done) { split("Spark Tar Zip Lzh ARJ Cab CPIO Zoo McStuffit PackdDir", m, " ");
                               for (i = 1; i <= 10; i++) print "Install " m[i] }
                  done = 1; next }
    { print }' > "$APP/Config/Choices"
VFULL=$(sed -n 's/^#define Module_FullVersion *"\(.*\)"/\1/p' "$SRC/SparkFSApp/VersionNum")
VDATE=$(sed -n 's/^#define Module_ApplicationDate *"\(.*\)"/\1/p' "$SRC/SparkFSApp/VersionNum")
for lang in France Germany Netherland UK; do
    cp -R "$AR/$lang" "$APP/Resources/$lang"
    # Build:AwkVers: the _Version line made, or added at the end
    awk -v v="$VFULL ($VDATE)" '/^_Version/ { print "_Version:" v; done = 1; next } { print }
        END { if (!done) print "_Version:" v }' "$AR/$lang/Messages" > "$APP/Resources/$lang/Messages"
done
for m in $CODECS; do
    printf '| !SparkFS.Modules.%s -- the %s codec, in the ROM: started again, as\n| loading it from here would start it afresh (SparkFSApp runs this to install it)\nRMReInit %s\n' \
        "$m" "$m" "$m" > "$APP/Modules/$m,feb"
done
for comp in SparkFS SparkLib $COMPS SparkFSApp; do
    cp "$SRC/$comp/LICENCE" "$APP/Licences/$comp"
done
tr -d '\r' < "$AR/Config/Extensions" | awk 'NF { print "SparkFSExtension " $0 }' \
    > "$APP/Resources/Extensions,feb"
tr -d '\r' < "$AR/Config/NoCo" | awk 'NF { print "SparkFSNoCo " $0 }' \
    > "$APP/Resources/NoCo,feb"
if [ -n "${SPARKFS_EPOCH:-}" ]; then
    python3 -c 'import os, sys
t = int(sys.argv[2])
for d, ds, fs in os.walk(sys.argv[1]):
    for n in fs + ds:
        os.utime(os.path.join(d, n), (t, t))' "$W/rom" "$SPARKFS_EPOCH"
fi
ls -l "$W/modules" "$APP"
