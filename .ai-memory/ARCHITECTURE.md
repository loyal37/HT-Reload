# Architecture

## 项目目录结构

- `shell/shell.cpp`：外壳，编译成 `HTReload.asi`。
- `src/`：核心，编译成 `HTReload.core.dll`。`src/host.h` 是两者之间的约定。
- `test/host.cpp`：替身宿主，编译成 `HTGame.exe`，不启动真游戏就能测试插件。
- `test/run.ps1`：端到端测试。
- `build.ps1`：构建外壳和核心；`-Test` 额外构建替身宿主。
- `deploy.ps1`：复制到游戏 Win64。核心随时可以部署；外壳有变化而游戏在运行时拒绝。

## 外壳与核心（v0.4.0）

- 外壳由 ASI 加载器加载，进程存续期间不卸载，只在 `HTGame.exe` 里生效。它负责三件事：
  - 日志文件（启动时把上一次的改名为 `HTReload.prev.log`）。
  - 游戏线程调度：对 `UnrealWindow` 所在线程装 `WH_GETMESSAGE` 钩子，钩子函数在外壳里。任务队列里只存“函数指针 + 上下文”。
  - 加载核心：把 `HTReload.core.dll` 复制成 `HTReload.core.run<N>.dll` 再加载，所以原文件随时可以被覆盖。每秒检查原文件的修改时间和大小，连续两次相同且与已加载的不同就更换。
- 更换核心的顺序：调核心的 `CoreStop`（核心置停止事件并等工作线程结束，最多 60 秒）→ 外壳拿独占锁清空任务队列，保证此后没有核心代码在游戏线程上运行 → `FreeLibrary` 并删掉副本 → 加载新的。核心没能按时停下时不卸载，只记日志。
- 外壳在游戏线程上执行任务时持有共享锁；任务执行期间又进入消息循环的话不重入。
- 核心导出 `CoreStart(const CoreHost*)` 和 `CoreStop()`。`CoreHost` 里有约定版本、第几次加载、插件目录、写日志、游戏线程是否就绪、投递任务。
- 核心每次启动都重新定位挂载入口和垃圾回收入口，并把磁盘上现有的 pak 全部记为已挂载。只有进程里第一次加载时才先等 3 秒。
- 核心的所有等待都能被停止事件打断，包括等待挂载结果。
- `build/`：构建产物，不入库。
- `.ai-memory/`：项目持久记忆。
- `.codex/skills/project-memory-keeper/`：本项目的记忆维护 Skill。

## 核心模块职责

- `main.cpp`：`CoreStart` 启动工作线程。工作线程依次完成定位、目录快照、等游戏线程就绪、每秒轮询。`Watcher` 负责发现新包并发起挂载、请求垃圾回收、执行指令文件。
- `explore.{h,cpp}`：只读查看指令。能把一个指针解释成“某函数 + 偏移”“主模块数据”“某个对象”或“某对象内部”。
- `engine.{h,cpp}`：定位并调用重载要用的引擎函数。
  - `UObject::Rename`：它的日志文字存在一个静态记录里，代码引用的是记录而不是文字本身。先找文字，再找指向文字的指针（即记录），再找引用记录的函数。然后抽样约 400 个对象，看它在虚表第几项，取多数。
  - `UObject::ProcessEvent`：虚表里开头 0x80 字节内含 `test dword ptr [rdx+偏移], 400h`（判断函数是不是原生函数）的那一项，同样取多数；那个偏移就是 `UFunction::FunctionFlags` 的偏移。
  - 调用都走对象自己的虚表，所以子类的重写会被正确调到。调用包在 SEH 里。
- `reload.{h,cpp}`：`Reloader`。`engine`、`calltest`、`plan`、`renameload`、`reload` 五条指令的实现，槽位的分类规则，以及完整重载本身。详见下面“原地重载”一节。
- `ObjectView`（在 `objects.{h,cpp}`）：一次对象表快照加上按地址查找、取名字、取路径、判断类继承。新代码都用它。
- `tools/ask.ps1`：往运行中的游戏发指令并打印答案。`tools/disasm.ps1`：把 `code` 取回的字节写成汇编数据，用 `ml64` 汇编、`dumpbin /disasm` 反汇编，并把地址换算回游戏里的 RVA。
- `resolver.{h,cpp}`：在运行中的游戏里定位挂载入口，输出 `MountApi { handleMountPak, pakPlatformFile }`。
- `image.{h,cpp}`：读取主模块内存。按页保护属性把已提交内存分成可执行、只读、可写三类；提供字节串查找、`lea [rip+disp32]` 引用扫描、函数起点查询（`RtlLookupFunctionEntry` 并跟随链式展开信息）。所有读取都有 SEH 保护。
- `game_thread.{h,cpp}`：核心一侧的任务队列，通过外壳投递到游戏线程。
- `log.{h,cpp}`：格式化后交给外壳写日志。

