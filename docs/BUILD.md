# 离线构建

目录保留原相对 include 关系：`research/tu102/transaction` 是运行源码和模拟测试，`unlock` 是 SHA-256，`deps/edk2` 是头文件。

1. 安装 Python 和 x64 MinGW GCC/LD；可通过 Build.ps1 参数指定绝对可执行文件路径，不依赖作者用户名或工具安装目录。
2. 先运行 `tools/Test-Public.ps1`：20 项 Windows 交接、2 项同 ESP 交接，以及公开前后测速比较不需要私有固件。
3. 完整构建需 `-PrivateInputs` 指向原本机资料目录或私人备份中的 `rebuild/research/tu102`。必需文件清单见 `inputs.example.json` 和 `tools/Prepare-PrivateInputs.py`。还需要该资料目录的原始 `transaction/internal_auto.h` 提供本机 ESP GUID 字节。
4. 输入全部进行大小/SHA256 校验；只生成被 Git 忽略的 `build/`、`captures/`、`assets/`，不访问 GPU、不下载文件、不创建启动项。
5. Build.ps1 运行核心、入口、DMA、GOP、驱动释放、Windows 交接和 29 项自动故障恢复模拟后生成 EFI，输出在 `research/tu102/build/transaction`。该输出含嵌入固件，不能把整个构建目录上传。

公开审计扫描目录本身，不忽略生成物：执行完整构建后，应另外导出干净 Git 源码树再运行 `tools/Audit-Public.py`；不要压缩整个已构建工作目录。

这份固定配置仍限定同一 GPU/主板/原生固件、PCI BAR、CPU、IVRS 与原 Boot0000 身份。更换显卡、BIOS、驱动或启动变量后可能拒绝；不要只替换 GUID/哈希宣称支持新机器。为其他平台移植需要重新采集与评估。

无通用安装脚本。读源码和构建不会自动安装。原本机恢复可从固件菜单选择原 Windows Boot Manager；私人备份中另有独立 Windows-first 恢复脚本。
