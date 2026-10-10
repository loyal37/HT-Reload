# Current State

## HT Reload (2026-10-10)

- **原地完全重载已在真游戏里跑通，用户确认看到了效果。** 发布新版本后不用返回登录，网格、材质、贴图、动画蓝图都换成新版本，旧对象全部被回收。
- **游戏里运行的是外壳 v0.4.0 + 核心 0.6.2**，目录 `D:\Neverness To Everness\Client\WindowsNoEditor\HT\Binaries\Win64`：
  - 外壳 `HTReload.asi`，SHA256 `0F2CB234A023A26F3F73FF09B56D3029C96682535E0D1DB6141916A467EA9720`，16:15 前后游戏关闭时部署，之后没变。
  - 核心 `HTReload.core.dll`，SHA256 `9F4C74F6EFF38B089FD6BA350178DF74827F92CDFED03ED82DBC2AAC852017C1`，17:44 在游戏运行中部署，外壳自动换上（这次运行的第 4 次更换）。
- git：`origin` = `https://github.com/loyal37/HT-Reload`（私有）。最新代码提交 `fcdc7cc`（核心 0.6.2），已推送。本轮的三个提交：`5590bf6`（0.6.0 完整重载）、`cf3dd15`（0.6.1 名字防护）、`fcdc7cc`（0.6.2 挂载后自动重载）。
- **用户规定：每次改完都要提交，不能攒着。** 见 PROJECT_OVERVIEW 的长期约束。

## 这次运行（17:19 启动，PID 37696）里做过的写操作，全部成功

1. `renameload 042_blackbird_body_d`：改名返回 1，加载返回一个不同的新对象。没人引用的新副本几秒后被回收，游戏不受影响。
2. `reload blackbird`（17:34，磁盘上只有 `blackbird_2_P`，内容相同）：改名 25/25、加载 25/25，16 个动态材质换父材质，6 处改写，场上角色重设网格。耗时约 1.3 秒。回收后带 `_HTOLD_` 的包一个不剩。
3. 用户打包了 `blackbird_4_P`，17:42 自动挂载；`reload blackbird`（17:43）：改名 26/26、加载 26/26，14 个换父材质，6 处改写，重设网格。**用户确认游戏里看到了变化。** 旧包同样全部被回收。

## 还没在真游戏里走过的

- **挂载后自动重载**（核心 0.6.2）：代码路径和手动 `reload` 相同，只是由挂载成功触发，用的是新容器自己的包 ID。替身宿主里走到了“找不到引擎函数就退回老办法”的分支；真游戏里还没有发布过新包来触发它。用户下次打包就是第一次。预期：两声上行提示音（挂载）之后再一声更高的（重载完成），日志有“重载完成，新版本已经生效”。

## 重载过程中摸清的游戏行为

- 游戏的组件类 `HTSkeletalMeshComponentBudgeted` 在网格被重设时会自己重建材质：
  - 布料的 14 个槽（cloth ×8、cloth2 ×6）重新创建动态材质，名字 `MID_<父材质名>_<序号>`，同时各配一个同名的、放在 `/Engine/Transient` 下的**描边材质**（父材质是游戏的描边材质，例如 `…/019_mint/ter_new_2/outline/cloth/player_019_mint_cloth`）。主动态材质在 +0x178 指向它的描边材质。
  - **身体的 2 个槽（body1）重设后放的是新材质本身，没有再包一层动态材质。** 第一次进场时这两个槽是有动态材质的（那是角色初始化时另一趟逻辑建的，重设网格不会再跑这一趟）。用户看过效果说可以，但受击闪白、溶解这类靠动态材质参数驱动的效果在身体部位是否还正常，没有专门验证。
  - 其余 24 个槽是游戏本体的材质，新旧网格相同，游戏没有动它们。
  - 所以我先做的“换父材质”对这个 Mod 其实是多余的（那些动态材质随后就被游戏换掉了），但无害；对网格不在包里、只换材质和贴图的 Mod 才是必须的。