## 定位方法

1. 在只读数据里找 UTF-16 字符串 `Mounting pak file: %s \n`。它是 `HandleMountPakDelegate` 和 MountPaksEx 处理函数传给 `LowLevelOutputDebugStringf` 的参数，不是 `UE_LOG`，所以 Shipping 版里也保留。
2. 在代码段找引用该字符串的 `lea`，取所在函数起点，得到候选函数（原版引擎是 2 个）。
3. 扫描主模块可写数据里的每个指针，看它指向的堆块前 12 个机器字里是否有“对象指针 + 候选函数地址”相邻的一对。这正是 `FCoreDelegates::MountPak` 的委托实例里 `{ UserObject, MethodPtr }` 的布局。对象要求是堆指针且虚表在主模块内。
4. 命中的函数就是 `HandleMountPakDelegate`，前一个字就是 `FPakPlatformFile*`。只有它被直接绑定到委托，另一个候选不会命中。

代码在游戏启动过程中才解密，委托在引擎初始化 pak 系统后才绑定，所以定位会重试：先等 3 秒，前 15 次间隔 1 秒，之后间隔 5 秒，300 次后放弃。候选不足 2 个时每次都重扫。

## 挂载调用

- 签名：`IPakFile* HandleMountPakDelegate(FPakPlatformFile* this, const FString& PakFilePath, int32 PakOrder)`。
- `FString` 按 `{ wchar_t* Data; int32 Num; int32 Max }` 构造，`Num` 含结尾空字符。
- 路径用引擎自己的写法：`../../../<Project>/Content/Paks/<相对路径>`，正斜杠。`<Project>` 取 exe 上两级目录名（游戏里是 `HT`）。
- `PakOrder` 传 `-1`（`INDEX_NONE`），让引擎用和启动扫描相同的规则算顺序。
- 调用包在 SEH 里；一旦抛异常就记录并停止后续挂载。

## 垃圾回收请求

- 目标函数：`UKismetSystemLibrary::execCollectGarbage`。它执行 `GEngine->ForceGarbageCollection(true)`，只置标志，引擎在下一次 Tick 里自己做完整回收。
- 定位：在只读数据里找 ASCII `CollectGarbage`（前后都是空字符，正好 1 处），再找指向它的 8 字节指针。UHT 生成的两张表都含这个名字：
  - `FClassFunctionLinkInfo { 构造函数, 名字 }`：相邻条目的函数大小全部相同。
  - `FNameNativePtrPair { 名字, exec 函数 }`：相邻条目的函数大小各不相同，名字后面紧跟的就是要找的函数。
  - 取名字前后各 3 个条目的函数大小，全相同且非零的判为构造表并跳过。
- 调用：`thunk(nullptr, frame, nullptr)`，`frame` 是 0x200 字节的全零缓冲区，冒充 `FFrame`。其 `Code` 指针为空，`P_FINISH` 不做任何事。静态函数不使用 `Context`。
- 节奏：一次挂载成功后 120 秒内每 5 秒请求一次，在游戏线程执行。调用包在 SEH 里，出异常就停用。
- 2026-10-01 版本离线核对的 RVA：名字 `0xbf660a0`，构造表条目 `0xbf50a08`（函数大小全是 0x2f），原生表条目 `0xbfb57e0`，exec 函数 `0x4406bd0`（叶函数，没有展开信息）。
- 游戏自己还有一个名叫 `ForceGarbageCollection` 的函数，但它和相邻几个函数指向同一个地址，是 Shipping 版里的空桩，不能用。

## 原地重载（核心 0.6.x，2026-10-10 在真游戏里验证通过）

- 入口：`Reloader::Reload(包 ID 列表, 报告)`，返回 `Nothing`（这些包都没加载）、`Done`、`Failed`。挂载成功后 `Watcher::ReloadMounted` 自动调用；也可以用指令 `reload [过滤词]` 手动触发。`plan [过滤词]` 只列出会做什么，不改任何东西。
- 准备（工作线程，只读）：`FindSuperseded` 找出被新包覆盖的已加载包、它们里面的全部对象、以及其他对象体内指向这些对象的槽位；`PrepareWork` 把要做的事全部算好。
  - 要加载的资产 = 包里直属的公开对象，类默认对象除外。
  - 槽位分类（`Classify`）：
    - 不动：槽位的主人是旧类的实例（比如动画蓝图实例的类指针），它会随旧资产一起消失。
    - 存疑：旧对象的地址同时也是一个名字编号，而主人的类没有在这个偏移声明对象引用。永不改写。
    - 换父材质：主人是 `MaterialInstanceDynamic` 且槽位正是 `Parent`（偏移用反射查，实测 +0x128）。
    - 重设网格：主人是关卡里的（非模板）`SkinnedMeshComponent`，目标是 `SkeletalMesh`。
    - 改写：其余，直接存新指针。
