# ZeroARKTrait v1.0

ARK: Survival Ascended **服务端插件**（AsaApi 2.x）：基因词条商店的**定价 / 扣点 / 按 DinoID 发货 / 审计**，并把面板数据推送给客户端 mod **ZeroTraitUI**。

## 这个包里是什么

`ZeroARKTrait-v1.0.zip` 解压后是一个可直接拷进服务器的插件文件夹：

| 文件 | 说明 |
|---|---|
| `ZeroARKTrait.dll` | 编译好的插件（825,856 字节，静态 CRT，无 VC++ 运行库依赖） |
| `PluginInfo.json` | 插件信息（AsaApi 读取） |
| `ZeroARKTrait-settings.ini` | 主设置（点数来源、扣费方式、进服提醒、按键、推送间隔…） |
| `zerotrait.ini` | 配套开关 |
| `ZeroARKTrait-prices.ini` | 词条基础价（默认 51 条） |
| `ZeroARKTrait-species.ini` | 物种系数（5 档：0.15 / 0.5 / 1.0 / 1.5 / 2.0） |
| `ZeroARKTrait-discounts.ini` | 按权限组折扣 |
| `ZeroARKTrait-messages-zh.ini` / `-messages-en.ini` | 面板与聊天文案（中文 / 英文） |
| `trait_names.ini` | 词条名别名表 |
| `ZeroARKTrait-config-guide.md` | 配置指南：每个键的作用与取值 |

想自己改代码 / 重新编译 ⇒ 用仓库里的 `src\`（见 `src\BUILD.md`），本 Release 只给成品。

## 装它

1. 解压 `ZeroARKTrait-v1.0.zip`；
2. 把里面的 `ZeroARKTrait\` 整个文件夹拷到
   `<你的服务端>\ShooterGame\Binaries\Win64\ArkApi\Plugins\ZeroARKTrait\`
3. 重启服务器（换插件时请**停服再覆盖** DLL，不要用 `plugins.unload`）。

启动日志出现这一行就算加载成功：

```
ZeroARKTrait: prices=51 species=… tier=1/1.5/2 discounts=… msgs=…
```

## 玩家侧（客户端 mod）

面板由客户端 mod **ZeroTraitUI**（CurseForge 项目 `1704776`）提供，服务器的 mod 列表里要带上它。
没有它插件照样工作，只是玩家没有图形面板（管理命令与聊天指令不受影响）。

## 配置里能改什么（不用碰代码）

- **价格**：`-prices.ini` 的 `词条=基础价`
- **物种系数**：`-species.ini`（上限 2 倍，PvE 口径）
- **折扣**：`-discounts.ini`（按权限组，0~50%）
- **文案 / 语言**：`-messages-zh.ini` / `-messages-en.ini`，`msg_lang=zh|en`
- **点数来源**：`-settings.ini` 的 `points_mode = file | db | push`
  （`db` 用 ArkShop 的库表布局 `arkshopplayers` / `EosId` / `Points`，也可以让插件自建库表）

`docs\Commented\` 里有 8 份**带逐键中英注释**的完整配置，可以直接拿来当正式配置用（插件会跳过 `#` 开头的行）。
仓库里的 `config\` 是零注释的纯净版。

## 依赖

- **AsaApi 2.x**（必须）
- **官方 Permissions 插件**（可选，动态加载 —— 没装也能跑，只是没有权限组折扣）
- 客户端 mod **ZeroTraitUI**（可选，只影响图形面板）
- **无第三方依赖**：数据库交互是手写的 `MiniMysql.h`，只依赖 Windows 系统库 `ws2_32` + `bcrypt`

## 注意

- 用中文 RCON / 控制台命令（`Trait 查点数` 这类）需要 **UnicodeRCONASA**；纯英文别名（`Trait getpoints`）在自带 RCON 下也能用。
- 插件只管理词条，不改存档结构；发货走游戏原生 `scriptcommand givetrait`。
- 换 DLL 前先 `SaveWorld`。

---

MIT License. 按现状提供、不担保。
