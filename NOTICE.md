# 来源和许可证

本目录是单机成功实现的源码整理版，不将工作全部宣称为原创。

组合项目使用根目录 GPL-2.0-only；原 TU102 copyleft-next 0.3.1 第 3 条提供 GPL 组合兼容条款。原许可证全文保存在 `LICENSES/copyleft-next-0.3.1.txt`。NVIDIA、CMP40HX 的 MIT 声明、EDK2 的 BSD-2-Clause-Patent 和具体文件头的其他声明继续适用。

`research/tu102/transaction/tu102_core.h` 和核心模拟测试来自 `haohervchb/Turing-unlock` 固定提交 `a188b24a392414f29f931ea886329227cfbc2f98`，保留上游来源说明。原通知在 `LICENSES/UPSTREAM-NOTICE.md`；其中相对链接及“上游公开版”的描述属于原项目上下文。

本机移植改动包括：原生 FWSEC 位置、22GB/FRTS/107MiB 堆布局、入口 selector 锁存、UEFI PCI/GOP 所有权处理、AMD CPU_IO2 主机寄存器检查、直接高物理地址分配、Windows 交接、静默自动入口、错误恢复和对应模拟测试。

Booter/V67 等输入在本机从 `xrip/cmp50hx-unlock` 固定提交 `df2fe11b370bb52cc314576d0f2f5ae47ce908f8` 取得；这不意味着该仓库每个文件或固件都是 MIT。本目录不重新分发固件输入。NVIDIA 普通 Falcon/WPR 接口研究来源见原通知。

`preflight.c`、`live_prom.h`、SHA-256 和本地框架采用其现有 GPL 上下文。EDK2 头文件位于 `deps/edk2/MdePkg/Include`，其文件头及 `LICENSES/EDK2-BSD-2-Clause-Patent.txt` 保留。

本次公开整理只对公开副本做 ESP GUID 参数化、移除个人环境脚本依赖并增加便携离线构建与说明。未改动机器上正在运行的 EFI。原成功二进制的哈希、公开源码原/新哈希及结果另行记录；不声称公开副本逐字节等于原成功二进制。
