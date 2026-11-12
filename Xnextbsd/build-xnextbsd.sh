#!/bin/sh
# =============================================================================
# build-xnextbsd.sh: build Xnextbsd, the rootless X server on WindowServer
# =============================================================================
#
# Xnextbsd has to be compiled inside XLibre's source tree: the server's core
# libraries (dix, mi, fb, os) are never installed. And it has to be a build
# of its own: it draws through miext/rootless, which sets ROOTLESS in
# dix-config.h for the whole tree, so it is never built together with Xorg.
# So, from the pins in sources.lock (nextbsd/apps-xlibre#2):
#
#   1. fetch    the XLibre tarball nextbsd-xlibre pins (same URL and
#               SHA-256) and nextbsd-xlibre itself at a commit, read only;
#               each is refused unless its SHA-256 matches;
#   2. patch    nextbsd-xlibre's patches/xlibre-xserver series (the patches
#               Xorg is built with), then this repo's patches/xlibre-xserver
#               series (the -Dxnextbsd meson option), and copy this repo's
#               hw/nextbsd into the tree;
#   3. build    with -Dxorg=false -Dxnextbsd=true, with Xorg's font, XKB and
#               log paths (nextbsd-xlibre's build-xlibre.sh), against the X
#               libraries in /usr/X11 and libWindowServerClient;
#   4. stage    only the Xnextbsd binary, as $DESTDIR/Xnextbsd (from a
#               meson install into a scratch root): it is
#               installed inside XLibre.app (Resources/Xnextbsd), never in
#               a system path, and it is not set-uid;
#   5. check    what it links (WindowServer and Mach, no DRM, udev or
#               libinput), that it isn't set-uid, its compiled-in paths,
#               and that it names no build-host path.
#
# TWO WAYS TO RUN IT
#   Cross (CI): inside the nextbsd-toolchain container, like nextbsd-xlibre,
#     with T (amd64 / arm64), TA (amd64 / aarch64), SYSROOT (compat's base
#     with nextbsd-userland's, nextbsd-displayserver-support's and
#     nextbsd-xlibs' artifacts) and
#     CROSS_BINDIR set. libWindowServerClient isn't published as an
#     artifact yet, so it is built from apps-windowserver at the commit in
#     sources.lock into the sysroot first. nextbsd-xlibre's meson cross
#     files are used.
#   Native (NextBSD): with no T, against the system's /usr/X11 and
#     /usr/lib/libWindowServerClient. It needs meson and ninja, which
#     NextBSD doesn't have yet (and no python3 to run meson): set MESON and
#     NINJA to them once it does.
#
# USAGE
#   DESTDIR=/stage ./build-xnextbsd.sh
#
# Plain sh, not bash: NextBSD's base has no bash.
# =============================================================================

set -eu

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="${BUILD:-$ROOT/.build}"
DL="$BUILD/dl"
LOCK="$ROOT/sources.lock"
: "${DESTDIR:=$BUILD/stage}"
: "${MESON:=meson}"
: "${NINJA:=ninja}"

# The paths Xorg is built with (nextbsd-xlibre build-xlibre.sh).
X_PREFIX=/usr/X11
XKB_DIR=/usr/share/X11/xkb
XKM_DIR=/var/db/xkb
LOG_DIR=/var/log
FONT_PATH="$X_PREFIX/share/fonts/X11/misc,$X_PREFIX/share/fonts/X11/75dpi,built-ins"

comp() { echo; echo "==> $*"; }
note() { echo "    - $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

if [ -n "${T:-}" ]; then
    : "${TA:?set TA to the target_arch (amd64 / aarch64)}"
    : "${SYSROOT:?set SYSROOT to the staged NextBSD base}"
    : "${CROSS_BINDIR:?set CROSS_BINDIR to the dir holding the cross clang/lld}"
    case "$T" in
        amd64) CROSS_TRIPLE=x86_64-unknown-freebsd ;;
        arm64) CROSS_TRIPLE=aarch64-unknown-freebsd ;;
        *)     fail "unsupported T=$T (amd64 / arm64)" ;;
    esac
    SYSROOT="$(cd "$SYSROOT" && pwd)"
    READELF="$CROSS_BINDIR/llvm-readelf"
    NM="$CROSS_BINDIR/llvm-nm"
    SUM="sha256sum"
    MODE=cross
