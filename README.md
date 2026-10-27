# RTX 2080 Ti 22GB Windows Tensor Unlock — local TU102 research port

已在一台 RTX 2080 Ti 22GB / 铭瑄 B850 / AMD 平台完成 Windows 开机前 Tensor 解锁，支持本机静默自动执行。本仓库公开源码、构建工具、测试和脱敏测速数据，**不是适用于所有 2080 Ti 的通用安装包**。

## 实测结果

2026-10-27，驱动 621.01、GSP 620.67.01；68 SM、22,528 MiB，VBIOS `90.02.0b.00.0e`。

| 测试项 | 解锁前 | 自动启动后 |
|---|---:|---:|
| FP16 输入 / FP32 累加 Tensor 微基准 | 45.33 TFLOPS | 89.94 TFLOPS |
| 每 SM 每时钟 FLOP | 511.23 | 1018.60 |
| 每时钟吞吐倍率 | 1.00 | 1.99244 |
| 精确结果检查 | 通过 | 通过 |
| 显存容量 | 22GB | 22GB |

FP16 累加对照的每时钟吞吐基本不变。结果见 `evidence/`。这些数据不代表所有应用翻倍，也不是长期稳定性认证。普通重启后的自动解锁已实测；快速启动保持开启，但关机再开机/休眠恢复没有单独验证。故障恢复分支经过模拟，未做真实显卡故障注入。

## 方法和行为

- Windows 启动前的 x64 UEFI 应用；固件载荷编进本机 EFI，正常开机无需工程目录或 U 盘。
- 本卡原生 FWSEC、22GB 布局，以及公开成功项目的 TU102 Booter/V67 方法。
- 本机 AMD 配置采用直接物理 DMA，**不是没有 DMA**，也没有 UEFI Map 验证；不关闭 IOMMU。
- 核对原卡 PROM、PCI 身份、CPU、IVRS、显示关系及驱动所有者，随后才允许执行。
- 自动模式无正常屏幕提示、无 Esc 跳过、无倒计时。错误才显示阶段与状态并写日志。
- 操作显卡前的失败直接启动 Windows；操作开始前准备原 Windows Boot Manager 的一次性恢复启动和固件看门狗；中途失败冷重启走原 Windows 启动项，成功则清除恢复设置。主板复位/硬件卡死等仍不能保证软件恢复。
- 不刷显卡/主板 BIOS，不改显存容量、时序或电压。固件仍会使用保留显存/WPR，沿用上游 CLOCK_BOOST 元数据。

## 构建和测试

需要 Windows、Python 3.9+、x64 MinGW GCC/LD。EDK2 头文件已附带，许可证在 `LICENSES/`。

无需固件的公开检查：

```powershell
python -B tools/Audit-Public.py
./tools/Test-Public.ps1 -Gcc gcc -Python python
```

完整构建需要本人合法持有的、符合当前固定配置的私有输入。仓库不包含 ROM、FWSEC、固件二进制、EFI 成品或本机启动变量。没有私有输入时构建会明确拒绝；不要删校验强行用于其他机器。

```powershell
./Build.ps1 -Gcc gcc -Ld ld -Python python -PrivateInputs 'X:\private-backup\rebuild\research\tu102'
```

构建只做离线编译和模拟测试，不安装、不改启动项、不重启。详见 [构建说明](docs/BUILD.md)、[来源与许可证](NOTICE.md)。

## 公开版与本机版的区别

公开版把私有 EFI 分区 GUID 的源码初始化值改为由忽略目录生成；其余本机硬件限定检查保留。公开构建不能被称为已验证的通用版本。配合原本机私有输入，整理版已离线重建出与当前安装 EFI **SHA256 完全一致**的程序，见 `evidence/PACKAGE-VERIFICATION.json`；私有输入和成品不包含在公开包。`evidence/VALIDATION.json` 中的 SHA256 标识原本机成功程序，不是可下载的 EFI 成品。公开版本的源码变更见 `SOURCE-MANIFEST.json`。

本仓库不附带通用自动安装器，避免把作者的分区、启动项或磁盘身份套到其他机器上。构建成功不等于具备安装资格。

## 致谢

- [haohervchb/Turing-unlock](https://github.com/haohervchb/Turing-unlock)：TU102 核心，固定 `a188b24a392414f29f931ea886329227cfbc2f98`。
- [xrip/cmp50hx-unlock](https://github.com/xrip/cmp50hx-unlock)：Booter/V67 输入来源，固定 `df2fe11b370bb52cc314576d0f2f5ae47ce908f8`。
- [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK)：Windows UEFI 框架与 SHA-256。
- [chenxluo/tu102-22g-uefi-unlock](https://github.com/chenxluo/tu102-22g-uefi-unlock)：Windows 高物理地址方法参考，固定 `6ee61bdaa8929e27926734f963873f356fc159ca`。
- CMP40HX-Unlock、Jon Pry 的 canary 研究、NVIDIA 开放驱动源码和 TianoCore EDK2，详见保留的上游声明。

组合项目按 GPL-2.0-only 分发；TU102 核心原 copyleft-next 0.3.1、MIT 和 BSD 声明均保留，不对固件重新授权。
