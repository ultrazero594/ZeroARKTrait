# ZeroARKTrait — Configuration Guide / 全配置说明

> **中英双语 / Bilingual**（中文在前，English below each item）
> 配套插件 / Companion plugin: **ZeroARKTrait**（ArkApi / ARK: Survival Ascended）
> 需要客户端 mod / Requires client mod: **ZeroTraitUI**（CurseForge Project ID `1704776`）

---

## 0. 总则 / General rules

| 规则 / Rule | 说明 / Notes |
|---|---|
| **配置文件不含注释** | 所有文件都只放 `键=值`，一行一条；说明文字全部在本文件里。 |
| **No comments inside config files** | Every file contains `键=值` (key=value) lines only, one per line; all explanations live in this document. |
| 编码 / Encoding | **UTF-8（不带 BOM）** / UTF-8 **without BOM** |
| 空行 / Blank lines | 允许（插件会忽略），不推荐 / Allowed (ignored by the plugin), not recommended |
| 以 `#` 或 `;` 开头的行 / Lines starting with `#` or `;` | 插件会跳过（不推荐使用）/ Skipped by the plugin (not recommended) |
| 大小写 / Case | 键名**区分大小写**，本插件一律小写 / Key names are **case-sensitive**; lowercase throughout |
| 文件名 / File names | **必须保持英文原名**（插件按名字读）/ **Keep the English file names** (the plugin loads them by name) |
| 重启生效 / Reload | 改完要重启服务器（或重载插件）/ Restart the server (or reload the plugin) after editing |

**带注释的完整副本 / Annotated full copies:** 交付包里另有 `Commented\` 夹，装的是这 8 个配置文件的**完整内容 + 逐项注释**版本（每键上面一行说明）。插件会**跳过以 `#` 或 `;` 开头的行**，所以你可以直接改那份、放进插件目录当正式配置用，不用来回对照文档 / The package also ships a `Commented\` folder with the **full, per-key annotated** versions of all 8 configs. The plugin **skips lines starting with `#` or `;`**, so you can edit one of those files and drop it in as your live config.

三个文件 / Three files:

| 文件 / File | 作用 / Purpose |
|---|---|
| `ZeroARKTrait-prices.ini` | 词条基础价 / Base price of each trait |
| `ZeroARKTrait-species.ini` | 物种价值系数 / Species value coefficient |
| `ZeroARKTrait-settings.ini` | 商店设置 / Shop settings |

**实付价公式 / Final price formula:**

```text
实付 = 基础价 × 等级系数 × 物种价值系数
Final = BasePrice × TierCoef × SpeciesCoef
```

（面板会在本地算给玩家看；**机器人/插件扣款时会再算一次**，以服务端为准 / the panel shows a local estimate; the server recomputes it when charging, and the server value is authoritative.）

---

## 1. `ZeroARKTrait-prices.ini` — 词条基础价 / Trait base prices

**格式 / Format:**

```text
词条中文名=基础价
TraitOfficialChineseName=BasePrice
```

- 键必须是**游戏官方中文名**（51 条）/ Keys must be the **official in-game Chinese names** (51 entries)。
- 值是不含任何系数的**基础价** / The value is the **base price** before any coefficient。
- 想改价就改数字；想删条目就整行删 / Edit the number to change a price; delete the whole line to remove an entry。
- 表里**共 51 条**；全部官方中文名可在游戏里用 `Trait 词条名` 列出（管理命令）/ **51 entries ship with the plugin**; list every official Chinese name in game with `Trait 词条名` (admin command)。
- **没定价的词条不能买**：面板与聊天下单都会被拒，玩家看到 `trait_no_price` 那条文案 / A trait **without a price cannot be purchased** (players see the `trait_no_price` message)。

**示例 / Example:**

```text
迟钝=200
干劲十足=200
嗜血=800
```

---

## 2. `ZeroARKTrait-species.ini` — 物种价值系数 / Species value coefficients

**格式 / Format:**

```text
物种名=系数
SpeciesName=Coefficient
```

