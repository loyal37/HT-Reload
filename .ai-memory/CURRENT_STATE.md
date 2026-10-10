# Current State

## HT Reload (2026-10-10)

- **游戏里运行的是外壳 v0.4.0 + 核心 0.5.0**，目录 `D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`：
  - 外壳 `HTReload.asi`，SHA256 `0F2CB234A023A26F3F73FF09B56D3029C96682535E0D1DB6141916A467EA9720`，16:15 前后游戏关闭时部署，之后没变。
  - 核心 `HTReload.core.dll`，SHA256 `88A2DCF8CF6DF1A6077CF8DCCD89A9A38833D10E37BEC7A277FC247AD89989D3`，17:16 前后在游戏运行中部署，外壳自动换上。
- git：`origin` = `https://github.com/loyal37/HT-Reload`（私有）。最新代码提交 `f8d54b1`（核心 0.5.0），已推送；之后只有记忆文件的更新提交。
- **用户规定：每次改完都要提交，不能攒着。** 见 PROJECT_OVERVIEW 的长期约束。

## 已在真游戏里确认的事

- 定位、挂载、垃圾回收请求都正常。垃圾回收入口 RVA `0x4406bd0`。
- **返回登录界面再进入可以完全刷新**，进程不用重启。队伍内切换、换出队伍、传送都不刷新。
- 对象系统：名字池 Blocks RVA `0xfa36e10`；对象表 RVA `0xfb1af80`，条目 0x18 字节；类大小字段偏移 0x58；GEngine 全局 RVA `0xfcc6d90`。对象表要等窗口出现后才存在。
- **外壳和运行中更换核心**（17:02 启动的这次运行）：外壳启动、加载核心、接入游戏线程都正常；部署新核心后不到一秒换上，三次更换都没问题，游戏正常。
- **指令通道和查看指令**都正常，用 `tools\ask.ps1` 发指令。
- **引擎入口**（`engine` 指令，抽样约 400 个对象的虚表投票）：
  - `UObject::Rename` = RVA `0x181e3b0`（大小 0x424），虚表第 46 项（偏移 0x170）。包类重写了它，RVA `0x1845670`，只有 0x13 字节，内容就是调用 `UObject::Rename` 再把返回值转成 bool。
  - `UObject::ProcessEvent` = RVA `0x18aacd0`（大小 0x3e8），虚表第 76 项（偏移 0x260）。已反汇编核对。
  - `UFunction` 的布局与原版一致：`FunctionFlags` 0xB0，`NumParms` 0xB4，`ParmsSize` 0xB6，`ReturnValueOffset` 0xB8，`FirstPropertyToInit` 0xC0，`EventGraphFunction` 0xC8，原生函数指针 `Func` 0xD8（用 `CollectGarbage` 的已知入口核对过）。函数对象大小 0xE0。
  - `FProperty`：`ArrayDim` 0x30，`ElementSize` 0x34，`PropertyFlags` 0x38，`Offset_Internal` 0x44；`FField::Next` 0x18。
- **试调用成功**（`calltest`）：通过 `ProcessEvent` 调 `KismetSystemLibrary.GetFrameCount`，两次返回 54325 和 54374，间隔半秒，帧号在涨。说明在游戏线程上调用带返回值的引擎脚本函数是通的。
- **需要的脚本函数对象**都在：
  - `KismetSystemLibrary.LoadAsset_Blocking`：参数块 0x30 字节，返回值偏移 0x28。参数是软引用（8 字节弱指针缓存 + 包名 8 + 资产名 8 + 子路径字符串 16）。
  - `SkeletalMeshComponent.SetSkeletalMeshAsset`：参数块 8 字节（新网格指针）。
  - `SkinnedMeshComponent.SetSkinnedAssetAndUpdate`：参数块 0x10 字节。
  - `EmptyOverrideMaterials` 不是脚本函数，没有。
  - 游戏自己有 `/Script/HTGame.ChangeMeshLogicObj.NetMultiCast_SetSkeletalMeshAndAnimClassNextTick`，像是它换模型的正规途径，还没研究。
- **两次真实探测**（报告在 `build\probes\`）和 **重载计划**（`plan blackbird`）：
  - 全进程约 37 万个对象。Mod 的 26 个包全部已加载，共 745 个对象，26 个顶层公开资产，没有带 RootSet 的。
  - 包名前缀 `/Game/Characters/Player/042_Blackbird_1/`。
  - 其他对象里指向旧对象的槽位共 25 处，计划的处理方式：
    - 重设网格（1 个组件、2 个槽位）：关卡里角色 `Player_042_BlackBird_C_…` 的 `CharacterMesh0`，+0x5b0 和 +0x5b8。
    - 改写（6 处）：角色对象 +0x2950；游戏的外观资产 `HTPlayerAppearance …/MeshAsset_Player042` +0x80；大招演出蓝图 `BP_BlackBirdUltraNormal` 默认对象的组件 +0x5b0、+0x5b8；关卡序列 `Blackbird_skill_ultraskill` 里的组件模板 +0x5b0、+0x5b8。
    - 不动（17 处）：16 个动态材质的父材质槽 +0x128，和动画蓝图实例的类指针 +0x10。它们应当在重设网格后随旧实例一起消失。
  - 黑鸟在场上和切换成别的角色之后，这些引用完全相同。她的角色对象不出场时照样活在关卡里。

## 原地完全重载的做法（用户 2026-10-10 要求做）

- 引擎自带的 `ReloadPackages` 被裁掉了。加载器的“旧包改名后可重新加载同名包”还在（`AsyncLoading2.cpp` 的 `FGlobalImportStore::AddPackageRef`，非编辑器代码）。它丢弃旧包记录时会把旧包对象的包 ID 清掉，所以旧包以后被回收不会误删新包的记录。
- 步骤：给旧包改名（`Rename`，标志 0x35：不重置加载器、不标脏、不建重定向、非事务）→ 用 `LoadAsset_Blocking` 加载（软引用里的包名和资产名直接取旧对象的名字 ID）→ 组件用 `SetSkeletalMeshAsset` 重设网格，其余槽位改写 → 垃圾回收。
- **已实现但还没运行过**：`renameload <资产名>`，只做前两步、只针对一个资产所在的包，不改任何引用。用来证明新旧两份可以并存。这是第一条会改动引擎状态的指令。
- 还没做：批量改名加载、重设网格和改写槽位、清掉组件上的覆盖材质（要先确认用哪个脚本函数）、接到挂载之后自动执行。

## Next step

1. 已告诉用户接下来要做第一次写操作，等用户确认可以开始（游戏可能崩溃）。
2. 得到确认后：`renameload 042_blackbird_body_d`，看答案里改名是否成功、加载返回的是不是一个新对象；再 `objects 042_blackbird_body_d` 确认新旧并存；观察游戏是否正常。
3. 成功后做批量版，再做第三步，每一步之后用 `probe` 核对。
4. 每轮改动测试通过后提交并推送，再更新记忆。

用 `tools\ask.ps1` 发指令，用 `tools\disasm.ps1` 反汇编 `code` 指令取回的字节。