else
    command -v "$MESON" >/dev/null && command -v "$NINJA" >/dev/null \
        || fail "a native build needs meson and ninja (MESON=$MESON, NINJA=$NINJA); NextBSD has neither yet: build in CI (the nextbsd-toolchain container) instead"
    SYSROOT=
    READELF="${READELF:-readelf}"
    NM="${NM:-nm}"
    SUM="sha256 -q"
    command -v sha256 >/dev/null || SUM="sha256sum"
    MODE=native
fi

# lock_field <name> <version|sha256|url>
lock_field() {
    local name="$1" col
    case "$2" in
        version) col=2 ;;
        sha256)  col=3 ;;
        url)     col=4 ;;
        *)       fail "lock_field: unknown field $2" ;;
    esac
    awk -v n="$name" -v c="$col" '$1 == n { print $c; found=1 } END { exit !found }' "$LOCK" \
        || fail "$name is not pinned in sources.lock"
}

# fetch <name>: download (cached), verify, unpack into $BUILD/src/<name>.
fetch() {
    local name="$1" url sha file got
    url=$(lock_field "$name" url)
    sha=$(lock_field "$name" sha256)
    file="$DL/$name-$(lock_field "$name" version).tar.gz"
    mkdir -p "$DL"
    if [ ! -f "$file" ]; then
        note "fetch $url"
        curl -fsSL --retry 3 -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    got=$($SUM "$file" | cut -d' ' -f1)
    [ "$got" = "$sha" ] || { rm -f "$file"; fail "$name: SHA-256 mismatch (got $got, sources.lock has $sha)"; }
    note "$name $(lock_field "$name" version), sha256 ok"
    rm -rf "$BUILD/src/$name"
    mkdir -p "$BUILD/src/$name"
    tar -xzf "$file" -C "$BUILD/src/$name" --strip-components=1
}