- **官方英文名与官方中文名各写一行**（这样中文客户端和英文客户端都能匹配上）/ Write **both the official English and the official Chinese name** (so both language clients match)。
- `default` 是兜底系数（表里找不到的物种用它）/ `default` is the fallback coefficient for unlisted species。
- 数值越大越贵 / Higher = more expensive。
- **本表共 261 行** = 130 个物种 ×（英文名 + 中文名）两行，再加一行 `default` / **The shipped table has 261 rows** = 130 species × two lines (official English + official Chinese) plus one `default`。
- 系数只用**五档**：`0.15`（家禽/低价值）· `0.5` · `1.0`（基准）· `1.5` · `2.0`（顶级），**上限 2 倍**；档位按 **PvE** 口径定 / Coefficients use **five steps**: `0.15` (low value) · `0.5` · `1.0` (baseline) · `1.5` · `2.0` (top tier), **capped at 2×**; tiers are picked with **PvE** in mind。

**示例 / Example:**

```text
default=1
Dodo=0.15
渡渡鸟=0.15
Ankylosaurus=1
甲龙=1
```

**注意 / Note:** 中文名请用**游戏官方中文**（来自官方 localization，别用社区俗称）/ Chinese names must come from the **official in-game localization**, not community nicknames。

---

## 3. `ZeroARKTrait-settings.ini` — 商店设置 / Shop settings

| 键 / Key | 默认 / Default | 中文说明 | English |
|---|---|---|---|
| `tier1` | `1` | 1 级系数（玩家看到的「1 级」）| Coefficient for level 1 as shown to players |
| `tier2` | `1.5` | 2 级系数 | Coefficient for level 2 |
| `tier3` | `2` | 3 级系数 | Coefficient for level 3 |
| `points_mode` | `push` | 点数来源：`push` = 由外部（机器人/RCON）推给插件；`db` = 插件自己读 MySQL；**`file` = 插件目录里的本地点数文件**（零依赖 ）| Where points come from: `push` = pushed in from outside (bot / RCON); `db` = the plugin reads MySQL itself; **`file` = a local points file in the plugin folder** (zero dependencies) |
| `charge_mode` | `bot` | **谁负责扣点**：**`plugin` = 插件自己扣**（不用机器人 ）；`bot` = 老方式（插件入队、机器人扣费发货）；`none` = 不扣（测试用）| **Who charges the points**: **`plugin` = the plugin charges by itself** (no bot needed); `bot` = the classic flow (plugin queues, bot charges and delivers); `none` = no charge (testing) |
| `points_cmd` | 空 | **保留字段，暂未使用**（未来的 cmd 模式命令）| **Reserved, not used yet** |
| `points_cmd_seconds` | `30` | **保留字段，暂未使用**（最小 5）| **Reserved, not used yet** (min 5) |
| `push_seconds` | `5` | 每隔几秒把商店数据推给客户端 mod（越小越跟手；测试服写 2）| How often (seconds) shop data is pushed to the client mod (smaller = snappier; the test server uses 2) |
| `ui_key` | `F2` | **打开面板的按键**（UE 按键名，如 `F2` / `F3` / `F4`）。需要客户端 mod **第 11 版起**才会读取它 | **Hotkey that opens the panel** (UE key name, e.g. `F2` / `F3` / `F4`). Read by the client mod **from v11 onwards** |
| `discount_mode` | `highest` | **玩家在多个权限组时怎么算折扣**：`highest` = 取最大的那个（推荐）；`stack` = 多组叠加求和（上限 95%，防 0 元）| **How to combine discounts** when a player is in several groups: `highest` = take the biggest (recommended); `stack` = sum them (capped at 95%) |
| `perm_admin` | `ZeroARKTrait.admin` | **管理权限串**（在 Permissions 插件里给某个组授权这个名字，就有管理权）| **Admin permission string** (grant this name to a group in the Permissions plugin) |
| `perm_use` | `1` | 是否用官方 Permissions 判权限；填 `0` = 回退到插件自带的闸门 | Use the official Permissions plugin for checks; `0` = fall back to the built-in gate |
| `msg_lang` | `zh` | **给玩家看的消息用哪份语言文件**（`zh` = 中文 / `en` = English / 也可自己加语言码）| **Which language file is used for player messages** (`zh` / `en` / or your own language code) |
| `trait_lang` | `zh` | **面板里词条名显示中文还是英文**（`zh` / `en`）—— 只影响显示 协议与审计永远用内部英文名 | **Whether trait names in the panel are shown in Chinese or English** (`zh` / `en`) — display only; the protocol and audit log always use the internal English name |
| `ui_enabled` | `1` | **面板总开关**：填 `0` ⇒ **不自动给玩家挂面板**（聊天菜单仍可用）| **Panel master switch**: `0` ⇒ the panel buff is never auto-attached (the chat menu still works) |
| `order_enabled` | `1` | **订单总开关**：填 `0` ⇒ **拒绝 `Trait 订单`**（发货链路整条停用，查询/加删/查折扣不受影响）| **Order master switch**: `0` ⇒ `Trait 订单` is rejected (the whole delivery path is disabled; queries, add/remove and the discount check still work) |
| `join_tip` | `1` | 玩家进服 / 重连时自动发一条提醒（`0` = 关）| Send one tip when a player joins / reconnects (`0` = off) |
| `join_tip_delay` | `8` | 提醒延迟几秒再发（等玩家加载完；钳 0~120）| Delay in seconds before the tip is sent (clamped 0~120) |
| `db_host` | — | 数据库地址（`points_mode=db` 时用）| MySQL host (used when `points_mode=db`) |
| `db_port` | `3306` | 端口 | Port |
| `db_user` | — | 用户名 | User |
| `db_pass` | — | 密码 | Password |
| `db_name` | `asa` | 库名（ASE 服是 `ase`）| Database name (`ase` for ASE servers) |
| `db_table` | `arkshopplayers` | 表名（**与 ArkShop 同构**）| Table name (**same layout as ArkShop**) |
| `db_key` | `EosId` | 玩家键列 | Column holding the player key |
| `db_val` | `Points` | 点数列 | Column holding the points |
| `db_refresh_seconds` | `20` | 点数缓存时间（避免频繁连库）| Points cache TTL (avoids hammering the DB) |
| `auto_create` | `1` | 查不到库/表时**自动建库建表**（没有 ArkShop 的服也能用）| Auto-create the database/table when missing (works without ArkShop) |
| `auto_insert` | `1` | 查不到这个玩家时**自动建档（0 点）**| Auto-insert a 0-point row for an unknown player |

