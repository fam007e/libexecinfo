# Maintainer: fam007e <faisalmoshiur@gmail.com>
pkgname=libexecinfo
pkgver=1.1.0.19
pkgrel=3
pkgdesc="BSD-licensed clone of backtrace facility found in GNU libc"
# armv7h/aarch64 intentionally absent: GCC does not implement
# __builtin_return_address(N>0) on ARM32 and returns NULL for it on
# AArch64, so backtrace() would always return 0 frames there.
arch=('x86_64' 'i686')
url="https://github.com/fam007e/libexecinfo"
license=('BSD')
depends=('glibc')
makedepends=('python')
# Soname convention so packages can depend on libexecinfo.so directly.
provides=('libexecinfo.so')
conflicts=('libexecinfo-git')
# Keep libexecinfo.a in the package: makepkg's default !staticlibs
# would otherwise silently strip it during tidying.
options=('staticlibs')

# Pinned to the exact tagged release asset, not releases/latest/download/.
# "latest" follows whatever release is newest at *build* time, so the
# sha256sum below would silently stop matching the moment a new release
# is published, even though pkgver here hasn't changed.
source=("$pkgname-$pkgver.tar.gz::https://github.com/fam007e/libexecinfo/releases/download/v$pkgver/libexecinfo-$pkgver.tar.gz")
sha256sums=('REPLACE_ME_RUN_updpkgsums_AGAINST_THE_PINNED_TARBALL')

# No prepare()/sed step: the fixed Makefile generates libexecinfo.pc
# directly from libexecinfo.pc.in with correct PREFIX/LIBDIR/INCLUDEDIR
# substitution, so there is nothing left for makepkg to patch around.

build() {
    cd "$srcdir/$pkgname-$pkgver"
    make PREFIX=/usr LIBDIR=/usr/lib INCLUDEDIR=/usr/include/libexecinfo all libexecinfo.pc
}

check() {
    cd "$srcdir/$pkgname-$pkgver"
    # With the fixed Makefile, `make test` now actually builds AND runs
    # the test binary and propagates its exit code, instead of being a
    # silent no-op (the previous Makefile's `test` PHONY target and its
    # output file both happened to be named "test", so make treated the
    # file as already up to date and never ran anything).
    make test
}

package() {
    cd "$srcdir/$pkgname-$pkgver"
    make install DESTDIR="$pkgdir" PREFIX=/usr LIBDIR=/usr/lib INCLUDEDIR=/usr/include/libexecinfo
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