- 执行（游戏线程，一个任务内做完，120 秒超时；这样加载出的新对象在被引用之前不会遇到垃圾回收）：
  1. 给所有旧包改名：`UObject::Rename`，新名字 `<原名>_HTOLD_<十六进制时刻>`，标志 0x35。
  2. 逐个 `KismetSystemLibrary.LoadAsset_Blocking`，软引用里的包名和资产名直接用旧对象的名字 ID。加载器发现已加载的包被改了名，会丢掉旧记录重新读盘，读到的是顺序最高的容器。
  3. 改写类槽位：存新指针前先核对槽位里还是旧值（`ReplacePointer`，带 SEH）。
  4. 换父材质：存新 `Parent`，再调 `MaterialInstanceDynamic.CopyParameterOverrides(新材质)`，它内部会 `InitResources()` 把新父材质推给渲染线程。副作用是这个动态材质上由游戏设的参数被清掉。
  5. 重设网格：每个组件调一次 `SkeletalMeshComponent.SetSkeletalMeshAsset(新网格)`；之后该组件的槽位若还是旧值就直接改写。
- 之后开 20 秒的垃圾回收窗口，旧对象全部被回收。
- 任何一步引擎抛异常就停在那里，报告写明停在哪一步和异常码。
- 报告：自动重载写到 `Win64\HTReload.reload.txt`，摘要行进日志（前缀“重载:”）；手动指令的报告在 `HTReload.out.txt`。
- 提示音：挂载成功两声上行；重载完成再加一声更高的；重载没完成或挂载失败是一声低音。

## 对象系统和引用探测（v0.3.0，只读诊断）

- `objects.{h,cpp}`：`ObjectSystem` 按形状在主模块可写数据里找两样东西，不依赖地址。
  - 名字池：`FNameEntryAllocator::Blocks` 的第一个块以条目 `None`、`ByteProperty` 开头。条目头 16 位：最低位是否宽字符，高 10 位长度。名字 ID 高 16 位是块号，低 16 位乘 2 是块内偏移。
  - 对象表：`FChunkedFixedUObjectArray { Objects, PreAllocated, MaxElements, NumElements, MaxChunks, NumChunks }`。用计数互相吻合、且前 64 个条目指向的对象 `InternalIndex` 等于自身序号来确认。条目大小依次试 0x18（未压缩）、0x10（`UE_PACK_FUOBJECT_ITEM` 压缩指针）、0x20、0x28。
  - 对象头按 UE 5.6：虚表 0x00，`ObjectFlags` 0x08，`InternalIndex` 0x0C，类 0x10，名字 0x18（ID 和序号各 4 字节），外层 0x20。
  - 对象大小取类的 `PropertiesSize`。它的偏移用 `Object`（0x28）和 `Field`（0x30）两个类加上紧随其后的对齐值 8 来标定，游戏里和原版一样是 0x58。对齐值 `MinAlignment` 只有 16 位（后面紧跟另一个 16 位值 1），必须只比较低 16 位，v0.3.2 按 32 位比较所以失败。标定失败时用相邻对象的间距估计。
  - 游戏里 `UStruct` 的布局实测与原版一致：`SuperStruct` 0x40，`Children` 0x48，`ChildProperties` 0x50，`PropertiesSize` 0x58，`MinAlignment` 0x5C。
  - 对象表要等引擎初始化对象系统之后才存在，比 pak 系统晚十几秒，所以在游戏窗口出现后才找，找不到就在挂载或执行指令时重试。名字池很早就有。
  - 真游戏实测（2026-10-01 版本）：名字池 Blocks RVA `0xfa36e10`；对象表 RVA `0xfb1af80`（Anticensor 日志里的 `A @ RVA 0xFB1AF70` 正是它前面 0x10 字节的 `GUObjectArray` 起点）；条目 0x18 字节未压缩；启动早期创建的对象在 `0x7ff4…` 这样的高地址。
- `probe.{h,cpp}`：
  - 从新包的 `.utoc` 读出包 ID：文件头 144 字节（魔数 `-==--==--==--==-`，偏移 20 是头大小，24 是条目数），之后每 12 字节一个块 ID，前 8 字节是 ID，最后 1 字节是类型，类型 1 是包数据。
  - 包 ID = `CityHash64`（小写的 UTF-16 包名），实现在 `cityhash.cpp`，已用真实的 `blackbird_2_P.utoc` 验证 6 个包名全部命中。
  - 没有外层的对象就是包。包名哈希落在新包 ID 集合里的，就是被新包覆盖的已加载包；最外层是这些包的对象是“旧对象”。
  - 扫描范围：主模块的可写数据，加上所有“至少放着一个引擎对象”的内存分配。无法这样归到引擎头上的内存一律不读。
  - 第一遍找直接等于旧对象地址的槽位：落在对象表里的忽略，落在旧对象体内的算内部引用，落在其他对象体内的报告为直接引用者，其余是“对象外的指针”（多半是 `TArray` / `TMap` 的堆存储）。
  - 第二遍给对象外的指针找主人：从该槽位往回最多 0x800 字节、且不越过别的对象，这些地址作为可能的堆块起点，再扫一遍找指向它们的对象或主模块全局。