### 3.3 取值与边界 / Types & limits

| 键 / Key | 类型 / Type | 取值与边界 / Values & limits |
|---|---|---|
| `tier1` `tier2` `tier3` | 小数 | 必须 > 0；填 0 或负数会被**强制回默认** `1 / 1.5 / 2` / Must be > 0; 0 or negative falls back to the defaults `1 / 1.5 / 2` |
| `points_mode` | 字符串 | `push` / `db` / `file` 三选一；写别的值 = 等外部推（`push`）/ One of `push` / `db` / `file`; any other value behaves as `push` |
| `charge_mode` | 字符串 | `plugin` = 插件自己扣（推荐）/ `bot` = 机器人扣（老方式）/ `none` = 不扣（测试）/ `plugin` charges here, `bot` = legacy bot-side, `none` = no charge |
| `points_cmd` `points_cmd_seconds` | 字符串 / 整数 | **保留字段，当前无效**；`points_cmd_seconds` 最小 5（默认 30）/ Reserved; the seconds key has a **minimum of 5** (default 30) |
| `push_seconds` | 整数 | **最小 1**（默认 5）；越小面板数字越跟手 / **Minimum 1** (default 5); smaller = snappier panel |
| `ui_key` | 字符串 | UE 按键名，**只有 `F1`~`F12` 可用**（其它键被游戏占用收不到）；需客户端 mod v11 起 / UE key name, **`F1`–`F12` only**; needs client mod v11+ |
| `discount_mode` | 字符串 | `highest`（取最大，默认）/ `stack`（叠加，**上限 95%**）/ `highest` (default) or `stack` (**capped at 95%**) |
| `perm_admin` | 字符串 | 权限串名字，默认 `ZeroARKTrait.admin` / Permission string, default `ZeroARKTrait.admin` |
| `perm_use` `ui_enabled` `order_enabled` `auto_create` `auto_insert` | 布尔 | 关 = `0` / `false` / `no`（其余都算开）/ Off = `0`, `false` or `no` (anything else is on) |
| `join_tip` | 布尔 | 关 = `0` / `false`（**这个键不认 `no`**，写 `no` 等于开）/ Off = `0` or `false` (**`no` is not recognised here**) |
| `join_tip_delay` | 整数 | **钳到 0~120 秒**（默认 8）/ **Clamped to 0–120 s** (default 8) |
| `msg_lang` `trait_lang` | 字符串 | `zh` / `en` / 自加的语言码；`trait_lang=auto` 跟随 `msg_lang`；**启动时读一次 ⇒ 改完要重启** / Language codes; `auto` follows `msg_lang`; **read once at startup — restart to apply** |
| `db_host` `db_user` `db_pass` `db_name` `db_table` `db_key` `db_val` | 字符串 | `points_mode=db` 时才用；默认表结构与 ArkShop 一致（`arkshopplayers` / `EosId` / `Points`）/ Only used with `points_mode=db`; same layout as ArkShop |
| `db_port` | 整数 | 默认 `3306` / Default `3306` |
| `db_refresh_seconds` | 整数 | 默认 `20`（同一玩家的点数缓存秒数）/ Default `20` (per-player points cache) |
| `near_radius` `id_search_radius` `list_radius` | 小数 | 游戏单位；越大越耗（已有分片预算保护）/ Game units; bigger costs more (the scan budget protects you) |
| `scan_budget_per_tick` | 整数 | **最小 100**（默认 2000）—— 防卡服的核心参数 / **Minimum 100** (default 2000) — the key anti-lag knob |
| `chat_query` `buy_request` | 布尔 | 关 = `0` / `false` / Off = `0` or `false` |
| `chat_cooldown_ms` | 整数 | 毫秒，默认 `5000` / Milliseconds, default `5000` |
| `list_max` | 整数 | 默认 `15`（`trait 龙` 最多列几只）/ Default `15` (max dinos listed) |

