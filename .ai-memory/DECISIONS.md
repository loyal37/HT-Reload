# Decisions

## 2026-10-10 - 暂不使用桥接，打包端改动落在打包器

- 决策内容：用户明确表示暂时不用 NTE Bridge，仍用 `NTE-Mod-Packager` 打包。以后的自动编号、清理旧版本都改打包器。
- 决策原因：用户当前的实际流程就是打包器直接输出到游戏的 `Paks\MOD\<Mod 名>`。
- 其他方案：改桥接的发布步骤。
- 未选择原因：用户暂时不用桥接。
- 影响模块：`D:\Neverness to Everness Mod Loader\cook\packager` 的 `BuildService.PublishWithRollbackAsync`、主窗口的覆盖确认。本项目代码不受影响。

## 2026-10-10 - 验证阶段不改打包器，手动换名

- 决策内容：首轮验证只靠 ASI。用户在打包器名称框里手动填 `name_1_P`、`name_2_P`。
- 决策原因：打包器现有逻辑对新名字能正常发布；先确认热重载可行再投入改打包器。
- 其他方案：先改打包器再验证。
- 未选择原因：最大的未知数在游戏是否释放旧资产，和打包器无关。
- 影响模块：无。

## 2026-10-10 - 换名挂载，不卸载、不覆盖

- 决策内容：每次热发布用更大的版本号挂一个新容器，旧容器保持挂载到游戏退出。
- 决策原因：游戏打开容器时不带 `FILE_SHARE_DELETE`，同名文件无法覆盖；卸载一个还有贴图在流送的容器可能崩溃。引擎对 `_N_P` 名字自动提高优先级，新版本一定压过旧版本。
- 其他方案：先卸载旧容器、覆盖文件、再挂载。
- 未选择原因：需要先保证该容器的资产全部释放，风险和实现量都更大。
- 影响模块：`main.cpp` 的 `Watcher`，以及以后的打包器改动。

## 2026-10-10 - 调用引擎自己的 MountPak 处理函数，用字符串和委托实例定位

- 决策内容：不写死地址和特征码。通过保留在 Shipping 版里的字符串 `Mounting pak file: %s \n` 找候选函数，再通过 `FCoreDelegates::MountPak` 的委托实例确认函数并取得 `FPakPlatformFile` 对象。
- 决策原因：游戏代码在磁盘上加密，无法离线取特征码；游戏更新频繁（SigBypasser 日志在 2026-08-16 到 10-09 之间记录了 6 个不同的命中地址）；这条路径同时拿到函数和对象，不需要假设任何结构体偏移。
- 其他方案：对 `FPakPlatformFile::Mount` 做特征码扫描；启动时 inline hook `Mount` 截获 `this`；走 `FPlatformFileManager` 链。
- 未选择原因：特征码随更新失效；inline hook 要改游戏代码；后者依赖虚表索引。
- 影响模块：`resolver.cpp`。

## 2026-10-10 - 只读不改，不做反作弊规避

- 决策内容：插件只读取进程内存并调用引擎已有函数，不修改游戏代码，不做任何隐藏或绕过反作弊的事。
- 决策原因：这是自用的开发工具；动作越少越稳，也不该碰反作弊。
- 其他方案：无。
- 影响模块：全部。

## 2026-10-10 - 用线程消息钩子切到游戏线程

- 决策内容：对 `UnrealWindow` 所在线程装 `WH_GETMESSAGE` 线程钩子，用 `PostThreadMessage` 投递自定义消息，在引擎取消息时执行挂载。
- 决策原因：UE 在游戏线程上每帧取一次消息，位置在世界 Tick 之外；不需要再找一个引擎函数地址。
- 其他方案：替换窗口过程；hook 引擎的 Tick。
- 未选择原因：替换窗口过程会和其他子类化的工具（如 ReShade）相互影响；hook Tick 需要额外定位并改代码。
- 影响模块：`game_thread.cpp`。

## 2026-10-10 - 轮询目录，不用通知文件

- 决策内容：每秒扫描一次 `Content\Paks`，以“文件可独占读打开且大小连续两次不变”判断写入完成。
- 决策原因：打包器是先写临时文件再改名，文件一出现就是完整的；这样打包器完全不用改。
- 其他方案：打包器写请求文件；`ReadDirectoryChangesW`。
- 未选择原因：前者要改打包器；后者代码更多，而目录里只有几十个文件，轮询成本可忽略。
- 影响模块：`main.cpp` 的 `Watcher`。

## 2026-10-10 - 顺序参数传 INDEX_NONE

- 决策内容：调用处理函数时 `PakOrder` 传 `-1`。
- 决策原因：引擎会用和启动扫描相同的 `GetPakOrderFromPakFilePath` 计算，行为与启动时挂载的 Mod 完全一致。
- 其他方案：显式传 3。
- 未选择原因：那是对游戏目录结构的额外假设。
- 影响模块：`main.cpp` 的 `CallHandleMountPak`。

## 2026-10-10 - 挂载后通过 execCollectGarbage 请求垃圾回收

- 决策内容：v0.2.0 起，挂载成功后 120 秒内每 5 秒调用一次 `UKismetSystemLibrary::execCollectGarbage`。取代同日早些时候“v0.1 不做强制 GC”的做法。
- 决策原因：真游戏里挂载成功但队伍内切换角色不刷新。没有主动回收时，“没变化”分不清是没卸载还是还没回收。
- 为什么选这个入口：它只是请求引擎在下一帧自己回收，不需要拿 GC 锁；没有参数，用全零的假脚本帧就能调用；可以通过 UHT 生成的名字表稳定定位。
- 其他方案：直接调 `CollectGarbage`；找 `UEngine::ForceGarbageCollection` 和 `GEngine`；绑热键手动触发。
- 未选择原因：前者要在正确的时机和锁状态下调用；第二个函数太小，没有可用的锚点；热键需要轮询键盘或再装钩子。
- 影响模块：`resolver.cpp` 的 `FindCollectGarbageThunk`，`main.cpp` 的 `Watcher::RequestCollection`。

## 2026-10-10 - 部署到 Win64 根目录

- 决策内容：`HTReload.asi` 和 `HTReload.log` 放在游戏 `Win64` 根目录，与 `dsound.dll`、`Anticensor.asi` 并列。
- 决策原因：用户平时就在这个目录看这些文件。
- 其他方案：放 `Win64\plugins`，加载器同样会加载。
- 影响模块：`deploy.ps1`。

## 2026-10-10 - 脚本保持 ASCII，源码用 /utf-8

- 决策内容：`.ps1` 只用 ASCII 和英文提示；C++ 源码是无 BOM 的 UTF-8，编译加 `/utf-8`，日志是中文。
- 决策原因：Windows PowerShell 5.1 把无 BOM 脚本按系统代码页读取，中文会损坏甚至破坏语法。
- 影响模块：`build.ps1`、`deploy.ps1`、`test\run.ps1`。

## 2026-10-10 - 项目记忆持久化到 `.ai-memory`

- 决策内容：按 `D:\ueproject\HT\.ai-memory` 的做法，在本项目根目录维护同样的七个文件和 `project-memory-keeper` Skill。
- 决策原因：用户明确要求。
- 影响模块：项目工作流程。
