# Architecture

## 项目目录结构

- `src/`：ASI 源码。
- `test/host.cpp`：替身宿主，编译成 `HTGame.exe`，不启动真游戏就能测试插件。
- `test/run.ps1`：端到端测试。
- `build.ps1`：构建 `build\HTReload.asi`；`-Test` 额外构建替身宿主。
- `deploy.ps1`：复制 ASI 到游戏 Win64，游戏运行时拒绝。
- `build/`：构建产物，不入库。
- `.ai-memory/`：项目持久记忆。
- `.codex/skills/project-memory-keeper/`：本项目的记忆维护 Skill。

## 核心模块职责

- `main.cpp`：`DllMain` 只在主程序名为 `HTGame.exe` 时启动工作线程。工作线程依次完成定位、目录快照、接入游戏线程、每秒轮询。`Watcher` 负责发现新包并发起挂载。
- `resolver.{h,cpp}`：在运行中的游戏里定位挂载入口，输出 `MountApi { handleMountPak, pakPlatformFile }`。
- `image.{h,cpp}`：读取主模块内存。按页保护属性把已提交内存分成可执行、只读、可写三类；提供字节串查找、`lea [rip+disp32]` 引用扫描、函数起点查询（`RtlLookupFunctionEntry` 并跟随链式展开信息）。所有读取都有 SEH 保护。
- `game_thread.{h,cpp}`：找到本进程里类名为 `UnrealWindow` 的窗口，对它所在线程装 `WH_GETMESSAGE` 线程钩子；任务入队后用 `PostThreadMessage` 唤醒，在游戏线程取消息时执行。
- `log.{h,cpp}`：UTF-8 带 BOM 日志，每次启动覆盖。

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

## 数据流

1. 游戏启动，`dsound.dll` 加载 `HTReload.asi`。
2. 工作线程定位成功后，把 `Content\Paks` 下现有的 pak 全部记为“已处理”。委托是在引擎启动扫描之后才绑定的，所以此刻磁盘上的包都已被引擎挂载。
3. 等到游戏窗口出现，装线程钩子，日志写“就绪”。
4. 每秒扫描一次。新 pak 需满足：同名 utoc 和 ucas 要么都在要么都不在；每个文件都能以只读共享方式打开（打包器还在写时会失败）；总大小连续两次轮询不变。
5. 满足后投递到游戏线程调用挂载，工作线程等结果，写日志并响提示音。
6. 挂载成功后开启 120 秒的垃圾回收请求窗口。

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

`test/host.cpp` 只复现插件依赖的几件事：被两个函数引用的锚点字符串、堆上保存 `{对象, 成员函数}` 的委托实例（启动 2.5 秒后才绑定）、含 `CollectGarbage` 的构造表和原生表、`UnrealWindow` 窗口和消息循环（4 秒后才创建）。挂载被调用时检查路径、顺序参数、字符串长度和调用线程；之后还要在主线程收到一次垃圾回收请求，全部正确才返回 0。