### 3.2 不想用数据库？用**文件模式**（推荐给没有 ArkShop 的服）/ No database? Use **file mode**

```text
points_mode=file
charge_mode=plugin
```

- `points_mode=file` ⇒ 点数存在**插件目录**的 **`ZeroARKTrait-points.txt`**（一行 `EOS=点数` **零第三方依赖** 不需要 MySQL，也不需要 ArkShop）。
- `charge_mode=plugin` ⇒ **玩家下单时插件自己扣点**：`db` 模式走条件 UPDATE（`… WHERE 点数 >= 价` **不可能扣成负数**），`file` 模式直接改写这个文件 ；扣成功才发货（**按恐龙 ID** ），失败会把原因推给玩家 。
- 管理命令在 file 模式下**同样可用** ：`Trait 查点数 <EOS>` / `Trait 设点数 <EOS> <点数>` / `Trait 加点 <EOS> <增量>` / `Trait 删点数 <EOS>` ；`Trait 商店状态` 会显示 `mode=file charge=… file_points=N` 。
- `charge_mode` 的其它取值：`bot` = 老方式（插件入队 → 机器人扣费发货）；`none` = 不扣（纯测试）。

**文件长这样 / The file looks like:**

```text
<玩家EOS 32位hex>=800
```

### 3.1 想用数据库模式？把这个块**复制**进 `-settings.ini` / Want DB mode? Copy this block into `-settings.ini`

> 因为配置文件里不放注释，这段作为**可复制的模板**放在这里 / Config files carry no comments, so this block lives here as a copy-paste template。

```text
points_mode=db
db_host=127.0.0.1
db_port=3306
db_user=your_user
db_pass=your_password
db_name=asa
db_table=arkshopplayers
db_key=EosId
db_val=Points
db_refresh_seconds=20
auto_create=1
auto_insert=1
```

**表结构（与 ArkShop 一致）/ Table layout (identical to ArkShop):**

```sql
CREATE TABLE IF NOT EXISTS arkshopplayers (
 EosId VARCHAR(64) PRIMARY KEY,
 Points INT NOT NULL DEFAULT 0
);
```

---

## 4. 管理命令速查 / Admin command cheat sheet

命令**中英双语（中文 / 拼音 / 英文）都能敲** / Commands accept **Chinese, pinyin and English**:

