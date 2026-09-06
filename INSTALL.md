# Yaolang Termux 包安装指南

## 仓库信息
- GitHub: https://github.com/a737812/yaolang-termux
- GitHub Pages: https://a737812.github.io/yaolang-termux/

## 快速安装

### 方法一：Termux 用户仓库（推荐）

```bash
pkg install curl
mkdir -p ~/.termux/user-repo/pool

# 添加仓库配置
curl -s https://a737812.github.io/yaolang-termux/packages.list > ~/.termux/user-repo/packages.list

# 下载 deb 包
curl -s https://a737812.github.io/yaolang-termux/pool/yaolang_0.1.0_aarch64.deb -o ~/.termux/user-repo/pool/yaolang_0.1.0_aarch64.deb

# 升级并安装
pkg upgrade
pkg install yaolang
```

### 方法二：直接安装 .deb

```bash
pkg install curl
curl -s https://a737812.github.io/yaolang-termux/pool/yaolang_0.1.0_aarch64.deb -o ~/yaolang.deb
pkg install ~/yaolang.deb
```

## 验证安装

```bash
yaolang --version
# 输出: yaolang 0.1.0
```

## 测试程序

创建一个测试文件 `hello.耀`:

```bash
cat > hello.耀 << 'YAO'
函数 主函数() {
    打印("你好，曜语！")
    变量 年龄 = 30
    打印(年龄)
}
YAO

yaolang hello.耀
./hello
```

## 卸载

```bash
pkg uninstall yaolang
rm -rf ~/.termux/user-repo
```

## 从源码构建

```bash
git clone https://github.com/a737812/yaolang-termux.git
cd yaolang-termux
bash build-package.sh
pkg install yaolang_0.1.0_aarch64.deb
```

## 问题排查

如果遇到 "repository is under maintenance" 错误:
```bash
pkg clean
pkg update
```

## 相关链接

- 曜语主页: https://github.com/a737812/yaolang
- TUR PR: https://github.com/termux-user-repository/tur/pull/2744