- `arena.{h,cpp}`：探测自己的表用单独的 `VirtualAlloc` 分配，扫描时能认出来并跳过。
- 包路径的事实：这个 Mod 的包名是 `/Game/Characters/Player/042_Blackbird_1/<名字>`，写在资产内部。打包源目录 `xg\HT\Content\Characters\042_Blackbird_1` 少一层 `Player`，`.utoc` 目录索引里的路径跟着磁盘走，所以不能用目录索引推包名。

## 数据流

1. 游戏启动，`dsound.dll` 加载 `HTReload.asi`。
2. 工作线程定位成功后，把 `Content\Paks` 下现有的 pak 全部记为“已处理”。委托是在引擎启动扫描之后才绑定的，所以此刻磁盘上的包都已被引擎挂载。
3. 等到游戏窗口出现，装线程钩子，日志写“就绪”。
4. 每秒扫描一次。新 pak 需满足：同名 utoc 和 ucas 要么都在要么都不在；每个文件都能以只读共享方式打开（打包器还在写时会失败）；总大小连续两次轮询不变。
5. 满足后投递到游戏线程调用挂载，工作线程等结果，写日志并响提示音。
6. 挂载成功后立刻对这个容器里的包做原地重载（核心 0.6.2 起）。
7. 重载完成开 20 秒的垃圾回收请求窗口；没有可重载的内容或重载没完成则开 120 秒（配合返回登录界面的老办法）。

## 引擎侧依据（UE 5.6 原版源码）

- `Runtime/PakFile/Private/IPlatformFilePak.cpp`
  - 4414–4504：`Mount` / `Unmount` 控制台命令在 `#if !UE_BUILD_SHIPPING` 内。
  - 5716：`FCoreDelegates::MountPak.BindRaw(this, &FPakPlatformFile::HandleMountPakDelegate)`，在 `MountAllPakFiles` 之后。
  - 5951–5976：文件名以 `_P.pak` 结尾时顺序加 `100 * 版本号`；`name_P` 版本号为 1，`name_N_P` 为 N+1。
  - 6047–6066：挂载 pak 时一并挂载同名 utoc/ucas 并登记到包存储。
  - 6384：`GetPakOrderFromPakFilePath`，路径以 `ProjectContentDir` 开头得 3。
  - 6406：`HandleMountPakDelegate`。
- `Runtime/PakFile/Private/FilePackageStore.cpp:410`：容器按顺序降序排列，顺序相同则后挂载的在前。
- `Runtime/Core/Private/Windows/WindowsPlatformFile.cpp`：读文件不带 `FILE_SHARE_DELETE`，所以已挂载的文件不能改名或覆盖。

## 游戏可执行文件的情况（2026-10-01 版本）

- 所有节都叫 `.std`，入口点在最后一个节，是壳。
- 代码节在磁盘上 63% 的页全为零，其余是密文；只读数据是明文，展开表（892,503 项）完整。
- 因此字符串可以离线核对，代码只能在运行时读。离线核对用的是 Blender 自带的 Python（有 numpy）读 PE，临时脚本没有保留。
- 该版本的 RVA，仅供对照：锚点字符串 `0xb7bc558`，`Unmounting pak file: %s \n` 在 `0xb7bc608`，`_P.pak` 在 `0xb7bc1f8`，`UnrealWindow` 在 `0xb4c2028`。

## 测试宿主

`test/host.cpp` 只复现插件依赖的几件事：被两个函数引用的锚点字符串、堆上保存 `{对象, 成员函数}` 的委托实例（启动 2.5 秒后才绑定）、含 `CollectGarbage` 的构造表和原生表、名字池和对象表（1500 多个对象）、`UnrealWindow` 窗口和消息循环（4 秒后才创建）。挂载被调用时检查路径、顺序参数、字符串长度和调用线程；之后还要在主线程收到一次垃圾回收请求，全部正确才返回 0。

宿主里有一个带根集标志的贴图对象，被一个对象直接指向，又被另一个对象通过堆数组间接持有。`test\run.ps1` 用宿主的 `--write-utoc` 生成只含该包的 `.utoc`，并检查探测报告里这三样都找到了，且没有误报无关对象。只检查挂载后 3 秒的那次探测。