| 中文 | 拼音 / Pinyin | 英文 / English | 作用 / Purpose |
|---|---|---|---|
| `Trait 查点数 <EOS>` | `chadianshu` | `getpoints` | 查某玩家点数 / Query a player's points |
| `Trait 设点数 <EOS> <N>` | `sheDianShu` | `setpoints` | 直接设为 N / Set to N |
| `Trait 加点 <EOS> <N>` | `jiaDian` | `addpoints` | 加 N 点 / Add N |
| `Trait 删点数 <EOS>` | `shanDianShu` | `delpoints` | 删掉这条档案 / Delete the row |
| `Trait 商店状态` | `shangdianzhuangtai` | `dbstatus` | 看当前模式/表/条数 / Show mode, tables and counts |
| `Trait 查折扣 <EOS>` | `chaZheKou` | `discount` | 查某玩家的**权限组 / 折扣 / 折扣表条数 / 嗜血 1 级实价**（排查折扣为什么没生效就用它）/ Show a player's groups, discount, table size and a sample price — the fastest way to debug discounts |
| `Trait 缓存价目 <flag> <文本>` | — | — | 分块推送价目给面板 / Push the price table to the panel in chunks |
| `Trait 缓存点数 <EOS 或 角色名> <N>` | — | — | 推送某玩家点数（优先按 EOS）/ Push a player's points (EOS preferred) |

**注意 / Notes:**

- 这些是 **RCON / 服务器控制台**命令，不开放给普通玩家 / These are **RCON / server-console** commands, not for regular players。
- 连续快速发 RCON 命令建议**每条间隔 3~4 秒** / Leave **3–4 seconds** between RCON commands。
- 重启服务器后，插件内存里的价目表是空的 ⇒ **必须重新推一次价目**（否则连"自动挂 buff"都不会执行，玩家按 `ui_key` 没有反应）/ After a server restart the in-memory price table is empty ⇒ **push the price table again** (otherwise even the auto-attach of the shop buff is skipped and the hotkey does nothing)。

---

## 5. 常见问题 / FAQ

| 现象 / Symptom | 原因与处理 / Cause & fix |
|---|---|
| 玩家按 `ui_key` 没反应 / Pressing `ui_key` does nothing | ① 服务器重启后没重推价目 ⇒ 推一次；② 客户端没装 `ZeroTraitUI` mod；③ 玩家不在线（面板要玩家在线才创建）/ ① price table was not re-pushed after a restart; ② the client has no `ZeroTraitUI` mod; ③ the player is offline (the panel is created per online player) |
| 面板出来但右栏空 / Panel opens but the right column is empty | 价目没推或推送失败 / the price table was never pushed, or the push failed |
| 点数显示不对 / Wrong points | `points_mode` 与数据来源不匹配（`push` 模式下点数由外部推）/ `points_mode` doesn't match the data source (in `push` mode points come from outside) |
| 改了 `ui_key` 没生效 / Changed `ui_key` with no effect | 需要**客户端 mod 第 11 版起**；旧版 mod 写死 `F2` / Requires **client mod v11 or newer**; older mods hard-code `F2` |

---

## 6. 另外两个配置文件 / Two more config files

> 同样遵守"**零注释**"规则 / These follow the same **no-comments** rule。

### 6.1 `trait_names.ini` — 词条别名表 / Trait alias table

**格式 / Format:**

```text
别名=英文内部名
Alias=InternalEnglishName
```

- 用途：让玩家可以输入**旧简称/别名**（例如 `吸血`、`屠巨`）/ Lets players type **legacy aliases** (e.g. `吸血`, `屠巨`)。
- **显示名以内置的官方中文表为准**；本文件只在内置表里没有该英文名时才用于显示 / **Display names come from the built-in official table**; this file only feeds the display when the built-in table lacks that English name。
- 当前 45 条 / Currently 45 entries。

**示例 / Example:**

```text
吸血=Vampiric
屠巨=Giantslaying
```

### 6.2 `zerotrait.ini` — 运行参数 / Runtime options

| 键 / Key | 默认 / Default | 中文说明 | English |
|---|---|---|---|
| `near_radius` | `5000` | 按玩家名找"最近恐龙"的半径（游戏单位；5000 ≈ 半个屏幕视野）| Radius (game units) used when finding the nearest dino by player name; 5000 ≈ half a screen |
| `id_search_radius` | `15000` | 按 DinoID 搜索的半径（游戏单位）| Search radius (game units) when searching by DinoID |
| `scan_budget_per_tick` | `2000` | 每个 Tick 最多扫描多少个候选（防卡服）| Max candidates scanned per tick (keeps the server smooth) |
| `chat_query` | `true` | 是否允许**聊天栏**查询指令 / Allow query commands from **chat** |
| `chat_cooldown_ms` | `5000` | 聊天查询冷却（毫秒）| Chat query cooldown (ms) |
| `buy_request` | `1` | 是否开放**玩家游戏内下单**（`trait 买`）| Allow **in-game purchase requests** (`trait 买`) |
| `list_radius` | `8000` | `trait 龙` 列龙的半径（游戏单位）| Radius (game units) for the `trait 龙` dino list |
| `list_max` | `15` | `trait 龙` 最多列几只 | Max dinos listed by `trait 龙` |

