# Yaolang Termux Packages

自托管的曜语 Termux 软件包仓库。

## 安装

在 Termux 中添加此仓库：

```bash
pkg install curl
mkdir -p ~/.termux/user-repo

curl -s https://a737812.github.io/yaolang-termux/packages.list > ~/.termux/user-repo/packages.list
curl -s https://a737812.github.io/yaolang-termux/packages.sig > ~/.termux/user-repo/packages.sig
curl -s https://a737812.github.io/yaolang-termux/pool/yaolang_0.1.0_aarch64.deb > ~/.termux/user-repo/pool/yaolang_0.1.0_aarch64.deb

pkg upgrade
pkg install yaolang
```

## 结构

```
tur/
└── yaolang/
    └── build.sh
```

## License
MIT
