# Project Overview

## 项目名称和目标

项目名：HT Reload。

目标：让《异环 / Neverness to Everness》的 pak Mod 在游戏运行期间热重载。制作 Mod 时改完重新打包，游戏不重启就能看到新版本，省掉重启、登录、进场景的时间。

## 解决的问题

- 游戏只在启动时扫描 `Content\Paks` 挂载一次，之后没有任何途径再挂载。
- Shipping 版引擎没有 `Mount` / `Unmount` 控制台命令。
- 游戏的可执行文件在磁盘上加壳加密，无法离线找函数地址。

## 目标用户

当前项目作者本人，用于自己制作角色 Mod 时的迭代。不面向普通玩家，不随 Mod 分发。

## 技术栈

- C++20，MSVC（VS 2022 17.14，工具集 14.44），静态 CRT，只依赖 KERNEL32 / USER32
- 产物是 `.asi`（改名的 DLL），由 Ultimate ASI Loader 加载
- Windows x64
- 构建、测试、部署脚本为 Windows PowerShell 5.1

## 运行环境

- 开发目录：`E:\HT Reload`，远程仓库 `loyal37/HT-Reload`（私有，用户自己创建）
- 游戏目录：`D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`
- 游戏引擎：改过的 UE 5.6；本机原版源码在 `D:\ue\UE_5.6`，用于核对引擎行为
- ASI 加载器：游戏 Win64 里的 `dsound.dll` = Ultimate ASI Loader x64 9.7.1，加载 Win64 和 `Win64\plugins` 里的 `.asi`
- 同目录的其他 ASI：`plugins\UniversalSigBypasser.asi`（pak 签名补丁，Mod 能挂上靠它）、`Anticensor.asi`（近镜头淡出补丁，与 pak 无关）
- 游戏带 `AntiCheatExpert` 反作弊
- Mod 存放：`HT\Content\Paks\MOD\<Mod 名>\*.pak|.utoc|.ucas`
- 打包器：`D:\Neverness to Everness Mod Loader\cook\packager`（仓库 `loyal37/NTE-Mod-Packager`），用户当前直接输出到游戏的 `Paks\MOD\<Mod 名>` 目录
- 相关项目：`D:\ueproject\HT`（UE 编辑器插件和 NTE Bridge），有自己的 `.ai-memory`

## 核心功能

- 运行时定位引擎的 `FPakPlatformFile::HandleMountPakDelegate` 和它绑定的 `FPakPlatformFile` 对象。
- 每秒扫描 `Content\Paks`，发现新出现且写入完成的 pak（及同名 utoc/ucas）后，在游戏线程调用引擎挂载。
- 成功/失败提示音，日志写到 ASI 同目录的 `HTReload.log`。

## 明确不做的功能

- 不做任何反作弊规避或隐藏。
- 不给游戏代码打补丁，不做 inline hook。
- 不随 Mod 分发给玩家。
- 当前不做卸载旧包、不做强制 GC、不做原地替换已加载资产。

## 项目长期约束

- 用户决定：暂时不用 NTE Bridge，仍用打包器打包。打包端的改动落在打包器，不落在桥接。
- 游戏运行时同名三件套被占用，热发布必须换名，在 `_P` 前加递增数字（`name_1_P`、`name_2_P`）。
- 定位逻辑不写死地址或特征码，要能跨游戏更新；游戏更新后先看日志再判断是否需要改。
- `.ps1` 脚本保持纯 ASCII：Windows PowerShell 5.1 把无 BOM 脚本按系统代码页读取，中文会损坏。
- 改完先跑 `test\run.ps1`，通过后再部署；部署前游戏必须关闭。
- 每次提交后更新 `.ai-memory`，记录准确的提交、测试、部署结果和下一步。
- 不把 Token、Cookie、私钥等敏感信息写入项目记忆。
