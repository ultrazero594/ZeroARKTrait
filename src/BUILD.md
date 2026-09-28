# 自己改 / 重新编译 · Build it yourself

**先说结论**：想改**价格 / 物种系数 / 折扣 / 文案 / 语言 / 按键 / 点数来源** ⇒ **改 9 个 `.ini` 就行，不用碰代码**
（每个键的说明见 **`ZeroARKTrait-config-guide.md`**，中英双语）。
本文件只管**改功能**这件事。

---

## 1. 依赖（就这几样）

| 东西 | 说明 |
|---|---|
| Visual Studio 生成工具（MSVC x64）| 命令行 `MSBuild.exe` 就够，**不需要装完整 IDE** |
| AsaApi 的头文件与导入库 | 官方 AsaApi 自带（放在 `extern\AsaApi`：头文件 + `out_lib\AsaApi.lib`）—— **请用与你服务端同版本的那份** |
| 官方 **Permissions** 插件（**可选**）| 本插件**唯一的可选依赖**  而且是**动态加载**（`LoadLibrary` + `GetProcAddress`）⇒ **没装也照样编译、照样能跑** （权限判断与折扣会自动回退成"无折扣"） |
| 第三方库 | **一个都没有**  — 数据库交互是手写的 `MiniMysql.h`，只依赖 Windows 系统库 `ws2_32` + `bcrypt` |

## 2. 编译

- 工程文件：`ZeroARKTrait.vcxproj`
- 配置：`Release | x64`
- 平台工具集：`v145`（本项目实测；写别的版本号可能报 `MSB8020`）
- **必须是静态 CRT** — 工程里要显式写 `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>`（`/MT`）
  动态 CRT（`/MD`）会把 `MSVCP140.dll` 变成运行时依赖 ⇒ 别人的服上会报「**损坏的映像 0xc0000020**」

命令行例子：

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  .\ZeroARKTrait.vcxproj /p:Configuration=Release /p:Platform=x64 `
  "/p:SolutionDir=<本目录绝对路径，记得以反斜杠结尾>" /v:minimal /nologo
```

产物：`out\ZeroARKTrait.dll` ⇒ 拷到
`<服务端>\ShooterGame\Binaries\Win64\ArkApi\Plugins\ZeroARKTrait\` 即可（覆盖后重启服务器生效；想热重载就同时放一份同内容的 `ZeroARKTrait.dll.ArkApi`）。

## 3. 源码就两个文件

| 文件 | 内容 |
|---|---|
| `Main.cpp` | 插件全部功能（单文件，约 3000 行） |
| `MiniMysql.h` | 手写的最小 MySQL 客户端（只为读/写点数那一张表） |

## 4. 改代码时请守住的几个约定

1. **单编译单元** ⇒ **新加的全局变量与函数，请放到文件上方那块「声明块」里**
   （几千行一个 `.cpp`，声明必须在被使用的位置之前，否则编译报 `未声明的标识符` / `找不到标识符`）
2. **协议红线**：协议行 `名字[tier]`、`REQ|…`、审计日志、数据库 `tier` 列 —— **一律 `0/1/2`**
   只有**给玩家看**的层级才是 `1/2/3`（换算只出现在"玩家输入解析"和"显示"两处）
3. **玩家可见的文案不要写死在代码里**  ⇒ 一律走 `ZeroARKTrait-messages-<语言码>.ini`
   （缺键时会在日志里报 `msg key missing` 并在屏幕上显示 `[msg:key]`，方便你一眼看出漏配）
4. **配置文件保持"零注释、纯 `键=值`、UTF-8 无 BOM"**  —— 注释统一写在 `ZeroARKTrait-config-guide.md` 里
5. **写文件时文件名用英文** （中文路径在 Windows 上会让 `std::ifstream(const char*)` 打不开）

## 5. 想让它支持"新语言"或"新词条"

- **新语言**：复制一份 `ZeroARKTrait-messages-en.ini` → 改名 `ZeroARKTrait-messages-<你的语言码>.ini` →
  把值翻译掉（**键名一个都别改**）→ `-settings.ini` 里 `msg_lang=<你的语言码>`
- **新词条**：在 `ZeroARKTrait-prices.ini` 里加一行 `词条名=基础价`
  - 名字用**游戏内的官方中文名**最省事（插件内置表认得 51 条官方名 + 45 条旧简称）
  - 更新版本游戏带来的**新词条**：用它的**内部英文名**（如 `Vampiric`）也能用，只是列表里会显示英文原名
  - 想让它显示成中文 ⇒ 用 `trait_names.ini` 加一条 `你的叫法=内部英文名`，或在代码里给内置表补一行

## 6. 许可与免责

代码随包提供，**按现状提供、不担保**；因使用本插件造成的任何损失（存档、点数、纠纷等）由使用者自负。
用到的第三方组件只有官方 **AsaApi** 与官方 **Permissions** 插件，请遵循它们各自的许可。
