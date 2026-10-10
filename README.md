# HT Reload

《异环》pak Mod 的热重载插件。游戏运行期间，把新名字的 Mod 三件套放进 `HT\Content\Paks` 下任意位置，插件会让引擎立即挂载它，不用重启游戏。

仅用于自己制作 Mod 时的迭代，不随 Mod 分发。

## 使用

1. 部署后正常启动游戏。`Win64\HTReload.log` 里出现“就绪”说明可以用了。
2. 改完 Mod，用打包器打包。**游戏开着时名称必须换新的**，在 `_P` 前加递增数字，例如 `blackbird_1_P`、`blackbird_2_P`。同名文件被游戏占用，无法覆盖。
3. 几秒内听到两声短促提示音，表示新包已挂载。一声低音表示引擎拒绝挂载，详情看日志。
4. 返回登录界面再进入游戏，新版本就会生效。队伍内切换角色、把角色换出队伍都不够，游戏不会因此卸载旧资产。

挂载后的 120 秒内，插件每 5 秒请求引擎做一次垃圾回收，旧资产没人引用后会被立刻清掉。这段时间里可能有轻微卡顿。

数字越大优先级越高，所以新版本一定压过旧版本。旧版本在游戏运行期间保持挂载，退出游戏后可以删掉，只留最新的。

## 构建、测试、部署

需要带 C++ x64 工具集的 Visual Studio 2022。

```powershell
.\build.ps1        # 生成 build\HTReload.asi（外壳）和 build\HTReload.core.dll（核心）
.\test\run.ps1     # 用替身宿主做端到端测试，不需要启动游戏
.\deploy.ps1       # 复制到游戏 Win64 目录
```

插件分成两部分。外壳 `HTReload.asi` 随游戏启动加载，常驻；核心 `HTReload.core.dll` 由外壳加载，文件一更新就自动换上新的。所以核心可以在游戏运行时部署，只有外壳变了才需要先关游戏。

停用：把游戏目录里的 `HTReload.asi` 改名或删除。

## 指令文件

在游戏 `Win64` 目录写一个 `HTReload.cmd.txt`，每行一条指令，核心会在一秒内执行并删掉它。

- `gc [秒数]`：请求一段时间的垃圾回收。
- `probe <标签> [过滤词]`：报告磁盘上路径含过滤词的 Mod 包所对应的已加载对象，以及谁还引用着它们，追加到 `HTReload.probe.txt`。
- `objects`、`obj`、`mem`、`vtable`、`code`、`strref`、`astrref`、`callers`、`names`、`refs`：只读地查看引擎内部，答案写到 `HTReload.out.txt`。说明见 `src\explore.h`。

## 工作方式

- 由游戏目录里的 Ultimate ASI Loader（`dsound.dll`）加载，只在 `HTGame.exe` 里生效。
- 不修改游戏代码，不依赖固定地址。运行时在内存里找到引擎自己的 `FCoreDelegates::MountPak` 处理函数和它绑定的对象，在游戏线程上调用。
- 每秒扫描一次 `Content\Paks`，新出现且写入完成的包才会挂载；启动时已存在的包不处理。
- 垃圾回收通过引擎自带的 `UKismetSystemLibrary::CollectGarbage` 请求，它只是置一个标志，引擎在下一帧自己回收。
- 诊断：每次挂载后 3 秒和 105 秒，各记录一次新包覆盖的旧对象是否还活着、被谁引用，写入 `Win64\HTReload.probe.txt`，全部完成时响三声。这一步只读内存，范围限于引擎自己的堆和游戏主模块的数据。

项目状态、设计依据和已知问题见 `.ai-memory\`。
