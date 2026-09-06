#!/bin/bash
set -e

echo "=== YaoLang Termux 包发布 ==="
echo ""

# 构建包
bash build-package.sh

# 创建发布目录
VERSION="0.1.0"
RELEASE_DIR="/home/yvm/yaolang-termux-release"
mkdir -p "$RELEASE_DIR"
cp yaolang_${VERSION}_aarch64.deb "$RELEASE_DIR/"

# 生成 Packages 索引
cd "$RELEASE_DIR"
cpactool="apt-ftparchive" || cpactool="$(command -v apt-ftparchive)"
if [ -n "$cpactool" ]; then
    $cpactool packages . > Packages
    $cpactool release . > Release
    echo "✓ Packages 索引已生成"
else
    # 手动创建简单的索引
    echo "deb [trusted=yes] https://a737812.github.io/yaolang-termux/ ./" > "$RELEASE_DIR/packages.list"
    SHA=$(sha256sum yaolang_${VERSION}_aarch64.deb | cut -d' ' -f1)
    SIZE=$(stat -c%s yaolang_${VERSION}_aarch64.deb)
    echo "Package: yaolang"
    echo "Version: $VERSION"
    echo "Architecture: aarch64"
    echo "Filename: pool/yaolang_${VERSION}_aarch64.deb"
    echo "Size: $SIZE"
    echo "SHA256: $SHA"
    echo "Description: YaoLang - Chinese keywords programming language"
    echo "" > "$RELEASE_DIR/Packages"
    echo "✓ 手动索引已创建"
fi

echo ""
echo "发布目录: $RELEASE_DIR"
ls -la "$RELEASE_DIR"
