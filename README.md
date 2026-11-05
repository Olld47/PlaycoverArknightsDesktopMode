# AKDM — 明日方舟桌面模式（PlayCover 插件）

在 PlayCover 中为《明日方舟》(iOS) 启用 **PC 键位**。
插件只修改游戏进程内存，不写入游戏文件、不改存档。

**鼠标操作不由本插件处理**：PlayTools 已把鼠标转换为 Unity 触摸
（hook_GCMouse_mice / enableScrollWheel），与未装插件时一致。本插件只补
iOS 上缺失的键盘通路。

>
> 参考实现（详见文末[参考资料](#参考资料)）：
> - [playcoverNTEDesktopMode](https://github.com/Xingchuan000/playcoverNTEDesktopMode) —— 整体架构（PlayCover 插件布局、IL2CPP 符号解析思路）；
> - [EndPlayDesktop](https://github.com/HosamaJJF/EndPlayDesktop/blob/main/docs/plugin.md) —— 终末地（同为 Unity/IL2CPP 游戏）的实现，**直接给出了解决键盘通路的关键思路**：游戏在 iOS 上不查询 `Input.GetKey`，按键状态必须经未导出的 `UnitySetKeyState` 主动写入引擎（本项目第 3 层即照此实现）；
> - `libUnityDesktopMode.dylib`—— `__Hotfix0_IsPCMode` 委托槽注入与 `UnitySetKeyState` 指令模式定位手法

## 原理

明日方舟的 iOS 包里**完整保留了 PC 端键盘逻辑**（`TorappuKeyBoardLogic` 键位表、
`DeviceInfoUtil.IsPCMode`、按键配置界面等）。问题有三层：

1. 平台判定让 PC 分支永不启用；
2. iOS 上 Unity 的 `Input` 永远报告"没有键盘"；
3. 更重要的是：游戏的 PC 输入模块（`TorappuPCInputHelper`）在 iOS 构建中**从未被装配**，
   因此它根本不查询 `Input.GetKey` —— 光有第 2 层的钩子毫无作用。

插件分三个部分解决：

| 部分 | 做法 | 源码 |
| --- | --- | --- |
| 符号定位 | 从 `global-metadata.dat` + `Il2CppCodeGenModule` 重建 IL2CPP 符号表 | `src/AKDMTarget.cpp` |
| PC 模式 | 写入 `__Hotfix0_IsPCMode` 委托槽，让 `IsPCMode()` 恒真 | `src/AKDMHook.cpp` |
| 键盘 | **直接调用未导出的 `UnitySetKeyState`**，把按键写进 Unity 引擎自身状态 | `src/AKDMUnityKey.mm` |

### 为什么不是钩 `Input.GetKey`

钩 icall 槽的做法实测无效：钩子能装上（`3/3 installed`），键盘事件也到达插件
（`keys=38`），但游戏从不调用 `Input.GetKey`（替换函数一次都没被执行），因为
`TorappuPCInputHelper::OnProcess` 的前置门控 `[statics+0x50]` 在 iOS 上恒为 0。

正确路径是**主动把状态推进 Unity 引擎**。`UnitySetKeyState` 没有导出符号，只能通过
指令模式定位：

```asm
-[UnityView handleCommand:] 的 +0x3a0 处：
    mov  w1, #1          ; 第二个参数 true
    bl   <trampoline>    ; 跟随 B 指令链后得到真实入口
```

真实入口的序言是 `stp x20,x19,[sp,#-0x20]! / stp x29,x30 / add x29,sp`，函数体把
`x1` 存入 `x20`（bool）、`x0` 存入 `x19`（KeyCode），读全局状态表后转发。定位成功后
每次键盘事件都直接调用它。


### 输入接管方式

Unity 的每个输入包装函数都会把 icall 指针缓存到 `__DATA` 的一个槽里：

```asm
adrp x20, <page>
ldr  x1, [x20, #slot]     ; 首次使用前为 0
cbnz x1, .call
...                        ; 未解析：调用 il2cpp_resolve_icall 再写回
.call:
br   x1
```

插件把槽里的指针换成自己的实现。这是**纯数据写入**：不改任何指令、不需要跳板、
进程退出即消失。槽里原本的值（若已被游戏解析）保留为回退路径。

## 下载

在 [Releases](https://github.com/Olld47/PlaycoverArknightsDesktopMode/releases/latest) 页面下载：

| 文件 | 用途 |
| --- | --- |
| `libAKDMDesktopMode.dylib` | 正式版（无日志），**日常使用选这个** |
| `libAKDMDesktopMode-debug.dylib` | 带日志版，排查问题时使用；日志见[下文](#日志) |

各文件的 SHA256 校验和写在 Release 页面的说明里（CI 构建时生成）。
Release 由 CI 在 macOS runner 上构建并 ad-hoc 签名，哈希与自行构建不同（签名含
时间戳），因此以 Release 页面列出的校验和为准。

## 安装

1. 从 [Releases](https://github.com/Olld47/PlaycoverArknightsDesktopMode/releases/latest) 下载 `libAKDMDesktopMode.dylib`（也可自行构建，见[本地开发构建](#本地开发构建)）。
2. PlayCover 里选明日方舟 → 设置 → 杂项 → 添加插件 → 选择该 dylib。
3. **关闭 PlayCover 的所有键盘映射**（否则会与游戏原生输入冲突）。
4. 启动游戏。

## 键位

插件**不定义键位**：按键状态经 `UnitySetKeyState` 写入 Unity 引擎后，游戏自己的 PC 键位逻辑
（`TorappuKeyBoardLogic` 键位表）照常工作，默认与 PC 版一致。可用按键：字母/数字键、
方向键、Shift/Ctrl/Alt、Esc、Tab、空格、F1。
鼠标点击、拖动、滚轮全部走 PlayTools 原生触摸转换，插件不做任何干预。

## 配置

路径：

```text
~/Library/Containers/com.hypergryph.arknights/Data/Documents/AKDM/config.plist
```

见 `config.example.plist`：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `EnablePCMode` | true | 写入 `__Hotfix0_IsPCMode` 委托槽，让 `IsPCMode()` 恒真（PC 界面与键位的前提） |
| `EnableInputHooks` | true | 接管 `Input.GetKey/GetKeyDown/GetKeyUp` 的 icall 槽 |

不存在的文件、缺失的键都回退到默认值，不报错。

## 日志

默认关闭。诊断时重新构建：

```sh
AKDM_ENABLE_LOGGING=1 zsh build.sh
```

日志：`~/Library/Containers/com.hypergryph.arknights/Data/Documents/AKDM/plugin.log`

| 关键字 | 含义 |
| --- | --- |
| `metadata parsed` | 元数据解析成功，给出 image 数量 |
| `hook <方法>: slot ...` | 该 Unity 输入函数已接管，显示槽地址与原值 |
| `input hooks: N/M` | 成功接管数 / 尝试数 |
| `pc override: __Hotfix0_IsPCMode at ...` | PC 模式委托槽已写入（`IsPCMode()` 恒真） |
| `setkey: UnitySetKeyState at 0x...` | 定位到未导出的引擎按键函数（键盘生效的前提） |
| `tick=... keys=N pc=B` | 键盘事件计数（`keys` 随按键增长）、PC 模式状态（`pc=1`） |

> 按键无反应时优先看 `setkey`：没有 `UnitySetKeyState at ...` 这一行，说明指令模式
> 未命中（多为游戏更新导致函数布局变化）。

## 本地开发构建

### 构建

需要 macOS + Xcode Command Line Tools（含 Mac Catalyst 框架）。

```sh
zsh build.sh                 # release（无日志）
AKDM_ENABLE_LOGGING=1 zsh build.sh   # 带日志，用于诊断
```

产物：`libAKDMDesktopMode.dylib`（ad-hoc 签名，`@executable_path/Frameworks/` install name，
即 PlayCover 注入所要求的形态）。

### 测试与验证

```sh
zsh tools/test.sh
```

全部离线运行，不启动、不修改游戏：

```
== input state machine    PASS (0 failures)   # 键盘边沿语义
== symbol resolution      PASS (0 failures)   # 19 个目标方法
== input hooks and Touch  PASS (0 failures)   # icall 槽 + 模拟加载偏移
== plugin build           签名通过，Catalyst arm64
```

`tools/validate_target.cpp` 与插件共用 `AKDMTarget`，验证每个方法都能解析到真实可执行代码；
`tools/validate_hooks.cpp` 验证 icall 槽的位置与可写性，并在**模拟加载偏移**下重跑。

这两个验证器需要本机安装的游戏 bundle（读取 `global-metadata.dat` 与 UnityFramework，
二者不可再分发），默认路径为
`~/Library/Containers/io.playcover.PlayCover/Applications/com.hypergryph.arknights.app`，
也可作为第一个参数传入。因此 CI 只运行单元测试与构建，不运行这两个验证器
（见 `.github/workflows/release.yml`）。

### 源码结构

| 文件 | 职责 |
| --- | --- |
| `src/AKDMCompat.*` | 日志、`vm_read` 内存读取 |
| `src/AKDMTarget.*` | IL2CPP 元数据解析与符号解析 |
| `src/AKDMInput.*` | 无锁键盘状态（按住 + 帧边沿） |
| `src/AKDMHardware.mm` | GameController 键盘 → 输入状态桥接 |
| `src/AKDMHook.cpp` | Unity `Input` icall 接管、PC 模式覆盖 |
| `src/AKDMUnityKey.mm` | 定位并调用未导出的 `UnitySetKeyState` |
| `src/AKDMConfig.*` | 配置读取 |
| `src/AKDMPlugin.mm` | 入口、目标校验、PC 模式、定时维护 |

## 已知限制

- 静态验证针对明日方舟 **2.7.91 (build 76)**；其他版本插件会自行判断，不匹配则拒绝。
- **`UnitySetKeyState` 与热更委托槽都靠指令模式定位**，不依赖硬编码地址，但游戏大版本
  更新若改变 `-[UnityView handleCommand:]` 的调用形态或 `IsPCMode` 的字段布局，会定位
  失败。失败表现为**静默不生效（不崩溃）**，用 `AKDM_ENABLE_LOGGING=1` 构建后看日志：
  缺 `setkey: UnitySetKeyState at ...` 行即按键通路未建立，缺 `pc override: ...` 行即
  PC 模式未开启。
- 长按依赖 GameController 的按键重复回调触发重复注入；若某机型不产生重复回调，表现为
  长按不连续（点按不受影响）。后续可加周期性重发解决。
- 只接管键盘；触摸/鼠标行为与未装插件时完全一致（由 PlayTools 处理），多指手势仍需
  PlayCover 映射。
- `Cursor.get_visible` 在本构建中被裁剪，插件不依赖它。
- 若游戏某次更新后 PC 界面本身异常，可先在 config.plist 设 `EnablePCMode=false` 隔离
  验证（该键同时控制委托槽注入，是按键生效的前提）。

## 参考资料

- [EndPlayDesktop（终末地 PC UI 与输入插件）](https://github.com/HosamaJJF/EndPlayDesktop/blob/main/docs/plugin.md) ——
  同为 Unity/IL2CPP + PlayCover 场景，其文档「实现对照」表直接指出：Legacy 键鼠链路必须
  定位未导出的 `UnitySetKeyState` 并替换 icall 缓存，仅 `dlsym` 会缺这条链路。本项目的
  第 3 层键盘通路即据此实现。
- [playcoverNTEDesktopMode（异环，UE 引擎）](https://github.com/Xingchuan000/playcoverNTEDesktopMode) ——
  整体架构参考（插件布局、平台查询改写、严格的校验/回滚风格）。异环为 UE，输入机制与
  Unity 不同，仅工程方法可借鉴。
- [PlayTools MaaTools 分支](https://github.com/hguandl/PlayTools/tree/MaaTools) ——
  PlayCover 的插件加载机制（`UserPlugins` 目录）与鼠标→触摸转换（`hook_GCMouse_mice`）。
- `libUnityDesktopMode.dylib` 大佬写的聚合桌面模式插件

## 验证状态

| 项目 | 状态 |
| --- | --- |
| 符号解析（90 images / 19 方法） | 通过 |
| 3 个键盘 icall 槽解码 | 通过 |
| 键盘状态机单元测试 | 通过 |
| 编译 / 签名 / Catalyst arm64 | 通过 |
| `UnitySetKeyState` 定位 | 通过（离线命中 0x8a9ef70） |
| **游戏内实际运行** | **已验证：按键被游戏响应** |

游戏内实测（明日方舟 2.7.91）：`setkey: UnitySetKeyState at 0x... (BL +0x3a0 ->
trampoline ...)`，键盘输入成功驱动游戏。
