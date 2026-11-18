# User Instruction Memory

## Entries

[Project Knowledge Summary]
- Date: 2026-11-18
- Context: Discovered by Agent while porting Mu-Silicium UEFI to Kirin 9010
- Category: Build Methods
- Instructions:
  - UEFI 源码位于 `uefi/`，构建入口 `uefi/build_uefi.py -d mro -r DEBUG -c`，CI 为 `.github/workflows/build-kirin9010-uefi.yml`。
  - 依赖必须固定到 uefi-sheng 参考仓的提交（Mu_Basecore cfb9372、mu_plus b8d46c7、mu_oem_sample b70b4b7、Silicium-ACPI 2d465ad、openssl 67b5686），跟随分支 tip 会导致 `Resources/MuPatches` 应用失败。
  - 内核仓 `.gitignore` 忽略 `*.patch` 与 `.github`，新增 `uefi/Resources/MuPatches/*.patch` 和 workflow 必须 `git add -f`。
  - Mu_Basecore 需执行 `git submodule update --init --recursive --depth 1`，否则 MdePkg.dec 因 mipisyst 缺失而报错。
  - CI 依赖包：uuid-dev nasm lld clang llvm binutils-aarch64-linux-gnu gcc-aarch64-linux-gnu mono-devel python3 python3-pip（mono 供 nuget 的 nasm 依赖，llvm 提供 llvm-objcopy）。
