# Current State

## HT Reload (2026-10-10)

- **已部署 v0.4.0（外壳 + 核心）**，16:15 前后，游戏关闭时部署到 `D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`：
  - 外壳 `HTReload.asi`，193,024 字节，SHA256 `0F2CB234A023A26F3F73FF09B56D3029C96682535E0D1DB6141916A467EA9720`。
  - 核心 `HTReload.core.dll`，379,392 字节，SHA256 `8ED2A2F689DFDC9AD775F42DF3BEF04B16F64F51F206F6D4641CA86CC3458037`。
  - 都与 `build\` 下的产物一致。替身宿主测试通过，包含一次运行中更换核心。**v0.4.0 还没在真游戏里运行过。**
- v0.4.0 相对 v0.3.3：
  - 拆成外壳和核心。外壳由 ASI 加载器加载、常驻；核心由外壳加载，文件一变就换上新的，游戏不用关。以后只有外壳改动才需要关游戏。
  - 新增只读查看指令（`objects`、`obj`、`mem`、`vtable`、`code`、`strref`、`astrref`、`callers`、`names`、`refs`），答案写到 `HTReload.out.txt`。
  - 去掉了挂载后自动探测和它的提示音，探测只由指令触发。
- 指令文件用法：在游戏 `Win64` 写 `HTReload.cmd.txt`，每行一条，核心每秒检查、读完即删。日志里每条指令开始和结束各一行（`指令: …`、`指令完成: …`）。探测报告追加到 `HTReload.probe.txt`，其他答案覆盖写入 `HTReload.out.txt`。
- git：`origin` = `https://github.com/loyal37/HT-Reload`（私有）。v0.3.0 到 v0.4.0 的全部改动合在一个提交 `905f86040d720dd20f410f327b3efff38d3a5eb8` 里，已推送到 `main` 并核对远程一致；之后只有记忆文件的更新提交。没有标签，没有 Release。
- **用户规定（2026-10-10）：每次改完都要提交，不能攒着。** 见 PROJECT_OVERVIEW 的长期约束。

## 已在真游戏里确认的事

- 定位、挂载、垃圾回收请求都正常。垃圾回收入口 RVA `0x4406bd0`。
- **返回登录界面再进入可以完全刷新**，进程不用重启。队伍内切换、换出队伍、传送都不刷新。
- 对象系统（v0.3.3）：名字池 Blocks RVA `0xfa36e10`；对象表 RVA `0xfb1af80`，条目 0x18 字节；类大小字段偏移 0x58；GEngine 全局 RVA `0xfcc6d90`。对象表要等窗口出现后才存在。
- **两次真实探测**（报告在 `build\probes\`，`build` 不入库）：
  - 全进程约 37 万个对象、2.8 万个包。Mod 的 26 个包全部已加载，共 745 个对象，没有一个带 RootSet。扫描 1.2 GB 约 1.1 秒。
  - 包名前缀 `/Game/Characters/Player/042_Blackbird_1/`。主要资产：`player_042_Blackbird_skin`（SkeletalMesh）、5 个 `MI_mint_042_blackbird_*`、16 张贴图、`Blackbird_1_animbp`、`Blackbird_1_save`、`WBP_NTEPanel`、`BPI_NTEPanelTarget`。
  - 直接指向骨骼网格的对象：
    - `HTPlayerAppearance /Game/Characters/Player/042_Blackbird_1/MeshAsset_Player042` +0x80。游戏自己的外观数据资产，不在 Mod 里。
    - `HTSkeletalMeshComponentBudgeted` +0x5b0 和 +0x5b8，三个组件：关卡里的角色 `Player_042_BlackBird_C_…` 的 `CharacterMesh0`；`/Game/sequence_pack/SkillsAnimation_Live/Blackbird/BP_BlackBirdUltraNormal` 类默认对象的 `CharacterMesh0`；关卡序列 `Blackbird_skill_ultraskill` 里的同名组件模板。
    - 角色对象 `Player_042_BlackBird_C_…` +0x2950。
  - 指向材质实例的：16 个 `MaterialInstanceDynamic`，都在角色的 `CharacterMesh0` 下，+0x128 是父材质。
  - 指向动画蓝图类的：角色组件下的 `Blackbird_1_animbp_C_…` 实例，+0x10 是它的类。
  - **黑鸟在场上和切换成别的角色之后，这 25 处直接引用逐行相同**（第二次之前先请求了 15 秒垃圾回收）。她的角色对象、组件、动态材质、动画蓝图实例在不出场时照样活在关卡里。
- 结论：这套模型是黑鸟的主模型，不是可选时装。靠任何切换让游戏放手都行不通，只能原地替换，或者像返回登录那样整个世界重来。

## 原地完全重载的路线（用户 2026-10-10 要求做，尚未实现）

- 引擎自带的 `ReloadPackages` 不在游戏的可执行文件里（`_DEADPACKAGE`、`Reloading %d Package(s):` 都搜不到）。
- 加载器里“旧包改名后允许重新加载同名包”的逻辑在：字符串 `Dropping renamed package` 在 RVA `0xb4aeb54`。源码是 `AsyncLoading2.cpp` 的 `FGlobalImportStore::AddPackageRef`，判断条件是记录的原始包名与包对象当前名字不同，不在 `WITH_EDITOR` 里。
- 游戏里有这些脚本函数的名字：`LoadAsset_Blocking`、`SetSkeletalMesh`、`SetSkinnedAssetAndUpdate`、`SetAnimInstanceClass`。
- 计划的做法：给 26 个旧包改名 → 加载新版本 → 把指向旧对象的引用换成新对象（关卡里的组件通过重新设置网格来重建渲染状态和后处理动画蓝图实例，其余槽位直接改写）→ 垃圾回收。
- 还要攻的入口，以及打算怎么找：
  - 改名：`UObject::Rename` 是虚函数。它里面有日志字符串 `Renaming an object (%s) on top of an existing object (%s) is not allowed`，用 `strref` 找到函数，再在某个没重写它的对象的虚表里找到它的序号，然后对包对象按同一序号调用（包类重写了它）。只改内存里的名字不行，引擎的名字哈希表里会留下悬空指针。
  - 调用带参数的脚本函数：需要 `UObject::ProcessEvent`（虚函数）。它里面只有 `checkf` 的字符串，Shipping 版多半没有，要靠别的办法定位，比如从调用它的函数里读虚表偏移。
  - 脚本函数对象里原生函数指针的偏移：用 `CollectGarbage` 这个函数对象标定，它的原生指针已知是 RVA `0x4406bd0`。
  - 参数布局：读函数对象的属性链，需要先核对属性结构的偏移。
  - 精确替换容器里的引用：需要按属性反射遍历，不能靠按地址猜。

## Next step

1. 用户启动游戏（任何时候，不需要特定角色），告诉我一声。先看日志里“外壳: 核心已加载”“就绪”，再发一条 `objects GameEngine` 确认指令通道和对象系统在真游戏里正常。
2. 用查看指令依次摸清上面列的入口。每查一步把结论记到 ARCHITECTURE。需要反汇编时，把字节写成 `db` 交给 MSVC 的 `ml64` 汇编，再用 `dumpbin /disasm` 看。
3. 入口齐了之后，在核心里加写操作的指令，一步一步做：先改名并加载新包，用探测确认新旧并存；再替换引用并回收，用探测确认旧对象消失。每一版核心直接部署，游戏不用关。

读游戏里的日志和报告要用允许写共享的方式，参考 `test\run.ps1` 的 `Read-Shared`。