---

## 7. `ZeroARKTrait-discounts.ini` — 权限组折扣 / Permission-group discounts

**格式 / Format:**

```text
权限组名=折扣百分比
PermissionGroupName=DiscountPercent
```

- **组名 = Permissions 插件里的组名**（例如 `Default` / `Admins` / `VIP1` / `SVIP` / `SSVIP` ）。
- **数值 = 百分比**：`10` = 打九折（付 90%）；`20` = 打八折 ；`0` 或不写 = 不打折 。
- **玩家在多个组**时怎么算 ⇒ 由 `-settings.ini` 的 **`discount_mode`** 决定：`highest`（取最大 默认）/ `stack`（叠加求和 上限 95%）。
- **折扣同时作用于两处** ：① **面板显示的价**（插件按玩家推送"折后价" 不需改客户端 mod ）；② **实际扣费**（用**同一个公式** ：`折后价 = floor(价 × (1 - 折扣/100) + 0.5)` ）。
- 表里找不到该组 ⇒ 该组折扣为 0 。
- 随插件发出的表**已填了 14 行示例组**：`Default` / `Discord` / `Test` / `Admins` / `VIP1`~`VIP8` / `SVIP` / `SSVIP`（0~50%）；用不到的整行删，要加组就照抄一行改名 / The shipped file **already lists 14 groups**: `Default` / `Discord` / `Test` / `Admins` / `VIP1`–`VIP8` / `SVIP` / `SSVIP` (0–50%); delete the rows you don't use, copy a row to add your own。

**示例 / Example:**

```text
Default=0
Admins=0
VIP1=3
VIP8=35
SSVIP=50
```

### 7.1 官方 Permissions 插件 / The official Permissions plugin

- 插件会**动态加载** `ArkApi\Plugins\Permissions\Permissions.dll` —— **没装也能正常启动** （日志会写 `Permissions plugin NOT found` ，权限判断回退到插件自带闸门 ）。
- 装好后启动日志会打印：`Permissions plugin loaded: IsPlayerHasPermission=true GetPlayerGroups=true` 。
- 用到的接口（从该 DLL 的导出符号取 没有头文件）：`Permissions::IsPlayerHasPermission(玩家EOS, 权限串) → bool` 与 `Permissions::GetPlayerGroups(玩家EOS) → TArray<FString>` 。
- **权限组怎么建**：用 Permissions 插件自己的命令/配置（它的组与玩家归属存在它自己的库里 ）；本插件**只读**、不改它的数据 。

---

## 8. 玩家消息 / Player messages（可配置 + 中英双语 / configurable, bilingual）

**两份文件 / Two files:**

| 文件 / File | 内容 / Content |
|---|---|
| `ZeroARKTrait-messages-zh.ini` | 中文消息全集（**64 条** ）|
| `ZeroARKTrait-messages-en.ini` | English message set (same **64** keys ) |

- `-settings.ini` 的 **`msg_lang`** 决定读哪一份（默认 `zh` ；填 `en` 就全英文 ）。
- 想加别的语言 ⇒ **复制一份改名** 成 `ZeroARKTrait-messages-<语言码>.ini` 即可 （**键名不要改** ）。
- **格式** `键=文案`（零注释 无 BOM ）：`{0}`~`{3}` 是占位符 ；`\n` 会变成真换行 ；颜色用 ARK 富文本 `<RichColor Color="0.6,1,0.6,1">文字</>`（RGB 取 0~1 第四个是透明度 ）。
- 某条键缺失 ⇒ 日志出现 `msg key missing: xxx` 、玩家看到 `[msg:xxx]`（一眼能看出漏配 ）。

**键表 / Key list**（共 64 个 / 64 keys）