# apply_series <patch dir> <source dir>: the series file's patches, in order.
apply_series() {
    local dir="$1" src="$2" p n=0
    [ -f "$dir/series" ] || fail "$dir has no series file"
    for p in "$dir"/*.patch; do
        [ -e "$p" ] || continue
        grep -qx "$(basename "$p")" "$dir/series" || fail "$p is not listed in its series"
    done
    while read -r p; do
        case "$p" in ''|'#'*) continue ;; esac
        grep -q '^Origin:' "$dir/$p" || fail "$dir/$p has no Origin: line"
        patch -d "$src" -p1 --forward --batch --quiet < "$dir/$p" || fail "$dir/$p does not apply"
        note "patch $p"
        n=$((n + 1))
    done < "$dir/series"
    note "$n patch(es) from $dir"
}

# ---- cross only: libWindowServerClient into the sysroot ---------------------
# What apps-windowserver's GNUmakefile builds for the library (lib/wsclient.c,
# hidden visibility, soname libWindowServerClient.so.1, linked with liblaunch
# and libsystem_kernel from /usr/lib/system), with its headers and .pc file.
build_wsclient() {
    local src="$BUILD/src/apps-windowserver" cc o
    comp "libWindowServerClient (apps-windowserver $(lock_field apps-windowserver version | cut -c1-12)) into the sysroot"
    fetch apps-windowserver
    [ -e "$SYSROOT/usr/lib/system/liblaunch.so" ] && [ -e "$SYSROOT/usr/lib/system/libsystem_kernel.so" ] \
        || fail "the sysroot has no /usr/lib/system/liblaunch.so and libsystem_kernel.so (stage nextbsd-userland's artifact)"
    cc="$CROSS_BINDIR/clang --target=$CROSS_TRIPLE --sysroot=$SYSROOT"
    o="$BUILD/wsclient-$T"
    mkdir -p "$o"
    $cc -O2 -fPIC -fvisibility=hidden -std=c11 -D_DEFAULT_SOURCE \
        -Wall -Wextra -I"$src/include" -c -o "$o/wsclient.o" "$src/lib/wsclient.c"
    $cc -fuse-ld=lld -shared -Wl,-soname,libWindowServerClient.so.1 \
        -o "$o/libWindowServerClient.so.1" "$o/wsclient.o" \
        -L"$SYSROOT/usr/lib/system" -llaunch -lsystem_kernel -Wl,-rpath,/usr/lib/system
    install -d "$SYSROOT/usr/lib" "$SYSROOT/usr/include/WindowServer" "$SYSROOT/usr/libdata/pkgconfig"
    install -m 0755 "$o/libWindowServerClient.so.1" "$SYSROOT/usr/lib/"
    ln -sf libWindowServerClient.so.1 "$SYSROOT/usr/lib/libWindowServerClient.so"
    install -m 0644 "$src"/include/WindowServer/*.h "$SYSROOT/usr/include/WindowServer/"
    sed -e 's#@PREFIX@#/usr#' -e 's#@LIBDIR@#${prefix}/lib#' \
        -e 's#@INCDIR@#${prefix}/include#' -e 's#@SYSLIBDIR@#/usr/lib/system#' \
        "$src/WindowServerClient.pc.in" > "$SYSROOT/usr/libdata/pkgconfig/WindowServerClient.pc"
    note "libWindowServerClient.so.1, headers and WindowServerClient.pc in the sysroot"
}

# ---- the server --------------------------------------------------------------
SRC="$BUILD/src/xlibre-xserver"
OBJ="$BUILD/xnextbsd${T:+-$T}"

prepare_source() {
    comp "XLibre $(lock_field xlibre-xserver version) with nextbsd-xlibre's patches and hw/nextbsd"
    fetch nextbsd-xlibre
    fetch xlibre-xserver
    apply_series "$BUILD/src/nextbsd-xlibre/patches/xlibre-xserver" "$SRC"
    apply_series "$ROOT/patches/xlibre-xserver" "$SRC"
    cp -R "$ROOT/hw/nextbsd" "$SRC/hw/nextbsd"
    note "hw/nextbsd copied in"
}

configure_and_build() {
    local paths
    comp "meson ($MODE): -Dxorg=false -Dxnextbsd=true"
    set --
    if [ "$MODE" = cross ]; then
        paths="$BUILD/cross/paths-$T.ini"
        mkdir -p "$BUILD/cross"
        printf "[constants]\nsysroot = '%s'\nllvm_bindir = '%s'\n" "$SYSROOT" "$CROSS_BINDIR" > "$paths"
        set -- --cross-file "$paths" --cross-file "$BUILD/src/nextbsd-xlibre/cross/meson/$T.ini"
    fi
    rm -rf "$OBJ"
    # Only Xnextbsd: no Xorg (and so no DRM, glamor, udev or libinput), no
    # other DDX, no GLX yet (software rendering; acceleration is later work).
    # __FILE__ is mapped away from the build host's directories.
    "$MESON" setup "$OBJ" "$SRC" "$@" \
        --prefix="$X_PREFIX" --libdir=lib --buildtype=release \
        --wrap-mode=nodownload \
        -Dc_args="-fmacro-prefix-map=$SRC/= -fmacro-prefix-map=$OBJ/=" \
        -Dxorg=false -Dxnextbsd=true -Dxvfb=false -Dxnest=false \
        -Dxephyr=false -Dxfbdev=false -Dxwin=false -Dxquartz=false \
        -Dglamor=false -Ddrm=false -Dglx=false -Ddri2=false -Ddri3=false \
        -Dpciaccess=false -Dudev=false -Dudev_kms=false -Dhal=false \
        -Dsystemd_logind=false -Dsystemd_notify=false -Dseatd_libseat=false \
        -Dsuid_wrapper=false -Dxselinux=false -Ddtrace=false \
        -Dlibunwind=false -Ddocs=false -Ddevel-docs=false -Ddocs-pdf=false \
        -Dtests=false -Dsha1=libmd -Dxcsecurity=true \
        -Dlog_dir="$LOG_DIR" -Ddefault_font_path="$FONT_PATH" \
        -Dxkb_dir="$XKB_DIR" -Dxkb_bin_dir="$X_PREFIX/bin" \
        -Dxkb_output_dir="$XKM_DIR"
    "$NINJA" -C "$OBJ"
}

# meson install into a scratch root (it removes the build tree's RUNPATH,
# which names the sysroot), then take only the binary from it.
stage() {
    local root="$BUILD/root${T:+-$T}"
    comp "stage $DESTDIR/Xnextbsd"
    rm -rf "$root"
    DESTDIR="$root" "$MESON" install -C "$OBJ" --no-rebuild >/dev/null
    [ -x "$root$X_PREFIX/bin/Xnextbsd" ] || fail "meson installed no $X_PREFIX/bin/Xnextbsd"
    install -d "$DESTDIR"
    install -m 0755 "$root$X_PREFIX/bin/Xnextbsd" "$DESTDIR/Xnextbsd"
}

# ---- static checks -----------------------------------------------------------
check() {
    local x="$DESTDIR/Xnextbsd" needed lib p mode
    comp "checks"

    # What it links: WindowServer's client library (which links liblaunch
    # and libsystem_kernel, the Mach and bootstrap libraries), and nothing
    # of the console display stack Xorg uses.
    needed=$("$READELF" -d "$x" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
    [ -n "$needed" ] || fail "can't read Xnextbsd's DT_NEEDED with $READELF"
    printf '%s\n' $needed | sed 's/^/      NEEDED /'
    printf '%s\n' $needed | grep -qx 'libWindowServerClient.so.1' \
        || fail "Xnextbsd doesn't link libWindowServerClient.so.1"
    for p in 'libdrm\.' 'libudev\.' 'libinput\.' 'libevdev\.' 'libgbm\.' \
             'libEGL\.' 'libGL\.' 'libGLX\.' 'libepoxy\.' 'libpciaccess\.'; do
        if printf '%s\n' $needed | grep -E "^$p"; then
            fail "Xnextbsd links $p"
        fi
    done
    note "links libWindowServerClient.so.1; no DRM, udev, libinput, GBM, EGL, GL or libpciaccess"
    if [ "$MODE" = cross ]; then
        lib="$SYSROOT/usr/lib/libWindowServerClient.so.1"
        for p in liblaunch.so.1 libsystem_kernel.so.0; do
            "$READELF" -d "$lib" | grep -q "NEEDED.*\[$p\]" \
                || fail "libWindowServerClient.so.1 doesn't link $p"
        done
        note "libWindowServerClient.so.1 links liblaunch.so.1 and libsystem_kernel.so.0 (Mach)"
    fi
    # The server's own XKB rules code (xkb/maprules.c) has the same symbol
    # names as libxkbfile's, with other structures: a link that takes them
    # from libxkbfile crashes on the first keymap. They must be the
    # server's.
    if "$NM" -D --undefined-only "$x" | grep -E ' XkbRF_'; then
        fail "Xnextbsd takes XkbRF_* from a shared library (libxkbfile), not from the server's xkb"
    fi
    note "XkbRF_* resolved inside the server"

    # Not set-uid or set-gid: it runs as the user, from the app bundle.
    mode=$(stat -c '%a' "$x" 2>/dev/null || stat -f '%Lp' "$x")
    [ "$mode" = 755 ] || fail "Xnextbsd has mode $mode, not 755"
    [ ! -u "$x" ] && [ ! -g "$x" ] || fail "Xnextbsd is set-uid or set-gid"
    note "mode 755, not set-uid or set-gid"

    # The compiled-in paths are Xorg's.
    for p in "$XKB_DIR" "$X_PREFIX/bin" "$XKM_DIR" "$X_PREFIX/share/fonts/X11/misc"; do
        grep -aqF "$p" "$x" || fail "Xnextbsd doesn't have $p compiled in"
    done
    note "compiled-in paths: XKB $XKB_DIR, xkbcomp in $X_PREFIX/bin, keymaps $XKM_DIR, fonts $FONT_PATH"

    # No build-host path: not in the binary's strings, not in an RPATH.
    for p in "$ROOT" "$BUILD" ${SYSROOT:+"$SYSROOT"}; do
        if grep -aqF "$p" "$x"; then
            grep -aoE "[[:print:]]*$p[[:print:]]*" "$x" | sort -u | sed 's/^/      /'
            grep -lF "$p" "$OBJ"/*.h 2>/dev/null | sed 's/^/      in /' || true
            fail "Xnextbsd names the build-host path $p"
        fi
    done
    if "$READELF" -d "$x" | grep -E '\((RPATH|RUNPATH)\)'; then
        fail "Xnextbsd has an RPATH or RUNPATH"
    fi
    note "no build-host paths, no RPATH"
}

if [ "$MODE" = cross ]; then
    build_wsclient
fi
prepare_source
configure_and_build
stage
check
comp "done: $DESTDIR/Xnextbsd"
