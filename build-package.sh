#!/bin/bash
set -e
PACKAGE="yaolang"
VERSION="0.1.0"
ARCH="aarch64"
BASE_DIR=$(pwd)
WORK_DIR="$BASE_DIR/.build-workspace"
PKG_DIR="$WORK_DIR/$PACKAGE"
rm -rf "$WORK_DIR"
mkdir -p "$PKG_DIR/DEBIAN" "$PKG_DIR/usr/bin" "$PKG_DIR/usr/share/doc/$PACKAGE"
cp "$BASE_DIR/tur/$PACKAGE/src/yaolang.c" "$PKG_DIR/src/yaolang.c"
cp -r "$BASE_DIR/tur/$PACKAGE/test" "$PKG_DIR/test"
cd "$PKG_DIR"
cc -O2 -o "usr/bin/yaolang" src/yaolang.c -lm
chmod 755 "$PKG_DIR/usr/bin/yaolang"
cp "$BASE_DIR/README.md" "$PKG_DIR/usr/share/doc/$PACKAGE/" 2>/dev/null || true
cat > "$PKG_DIR/DEBIAN/control" << CTRL
Package: $PACKAGE
Version: $VERSION
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: @a737812
Description: YaoLang - Chinese keywords programming language
 A compiled programming language with 100% Chinese keywords.
CTRL
cd "$BASE_DIR"
dpkg-deb --build --root-owner-group "$PKG_DIR" "$PACKAGE_$VERSION_${ARCH}.deb"
echo "Built: $PACKAGE_$VERSION_${ARCH}.deb"