| 键 / Key | 用途 / Usage | 占位符 / Placeholders |
|---|---|---|
| `no_pawn` | 玩家没角色（死亡/未上线）| — |
| `no_inventory` | 拿不到背包组件 | — |
| `no_gamedata` | 拿不到词条定义 | — |
| `spawn_done` | 刷完野生龙 | `{0}`数量 |
| `add_ok` | 加词条成功 | `{0}`龙 `{1}`词条 `{2}`级别 `{3}`订单后缀 |
| `add_fail` | 加词条失败 | `{0}`原因 `{1}`订单后缀 |
| `del_ok` | 删词条成功 | `{0}`龙 `{1}`词条 |
| `del_fail` | 删词条失败 | `{0}`原因 |
| `bundle_done` | 礼包处理完 | `{0}`成功数 `{1}`总数 `{2}`目标 |
| `no_dino_near` | 附近没龙 | `{0}`搜索半径 |
| `no_player_name` | 读不到玩家名 | — |
| `order_query_tip` | 查订单用法提示 | — |
| `sel_usage` | 选龙用法提示 | — |
| `sel_cancel` | 已取消选定 | — |
| `sel_ok` | 选定成功（简版）| `{0}`龙名 |
| `sel_bad_index` | 序号不对 | — |
| `sel_ok_detail` | 选定成功（带详情）| `{0}`龙名 `{1}`物种 `{2}`等级 `{3}`DinoID |
| `buy_disabled` | 游戏内下单未开启 | — |
| `buy_usage` | 下单用法提示 | — |
| `buy_bad_tier` | 级别不合法 | — |
| `buy_fail` | 提交失败 | `{0}`原因 |
| `buy_ok` | 提交成功 | `{0}`词条 `{1}`级别 `{2}`按哪只算价 |
| `buy_no_points` | **点数不足**（插件扣点模式下）| `{0}`需要多少点 `{1}`具体原因 |
| `trait_no_price` | 这个词条还没定价 | `{0}`词条名 |
| `menu` | 帮助菜单（`trait 帮助` 等）| — |
| `notify_offline` | **不在线时排队的失败通知**（上线后自动推）| `{0}`订单号 `{1}`词条 |
| `search_miss` | 按名字/DinoID 找不到恐龙 | `{0}`目标 `{1}`已扫只数 `{2}`耗时秒 `{3}`半径 |
| `no_player_found` | 找不到该玩家 | `{0}`玩家名 |
| `need_online_player` | 该操作需要玩家在线 | — |
| `no_cheatmanager` | 拿不到 CheatManager | — |
| `cheat_on` | 已尝试开启作弊 | — |
| `shop_name` | **商店名（面板标题）** —— 插件随数据推给面板（`\|\|TITLE\|`）| — |
| `ui_hint` | 面板底部那行操作提示 | — |
| `Text_LeftTitle` | 面板左栏标题（我的龙） | — |
| `Text_RightTitle` | 面板右栏标题（词条） | — |
| `Text_T1` | 面板「1 级」按钮文案 | — |
| `Text_T2` | 面板「2 级」按钮文案 | — |
| `Text_T3` | 面板「3 级」按钮文案 | — |
| `Text_Clear` | 面板「清空」按钮文案 | — |
| `Text_Confirm` | 面板「确认」按钮文案 | — |
| `Text_Close` | 面板「关闭」按钮文案 | — |
| `Text_Selection` | 还没选龙时底部显示的占位文案 | — |
| `Text_SearchDinoHint` | 左栏搜索框提示文字 | — |
| `Text_SearchTraitHint` | 右栏搜索框提示文字 | — |
| `wheel_entry` | 轮盘（长按 E 菜单）里的条目名 | — |
| `join_tip` | **进服/重连提醒**（`{0}` 自动填面板按键） | `{0}`按键 |
| `pre_no_dino` | 预检拒绝：面板上选的那只龙找不到（低温舱/换图） | — |
| `pre_stack_full` | 预检拒绝：这条词条已到堆叠上限 | `{0}`词条 |
| `pre_trait_full` | 预检拒绝：这只龙词条数量已满 | `{0}`现有 `{1}`上限 |
| `pre_engine_no_reason` | 预检拒绝：引擎不给原因地拒绝 | `{0}`现有 `{1}`上限 |
| `pre_engine_reason` | 预检拒绝：引擎给出的原因 | `{0}`原因 |
| `pre_pending` | 预检拒绝：同一只龙同一条词条的上一单还没发货 | — |
| `pre_id_missing` | 预检拒绝：面板没把龙 ID 带上来 | `{0}`收到的内容 |
| `pre_dino_gone` | 预检拒绝：按 DinoID 找不到那只龙 | `{0}`id1 `{1}`id2 |
| `charge_no_points` | 扣费失败：点数不足 | — |
| `charge_no_eos` | 扣费失败：读不到玩家 EOS | — |
| `charge_db_down` | 扣费失败：数据库连不上 | — |
| `charge_db_query` | 扣费失败：查点数失败 | — |
| `charge_fail` | 扣费失败：扣点写回失败 | — |
| `Fmt_Points` | 面板点数栏模板 | `{0}`点数 |
| `Fmt_Lvl` | 龙行等级模板 | `{0}`等级 |
| `Fmt_Sel` | **面板底部「已选…」模板**（总价显示在 `{3}`） | `{0}`龙 `{1}`词条 `{2}`等级 `{3}`总价 |
| `Fmt_Price` | 词条行价格模板（面板里该控件默认隐藏） | `{0}`价格 |
| `sel_line` | 聊天栏里的「已选…」文本（与 `Fmt_Sel` 同格式） | `{0}`…`{3}` |