- 组件 +0xdf8 是游戏自己的原生字段（反射里没有），重设网格后短时间内放着新网格指针，随后清空。
- 动画蓝图实例是网格的后处理动画蓝图实例（组件没有槽位指向那个类）。重设网格后引擎自己用新类建了新实例，旧实例和旧类随后被回收。
- 名字 ID 的范围（当前到块 444，约 0x1BD0000）和低位堆地址重叠，对象确实会分配在这么低的地址（例如 0x1755b80）。实测撞上过：两个 `WorldPartitionRuntimeLevelStreamingCell` 对象的名字字段（+0x18）数值正好等于 Mod 面板里 `HorizontalBox_33`、`HorizontalBox_36` 的地址。当时因为目标不是顶层资产所以没有写入。0.6.1 加了防护，见 DECISIONS。

## 已在真游戏里确认的引擎事实（之前各轮）

- 定位、挂载、垃圾回收请求都正常。垃圾回收入口 RVA `0x4406bd0`。
- 对象系统：名字池 Blocks RVA `0xfa36e10`；对象表 RVA `0xfb1af80`，条目 0x18 字节；类大小字段偏移 0x58；GEngine 全局 RVA `0xfcc6d90`。对象表要等窗口出现后才存在。
- `UObject::Rename` = RVA `0x181e3b0`，虚表第 46 项（偏移 0x170）。包类的重写 RVA `0x1845670` 只是转调。
- `UObject::ProcessEvent` = RVA `0x18aacd0`，虚表第 76 项（偏移 0x260）。
- `UFunction`：`FunctionFlags` 0xB0，`NumParms` 0xB4，`ParmsSize` 0xB6，`ReturnValueOffset` 0xB8，`Func` 0xD8，大小 0xE0。
- `FField`：类 0x08（`FFieldClass` 开头就是名字），`Next` 0x18，名字 0x20。`FProperty`：`ArrayDim` 0x30，`ElementSize` 0x34，`PropertyFlags` 0x38，`Offset_Internal` 0x44。
- 脚本函数：
  - `KismetSystemLibrary.LoadAsset_Blocking`：参数块 0x30，返回值偏移 0x28。
  - `SkeletalMeshComponent.SetSkeletalMeshAsset`：参数块 8。
  - `MaterialInstanceDynamic.CopyParameterOverrides(UMaterialInstance*)`：参数块 8。
  - `KismetSystemLibrary.GetFrameCount`：参数块 8（试调用用）。
  - `EmptyOverrideMaterials` 不是脚本函数。`HTPlayerCharacter.ResetCharacterMesh(bool)` 只重置网格的相对变换，没用。
- 反射属性偏移：`SkinnedMeshComponent.SkeletalMesh` +0x5b0、`SkinnedAsset` +0x5b8；`MeshComponent.OverrideMaterials` +0x548（黑鸟有 40 项）；`MaterialInstance.Parent` +0x128；`HTPlayerAppearance.FashionMeshData` +0x80；`HTPlayerCharacter.CurMeshData` +0x2950、`CurPlayerAppearance` +0x2960；`Character.Mesh` +0x348。
- 黑鸟不出场时角色对象照样活在关卡里，所以队伍内切换从来不会刷新资产。
- 返回登录界面再进入可以完全刷新（老办法，重载失败时的退路）。

## Next step

1. 等用户下次打包，验证自动重载：看日志里“重载:”几行和“重载完成”，以及 `HTReload.reload.txt`。
2. 用户若发现身体部位的特效不对，再处理 body1 两个槽没有动态材质的问题（可以在重设网格后调 `PrimitiveComponent.CreateDynamicMaterialInstance` 补上，但要先弄清游戏自己的容器认不认）。
3. 打包器的自动编号还没做，用户一直没答复。现在每次打包要手动换一个更大的编号。

用 `tools\ask.ps1` 发指令，用 `tools\disasm.ps1` 反汇编 `code` 指令取回的字节。
