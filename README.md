# ZeroARKTrait

ARK: Survival Ascended（飞升）服务器插件 —— **词条（Gene Trait）商店 / 管理工具**。
基于 [AsaApi](https://github.com/ArkServerApi/AsaApi)（ArkServerAPI）2.03，纯 C++ 单编译单元，**不依赖任何第三方库**。

> 官方做不到的事，这个插件能做：给**任意成年、跨物种**的恐龙**加 / 删 / 查**词条（官方 `givetrait` 必须手持基因扫描仪瞄准目标、按物种校验，而且没有删词条命令）。

## 功能

- **词条管理**：任意恐龙加 / 删 / 查词条，按 `DinoID` 精确定位（不依赖准星）
- **词条商店**（配合客户端 mod `ZeroTraitUI`）：玩家侧 F 键开面板、选龙、选词条、选级别（1/2/3 级）、下单
- **价格链**：`实付 = round(round(基础价 × (1 − 折扣/100)) × 层级系数 × 物种系数)`，**面板显示 = 服务端实扣**
- **点数三种来源**：本地点数文件（零依赖）/ MySQL（兼容 ArkShop 表结构）/ 外部推送
- **权限组折扣**：读官方 Permissions 插件的组别，支持取最大值 / 叠加两种模式
- **审计与回执**：每一单写 `trait_audit.log`；玩家不在线时把结果排队，上线自动补推
- **中英双语**：`messages-zh.ini` / `messages-en.ini`，词条名可跟随语言
- **进服提醒**：进场/重连自动提示（照 EnhancedHLNA 的做法）

## 依赖

| 依赖 | 说明 |
|---|---|
| AsaApi **2.03** | 宿主框架（必需） |
| 官方 **Permissions** 插件 | **可选**，动态加载，没装也能正常跑（权限判断回退到内置闸门） |
| 客户端 mod `ZeroTraitUI` | **可选**，只有用"图形面板"时才需要；纯命令行 / 聊天也能用 |
| 第三方库 | **一个都没有** —— 数据库是手写的 `src/MiniMysql.h`，只依赖系统库 `ws2_32` + `bcrypt` |

## 安装

1. **停服**（先 `SaveWorld` 再关，别强杀）
2. 把 `ZeroARKTrait.dll` + `PluginInfo.json` + 配置文件放进
   `<服务端>\ShooterGame\Binaries\Win64\ArkApi\Plugins\ZeroARKTrait\`
3. **启动服务器**
4. 检查日志：`shop settings: tier=1/1.5/2 points_mode=file ...`、`messages: lang=zh entries=...`
5. 核对：RCON `Trait 商店状态`

> ⚠️ **换版本一律"停服 → 覆盖 DLL → 启动"**。不要用 `plugins.unload` / `load`（实测会把服务器搞崩）。

## 配置

9 个配置文件（**零注释、纯 `键=值`、UTF-8 无 BOM**，文件名请勿改）：

| 文件 | 作用 |
|---|---|
| `ZeroARKTrait-settings.ini` | 商店设置：层级系数 / 点数模式 / 面板按键 / 折扣方式 / 权限 / 语言 / 进服提醒 |
| `ZeroARKTrait-prices.ini` | 词条基础价 |
| `ZeroARKTrait-species.ini` | 物种价值系数（官方中英名各一行；五档，上限 2 倍） |
| `ZeroARKTrait-discounts.ini` | 权限组折扣 |
| `ZeroARKTrait-messages-{zh,en}.ini` | 玩家可见文案（含面板全部文字，可加语言） |
| `trait_names.ini` | 词条别名表（别名 → 内部英文名） |
| `zerotrait.ini` | 运行参数（搜索半径 / 分片预算 / 聊天查询 / 列表上限） |

**逐键说明（中英双语）见 [`docs/ZeroARKTrait-config-guide.md`](docs/ZeroARKTrait-config-guide.md)**；
`docs/Commented/` 里另有 8 个**带注释的完整副本** —— 插件会跳过 `#` / `;` 开头的行，所以可以直接改那份当正式配置用。

## 常用命令（RCON / 服务端控制台）

| 命令 | 说明 |
|---|---|
| `Trait 商店状态` | 打印当前模式、价目条数、物种条数、点数模式等 |
| `Trait 买 <玩家> <词条> <1/2/3> [id:<ID1>:<ID2>]` | 下单（带 `id:` = 面板单，精确发货） |
| `Trait 预检 <玩家> <词条> <1/2/3> [id:...]` | 只检查能不能加（引擎会不会拒） |
| `Trait 算价 <玩家> <词条> <1/2/3> [id:...]` | 只报价（与实扣同一条公式） |
| `Trait 查点数 / 设点数 / 加点 / 删点数` | 点数管理（EOS ID 或在线玩家名） |
| `Trait 加 / 删 / 列表 / 龙 / 面板 / 订单 / 礼包 / 查单` | 基础管理命令（更多见配置文档） |

命令同时支持 **中文 / 拼音 / 英文** 三种写法（例：`买` / `mai` / `buy`）。

## 构建

见 [`src/BUILD.md`](src/BUILD.md)。要点：

- MSVC（v145 工具集）+ **静态 CRT（`/MT`）** —— 用动态 CRT 别人服上会报 `0xc0000020`
- 把 AsaApi SDK 放到仓库根的 `extern/`（`extern/AsaApi/...` + `extern/AsaApi/out_lib/AsaApi.lib` + `extern/fmt/`），工程文件已按这个约定写好
- 单编译单元：`src/Main.cpp`（新增全局/函数请放在文件上方的声明块里）

## 许可

MIT，见 [LICENSE](LICENSE)。