> **离开游戏也会收到结果**：加/删词条、礼包、按 ID 搜索这些动作，**玩家在线时立刻推给他** ；**不在线时会把"结果 + 失败原因"排进待发队列** ，**他一上线（每 2 秒检查一次）立刻推** ，队列上限 200 条、超过 24 小时未送达自动丢弃 。相关提示用 `notify_offline` 。

---

## 9. 兼容性与注意事项 / Compatibility & caveats

- **改完配置要重启服务器**：本插件**没有热重载**，配置只在启动时读一次 / **Restart the server after editing**: there is **no hot reload**, the config is read once at startup
- **换版本**：停服 → 覆盖 `ZeroARKTrait.dll` → 启动；**不要用 `plugins.unload` / `load`**（实测会崩服）/ **Upgrading**: stop, overwrite the DLL, start. **Do not use `plugins.unload` / `load`** (it crashes the server in testing)
- **中文命令要搭配 `UnicodeRCONASA`**：RCON 通道只吃 ASCII，中文命令靠它转 / **Chinese commands need `UnicodeRCONASA`** (the RCON channel is ASCII-only)
- **点数三种模式**：`file` 零依赖（不需要 MySQL / ArkShop）；`db` 要填全 `db_*`，表结构与 ArkShop 相同（`arkshopplayers` / `EosId` / `Points`），可 `auto_create` 自建；`push` 要外部（机器人 / RCON）推 / **Three points modes**: `file` (no dependencies), `db` (fill in `db_*`; ArkShop layout, can auto-create), `push` (external push needed)
- **数据库账号认证**：插件用自写的最小 MySQL 客户端，建议账号用 `mysql_native_password`；MySQL 8 默认的 `caching_sha2_password` 只支持不加密的快认证路径，可能连不上 / **DB auth**: prefer `mysql_native_password`; MySQL 8's default `caching_sha2_password` may fail (only the fast-auth path is implemented)
- **面板**：需要客户端 mod `ZeroTraitUI`（CurseForge `1704776`）；`ui_key` 只能用 `F1`~`F12`；改完要重启服、玩家重进才生效 / **Panel**: needs the client mod `ZeroTraitUI` (CurseForge `1704776`); `ui_key` must be `F1`–`F12`; restart and let players rejoin
- **没装官方 Permissions 也能跑**：权限判断回退到插件自带闸门（启动日志会写 `Permissions plugin NOT found`）/ **Works without the official Permissions plugin** (falls back to the built-in gate; the log says `Permissions plugin NOT found`)
- **中文名必须是官方 localization**（价格表 / 物种表 / 别名表都一样），社区俗称匹配不上 / **Chinese names must come from the official in-game localization** (prices, species and aliases alike)
- **物种系数上限 2 倍**：五档 `0.15 / 0.5 / 1.0 / 1.5 / 2.0`，按 PvE 口径定档 / **Species coefficients cap at 2×**: five steps `0.15 / 0.5 / 1.0 / 1.5 / 2.0`, picked for PvE
- **文件位置**：运行记录 `trait_audit.log`；点数（`file` 模式）`ZeroARKTrait-points.txt`；都在插件目录 / **Where things live**: audit log `trait_audit.log`, points file `ZeroARKTrait-points.txt` — both in the plugin folder
- **玩家侧只读**：玩家只能用聊天查询与面板下单，**改词条永远只在管理侧**（RCON / 服务端控制台）/ **Players are read-only**: trait changes happen only on the admin side (RCON / server console)
