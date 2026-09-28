// ============================================================================
//  ZeroARKTrait -- ASA (ARK: Survival Ascended) server plugin
//  Purpose: add / remove / list Gene Traits on ANY dino (no species or age limit),
//           plus list every gene trait it carries.
//
//  Why it works (from the AsaApi SDK headers):
//    - UGeneTraitDefinitions::GeneTraits_AddInstanceOfGeneTraitIfPossible(
//          UObject* TargetToEvaluate, FName TraitName, int TraitTier, FString* FailReason) -> bool
//      also: GeneTraits_RemoveInstanceOfGeneTraitIfItExists
//            GeneTraits_GetCountOfGeneTraitStacksThisCreatureHas
//            GeneTraits_GetMaxAllowedTraitsForThisCreature
//      the instance comes from UPrimalGameData::GeneTraits_Definitions (take its CDO --
//      same pattern the SDK itself uses for MinimapData)
//    - APrimalDinoCharacter::GeneTraitsField() -> TArray<FName>   (read a dino's traits)
//
//  Threading: RCON / console callbacks are NOT guaranteed to run on the game
//  thread, so callbacks only parse + enqueue + ack. All engine calls happen in a
//  1-second game-thread timer (Tick).
//
//  Commands (registered on BOTH tables: console and RCON):
//    TraitAdd  <player> <trait> [tier]        add one trait to the nearest dino
//    TraitDel  <player> <trait>               remove one trait instance
//    TraitList <player>                       print that dino's trait list
//    TraitHelp                                usage
//
//  Install: ARK\ShooterGame\Binaries\Win64\ArkApi\Plugins\ZeroARKTrait\
//             ZeroARKTrait.dll   (dll name must equal folder name)
//             PluginInfo.json
//
//  Build: MSVC x64 / Release, static CRT (/MT), link extern\AsaApi\out_lib\AsaApi.lib
// ============================================================================

#include "API/ARK/Ark.h"
#include "IApiUtils.h"
#include "ICommands.h"
#include "Timer.h"

// 极简 MySQL 客户端（自己读点数用）
//   只依赖 ws2_32 + bcrypt（都是 Windows 系统库）⇒ 不违反"不要第三方依赖"
//   先例：ArkShop.dll 的依赖表里也只有 WS2_32.dll，它就是这么干的
#include "MiniMysql.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // ---------------------------------------------------------------- job queue
    struct Job
    {
        int kind = 0;              // 1=add 2=del 3=list
        std::wstring player;       // platform name or character name（by_id 时不用）
        bool by_id = false;        // true = 目标用 DinoID 指定
        bool by_name = false;      // true = 目标用恐龙名字（子串）匹配
        std::wstring name_sub;     // by_name 时的匹配子串
        unsigned int id1 = 0;
        unsigned int id2 = 0;
        std::wstring trait;        // 已归一化为引擎内部名（英文）
        int tier = 0;
        std::wstring eos;          // 下单玩家的 EOS（发货失败要退费用）
        int cost = 0;              // 这单实际扣了多少点（>0 才需要退）
        std::wstring order;        // 订单号（v1.3：机器人下单专用通道）
        std::vector<std::pair<std::wstring, int>> bundle;   // v1.4：词条礼包（词条,层级）列表
        std::wstring text;         // v1.9.8：随任务带的自由文本（面板文本推送用）
        float radius = 0.f;        // v1.11.0：龙列表搜索半径（0 = 用配置默认 list_radius）
    };

    std::mutex g_mu;
    std::vector<Job> g_jobs;

    // =========================================================================================
    // v1.23.2（B/C）：下单预检用的小状态 + 小工具
    //   pending：同一玩家 + 同一只龙 + 同一条词条 的已扣费但还没发货任务数
    //            （面板「确认」连点两下只差 1 秒 ⇒ 两单都过引擎检查 ⇒ 第二单被拒再退费）
    //   errors ：最近一次下单被拒的原因（下次给该玩家推商店数据时搭 ||ERR| 带回面板）
    //    锁 = g_push_mu（下面声明的那把专用锁；只在这一段里用，不与 g_mu 嵌套 ⇒ 无锁顺序问题）
    static std::map<std::wstring, int> g_pending_charge;
    static std::map<std::wstring, std::wstring> g_pending_error;
    static std::mutex g_push_mu;

    // 下面这几个帮助函数定义在文件后半段 ⇒ 预检要在 EnqueueBuyReq 里调用它们，
    //   必须先在这里前置声明（否则 MSVC 会在调用点直接报"找不到标识符"）
    //    注意 DinoRef 本身也要前置声明（它定义在 1200 行附近，比这里晚）
    struct DinoRef;
    APrimalDinoCharacter* NearestDino(AShooterPlayerController* pc, float radius);
    bool GetSelection(const std::wstring& player, DinoRef& out);
    APrimalDinoCharacter* FindDinoById(unsigned int id1, unsigned int id2);
    void StartSearch(const Job& j);

    //   key = EOS + "|" + 词条内部英文名 + "|" + DinoID1 + "|" + DinoID2
    //      带 0（DinoID 没拿到）时退化成按玩家+词条一门 ⇒ 宁可严一点也不重复扣费
    std::wstring PendingKey(const std::wstring& eos, const std::wstring& trait,
                            unsigned int id1, unsigned int id2)
    {
        return eos + L"|" + trait + L"|" + std::to_wstring(id1) + L"|" + std::to_wstring(id2);
    }

    void DecrementPendingCharge(const Job& j)
    {
        if (j.eos.empty()) return;                       // 没扣过费的单不用算
        const std::wstring pk = PendingKey(j.eos, j.trait, j.id1, j.id2);
        std::lock_guard<std::mutex> lk(g_push_mu);
        std::map<std::wstring, int>::iterator it = g_pending_charge.find(pk);
        if (it == g_pending_charge.end()) return;
        if (--(it->second) <= 0) g_pending_charge.erase(it);
    }

    void SetPendingError(const std::wstring& eos, const std::wstring& why)
    {
        if (eos.empty()) return;
        std::lock_guard<std::mutex> lk(g_push_mu);
        g_pending_error[eos] = why;
    }
    // =========================================================================================


    // 命令名：英文 + 中文别名，两套都注册（中英文都认）
    const std::wstring CMD_ADD  = L"TraitAdd";
    const std::wstring CMD_DEL  = L"TraitDel";
    const std::wstring CMD_LIST = L"TraitList";
    const std::wstring CMD_HELP = L"TraitHelp";

    const std::wstring CMD_ADD_CN  = L"词条加";
    const std::wstring CMD_DEL_CN  = L"词条删";
    const std::wstring CMD_LIST_CN = L"词条列表";
    const std::wstring CMD_HELP_CN = L"词条帮助";

    // ------------------------------------------------------------------ helpers
    std::wstring Trim(const std::wstring& s)
    {
        const size_t b = s.find_first_not_of(L" \t\r\n");
        if (b == std::wstring::npos) return L"";
        const size_t e = s.find_last_not_of(L" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    void Split(const std::wstring& s, std::vector<std::wstring>& out)
    {
        out.clear();
        size_t i = 0;
        while (i < s.size())
        {
            while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) ++i;
            const size_t start = i;
            while (i < s.size() && s[i] != L' ' && s[i] != L'\t') ++i;
            if (i > start) out.push_back(s.substr(start, i - start));
        }
    }

    // drop the leading command word
    std::wstring StripCmd(const std::wstring& line, const std::wstring& cmd)
    {
        const std::wstring t = Trim(line);
        if (t.size() > cmd.size() && _wcsnicmp(t.c_str(), cmd.c_str(), cmd.size()) == 0)
            return t.substr(cmd.size());
        return t;
    }

    // 所有命令名（中英文），用于"剥壳"：行首若是命令名就丢掉它
    const std::wstring* AllCmds(size_t& n)
    {
        static const std::wstring kAll[] = {
            L"Trait", L"trait",
            L"TraitAdd", L"TraitDel", L"TraitList", L"TraitHelp"
        };
        n = sizeof(kAll) / sizeof(kAll[0]);
        return kAll;
    }

    std::wstring StripAnyCmd(const std::wstring& line)
    {
        const std::wstring t = Trim(line);
        const size_t sp = t.find_first_of(L" \t");
        const std::wstring first = (sp == std::wstring::npos) ? t : t.substr(0, sp);

        size_t n = 0;
        const std::wstring* all = AllCmds(n);
        for (size_t i = 0; i < n; ++i)
        {
            if (_wcsicmp(first.c_str(), all[i].c_str()) == 0)
                return (sp == std::wstring::npos) ? L"" : t.substr(sp + 1);
        }
        return t;
    }

    // 还原"UTF-8 字节被按单字节码页解码"造成的乱码：
    //   例：'加'(E5 8A A0) 到达时变成 U+FFE5 U+FF8A U+FFA0（byte -> 0xFF00+byte）
    // 规则：把 0xFF00..0xFFFF 的字符按低 8 位还原成字节，再整体按 UTF-8 解码。
    std::wstring TryFixUtf8Mojibake(const std::wstring& s)
    {
        std::string bytes;
        bytes.reserve(s.size());
        bool any_high = false;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c < 0x80)
            {
                bytes.push_back(static_cast<char>(c));
            }
            else if (c >= 0xFF00 && c <= 0xFFFF)
            {
                bytes.push_back(static_cast<char>(c & 0xFF));
                any_high = true;
            }
            else
            {
                return s;   // 不是这种乱码，原样返回
            }
        }
        if (!any_high) return s;

        const std::wstring out = AsaApi::Tools::Utf8Decode(bytes);
        return out.empty() ? s : out;
    }

    // fmt::format treats { } specially
    std::wstring Esc(const std::wstring& s)
    {
        std::wstring o;
        o.reserve(s.size() + 8);
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c == L'{') o += L"{{";
            else if (c == L'}') o += L"}}";
            else o += c;
        }
        return o;
    }

    // `SendServerMessage` 这条通道不解析富文本 ⇒ 消息里的 `<RichColor …></>`
    //   会原样显示出来
    //   ⇒ 这里把标记剥掉，并把标签里的颜色抠出来交给 SendServerMessage 的 FLinearColor
    std::wstring StripRichColorTag(const std::wstring& in, FLinearColor& col)
    {
        std::wstring out;
        const std::wstring open = L"<RichColor";
        for (size_t i = 0; i < in.size(); )
        {
            if (in.compare(i, open.size(), open) == 0)
            {
                const size_t gt = in.find(L'>', i);
                if (gt == std::wstring::npos) break;
                const std::wstring tag = in.substr(i, gt - i + 1);
                const size_t q1 = tag.find(L'"');
                const size_t q2 = (q1 == std::wstring::npos) ? std::wstring::npos : tag.find(L'"', q1 + 1);
                if (q1 != std::wstring::npos && q2 != std::wstring::npos)
                {
                    const std::wstring cvals = tag.substr(q1 + 1, q2 - q1 - 1);
                    float rgba[4] = { 0.25f, 0.9f, 1.f, 1.f };
                    int n = 0;
                    size_t p = 0;
                    while (n < 4)
                    {
                        const size_t c = cvals.find(L',', p);
                        const std::wstring one = (c == std::wstring::npos) ? cvals.substr(p) : cvals.substr(p, c - p);
                        if (!one.empty()) rgba[n++] = (float)_wtof(one.c_str());
                        if (c == std::wstring::npos) break;
                        p = c + 1;
                    }
                    if (n >= 3) col = FLinearColor(rgba[0], rgba[1], rgba[2], (n >= 4) ? rgba[3] : 1.f);
                }
                i = gt + 1;
                continue;
            }
            if (in.compare(i, 3, L"</>") == 0) { i += 3; continue; }
            if (in.compare(i, 12, L"</RichColor>") == 0) { i += 12; continue; }
            out += in[i++];
        }
        return out;
    }

    void Say(AShooterPlayerController* pc, const std::wstring& text)
    {
        Log::GetLog()->info("[ZeroARKTrait] {}", AsaApi::Tools::Utf8Encode(text));
        if (pc != nullptr)
        {
            FLinearColor col(0.25f, 0.9f, 1.f, 1.f);
            const std::wstring plain = StripRichColorTag(text, col);   // 剥标记 + 取色
            const std::wstring e = Esc(plain);
            AsaApi::GetApiUtils().SendServerMessage(pc, col, e.c_str());
        }
    }

    // ------------------------------------------------------------------ lookups
    AShooterPlayerController* FindPlayer(const std::wstring& name)
    {
        if (name.empty()) return nullptr;
        FString f(name.c_str());
        AShooterPlayerController* pc = AsaApi::GetApiUtils().FindPlayerFromPlatformName(f);
        if (pc != nullptr) return pc;
        TArray<AShooterPlayerController*> arr =
            AsaApi::GetApiUtils().FindPlayerFromCharacterName(f, ESearchCase::Type::IgnoreCase, false);
        if (arr.Num() > 0) return arr[0];
        return nullptr;
    }

    // 待发通知 —— 玩家不在线时先把"结果+原因"存着，等他上线立刻推给他。
    //   用户要求：加词条的成功/失败 + 失败原因必须让玩家收到 （在线直接推、离线排队）
    struct NotifyItem { std::wstring player; std::wstring text; unsigned long long ts; };
    static std::vector<NotifyItem> g_notify;
    static unsigned long long g_notify_check = 0;

    void NotifyLater(const std::wstring& player, const std::wstring& text)
    {
        if (player.empty() || text.empty()) return;
        if (g_notify.size() >= 200) g_notify.erase(g_notify.begin());   // 上限 200 条，防爆
        NotifyItem it; it.player = player; it.text = text; it.ts = GetTickCount64();
        g_notify.push_back(it);
        Log::GetLog()->info("[ZeroARKTrait] notify queued for {}: {}",
                            AsaApi::Tools::Utf8Encode(player), AsaApi::Tools::Utf8Encode(text));
    }

    // 每 2 秒扫一次：人上线了就推；超过 24 小时还没送到就丢
    void PumpNotifications()
    {
        const unsigned long long now = GetTickCount64();
        if (g_notify_check != 0 && now - g_notify_check < 2000ULL) return;
        g_notify_check = now;
        for (size_t i = 0; i < g_notify.size(); )
        {
            if (now - g_notify[i].ts > 24ULL * 3600ULL * 1000ULL)
            {
                g_notify.erase(g_notify.begin() + i);
                continue;
            }
            AShooterPlayerController* pc = FindPlayer(g_notify[i].player);
            if (pc == nullptr) { ++i; continue; }
            Say(pc, g_notify[i].text);
            g_notify.erase(g_notify.begin() + i);
        }
    }

    APrimalDinoCharacter* NearestDino(AShooterPlayerController* pc, float radius)
    {
        if (pc == nullptr) return nullptr;
        const FVector loc = pc->CurrentPlayerCharacterLocationField();
        TArray<AActor*> actors = AsaApi::GetApiUtils().GetAllActorsInRange(
            loc, radius, EServerOctreeGroup::Type::DINOPAWNS);

        APrimalDinoCharacter* best = nullptr;
        double best_d2 = 1e30;
        for (int i = 0; i < actors.Num(); ++i)
        {
            AActor* a = actors[i];
            if (a == nullptr) continue;
            USceneComponent* root = a->RootComponentField();
            if (root == nullptr) continue;
            //  2026-09-22：同上，改用 ComponentToWorld 的世界位置（原来是相对坐标）
            const FVector p = root->ComponentToWorldField().GetLocation();
            const double dx = p.X - loc.X;
            const double dy = p.Y - loc.Y;
            const double dz = p.Z - loc.Z;
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2)
            {
                best_d2 = d2;
                best = static_cast<APrimalDinoCharacter*>(a);
            }
        }
        return best;
    }

    UGeneTraitDefinitions* TraitDefs()
    {
        UPrimalGameData* gd = AsaApi::GetApiUtils().GetGameData();
        if (gd == nullptr) return nullptr;
        UClass* cls = gd->GeneTraits_DefinitionsField().uClass;
        if (cls == nullptr) return nullptr;
        return static_cast<UGeneTraitDefinitions*>(cls->GetDefaultObject(true));
    }

    std::wstring TraitNameOf(FName& n)
    {
        FString s = n.ToString();
        return std::wstring(*s);
    }

    void ParseTraitToken(const std::wstring& tok, std::wstring& name, int& tier)
    {
        name = tok;
        tier = 0;
        const size_t lb = tok.find(L'[');
        const size_t rb = tok.find(L']');
        if (lb != std::wstring::npos && rb != std::wstring::npos && rb > lb + 1)
        {
            name = tok.substr(0, lb);
            tier = _wtoi(tok.substr(lb + 1, rb - lb - 1).c_str());
        }
    }

    // ---------------------------------------------------- 配置（zerotrait.ini）
    // 性能纪律：搜索半径越大、龙越多，越不能"一个 Tick 扫完"。
    //   所以按 DinoID / 名字搜索走 scan_budget_per_tick 的**分片推进**，每 Tick 有硬上限。
    struct Config
    {
        float near_radius = 5000.f;        // 按玩家找"最近的龙"的半径
        float id_search_radius = 15000.f;  // 按 DinoID / 名字搜索的半径（每名在线玩家周围）
        int   scan_budget = 2000;          // 每个 Tick 最多检查多少个 actor
        bool  chat_query = true;           // 是否开放玩家只读查询
        int   chat_cooldown_ms = 5000;     // 玩家查询冷却
        bool  buy_request = false;         // v1.10.0 是否允许玩家在游戏里"下单"（入队等机器人拉取）
        float list_radius = 8000.f;        // v1.11.0 `trait 龙` 的搜索半径（cm ⇒ 80 米）
        int   list_max = 15;               // v1.11.0 `trait 龙` 最多列出几只
    };
    Config g_cfg;

    // 搜索状态（跨 Tick 推进；避免一次性枚举几千只龙卡住主线程）
    struct SearchState
    {
        bool active = false;
        Job  job;
        bool by_name = false;
        std::wstring name;
        std::vector<FVector> locs;
        int player_idx = 0;
        TArray<AActor*> actors;
        int actor_cursor = 0;
        int scanned = 0;
        unsigned long long started = 0;      // v1.3：用于进度回执与耗时统计
        unsigned long long last_report = 0;
    };
    SearchState g_search;

    // ---------------------------------------------------- 审计日志（v1.3）
    // 按订单号在审计文件里回查（v1.4：机器人不用去读文件，直接问服务器）
    std::wstring QueryAuditByOrder(const std::wstring& order)
    {
        if (order.empty()) return L"订单号为空";
        const std::wstring path = AsaApi::Tools::Utf8Decode(
            AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ZeroARKTrait/trait_audit.log");
        std::ifstream f(AsaApi::Tools::Utf8Encode(path).c_str(), std::ios::binary);
        if (!f.is_open()) return L"审计文件还不存在";

        std::vector<std::wstring> hits;
        std::string line;
        while (std::getline(f, line))
        {
            const std::wstring w = AsaApi::Tools::Utf8Decode(line);
            if (w.find(order) != std::wstring::npos)
            {
                hits.push_back(w);
                if (hits.size() > 5) hits.erase(hits.begin());   // 只保留最近 5 条
            }
        }
        if (hits.empty()) return std::wstring(L"订单 ") + order + L" 没有任何审计记录";
        std::wstring out = std::wstring(L"订单 ") + order + L" 共 " + std::to_wstring(hits.size()) + L" 条：";
        for (size_t i = 0; i < hits.size(); ++i) out += L"\n" + hits[i];
        return out;
    }

    std::wstring NowStamp()
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return std::wstring(buf);
    }

    // 追加一行到 ArkApi/Plugins/ZeroARKTrait/trait_audit.log（UTF-8，便于机器人对账）
    void AuditLog(const std::wstring& line)
    {
        const std::wstring path = AsaApi::Tools::Utf8Decode(
            AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ZeroARKTrait/trait_audit.log");
        std::ofstream f(AsaApi::Tools::Utf8Encode(path).c_str(), std::ios::app | std::ios::binary);
        if (!f.is_open()) return;
        f << AsaApi::Tools::Utf8Encode(NowStamp() + L" | " + line) << "\n";
        f.flush();
    }

    // ---------------------------------------------------- 词条中文名映射（v1.1）
    // 输入：中文/英文都认；输出：显示「中文(英文)」。内置表 = 官方中文名；旧简称可写 trait_names.ini 当别名。
    struct TraitNamePair { const wchar_t* cn; const wchar_t* en; };
    //  官方中文名（2026-09-18 实测自游戏内 HUD；v1.9.5 起改为官方名，旧简称走 trait_names.ini 当别名）
    const TraitNamePair kTraitNames[] = {
        { L"好斗", L"Aggressive" }, { L"爆发", L"Angry" },
        { L"健壮", L"Athletic" }, { L"放松", L"Carefree" },
        { L"寒冷", L"Cold" }, { L"胆怯", L"Cowardly" },
        { L"令人分心", L"Distracting" }, { L"日行性", L"Diurnal" },
        { L"易兴奋", L"Excitable" }, { L"快速学习", L"FastLearner" },
        { L"肉盾", L"Fatty" }, { L"干劲十足", L"Frenetic" },
        { L"巨人杀手", L"Giantslaying" }, { L"沉重打击", L"HeavyHitting" },
        { L"精英杀手", L"Kingslaying" }, { L"夜行性", L"Nocturnal" },
        { L"迟钝", L"Numb" }, { L"保护", L"Protective" },
        { L"快速打击", L"QuickHitting" }, { L"代谢缓慢", L"SlowMetabolism" },
        { L"高持久力", L"Sprinter" }, { L"水栖性", L"Swimmer" },
        { L"顽强", L"Tenacious" }, { L"嗜血", L"Vampiric" },
        { L"暖和", L"Warm" },
        // 搬运类（按资源分类的同族词条）
        { L"洞穴资源-减重", L"AberrantCarrier" }, { L"尸体资源-减重", L"CarcassCarrier" },
        { L"奇特资源-减重", L"ExoticCarrier" }, { L"废土资源-减重", L"ExtinctionCarrier" },
        { L"肉类资源-减重", L"MeatCarrier" }, { L"矿物资源-减重", L"MineralCarrier" },
        { L"植被资源-减重", L"PlantCarrier" }, { L"沙漠资源-减重", L"ScorchedCarrier" },
        // 遗传类（影响后代继承/突变概率）
        { L"食物-虚弱", L"InheritFoodFrail" }, { L"突变-食物", L"InheritFoodMutable" }, { L"食物-强壮", L"InheritFoodRobust" },
        { L"血量-虚弱", L"InheritHealthFrail" }, { L"突变-血量", L"InheritHealthMutable" }, { L"血量-强壮", L"InheritHealthRobust" },
        { L"近战-虚弱", L"InheritMeleeFrail" }, { L"突变-近战", L"InheritMeleeMutable" }, { L"近战-强壮", L"InheritMeleeRobust" },
        { L"氧气-虚弱", L"InheritOxygenFrail" }, { L"突变-氧气", L"InheritOxygenMutable" }, { L"氧气-强壮", L"InheritOxygenRobust" },
        { L"耐力-虚弱", L"InheritStaminaFrail" }, { L"突变-耐力", L"InheritStaminaMutable" }, { L"耐力-强壮", L"InheritStaminaRobust" },
        { L"负重-虚弱", L"InheritWeightFrail" }, { L"突变-负重", L"InheritWeightMutable" }, { L"负重-强壮", L"InheritWeightRobust" },
        // ---- 旧简称（兼容输入用；显示仍以官方名称为准，因为官方条目排在前面）----
        { L"愤怒", L"Angry" }, { L"耐寒", L"Cold" }, { L"胆小", L"Cowardly" },
        { L"扰敌", L"Distracting" }, { L"昼行", L"Diurnal" }, { L"肥胖", L"Fatty" },
        { L"狂乱", L"Frenetic" }, { L"屠巨", L"Giantslaying" }, { L"重击", L"HeavyHitting" },
        { L"屠王", L"Kingslaying" }, { L"夜行", L"Nocturnal" }, { L"麻木", L"Numb" },
        { L"护主", L"Protective" }, { L"速击", L"QuickHitting" }, { L"冲刺", L"Sprinter" },
        { L"游泳", L"Swimmer" }, { L"坚韧", L"Tenacious" }, { L"吸血", L"Vampiric" },
        { L"耐热", L"Warm" }, { L"异变搬运", L"AberrantCarrier" }, { L"尸体搬运", L"CarcassCarrier" },
        { L"奇物搬运", L"ExoticCarrier" }, { L"灭绝搬运", L"ExtinctionCarrier" }, { L"肉类搬运", L"MeatCarrier" },
        { L"矿物搬运", L"MineralCarrier" }, { L"植物搬运", L"PlantCarrier" }, { L"焦土搬运", L"ScorchedCarrier" },
        { L"食性劣化", L"InheritFoodFrail" }, { L"食性易变", L"InheritFoodMutable" }, { L"食性稳健", L"InheritFoodRobust" },
        { L"生命劣化", L"InheritHealthFrail" }, { L"生命易变", L"InheritHealthMutable" }, { L"生命稳健", L"InheritHealthRobust" },
        { L"近战劣化", L"InheritMeleeFrail" }, { L"近战易变", L"InheritMeleeMutable" }, { L"近战稳健", L"InheritMeleeRobust" },
        { L"氧气劣化", L"InheritOxygenFrail" }, { L"氧气易变", L"InheritOxygenMutable" }, { L"氧气稳健", L"InheritOxygenRobust" },
        { L"耐力劣化", L"InheritStaminaFrail" }, { L"耐力易变", L"InheritStaminaMutable" }, { L"耐力稳健", L"InheritStaminaRobust" },
        { L"负重劣化", L"InheritWeightFrail" }, { L"负重易变", L"InheritWeightMutable" }, { L"负重稳健", L"InheritWeightRobust" },
    };

    std::vector<std::pair<std::wstring, std::wstring>> g_extra_names;   // ini 覆盖（中文 -> 英文）

    // 中文 -> 英文内部名（找不到就当英文原样返回）
    std::wstring NormalizeTraitName(const std::wstring& in)
    {
        for (size_t i = 0; i < _countof(kTraitNames); ++i)
            if (_wcsicmp(in.c_str(), kTraitNames[i].cn) == 0) return kTraitNames[i].en;
        for (size_t i = 0; i < g_extra_names.size(); ++i)
            if (_wcsicmp(in.c_str(), g_extra_names[i].first.c_str()) == 0) return g_extra_names[i].second;
        return in;
    }

    // 英文内部名 -> 中文显示（找不到就返回英文）
    std::wstring DisplayTraitName(const std::wstring& en)
    {
        std::wstring base = en;
        const size_t lb = base.find(L'[');
        std::wstring tier;
        if (lb != std::wstring::npos) { tier = base.substr(lb); base = base.substr(0, lb); }

        std::wstring cn;
        for (size_t i = 0; i < _countof(kTraitNames); ++i)
            if (_wcsicmp(base.c_str(), kTraitNames[i].en) == 0) { cn = kTraitNames[i].cn; break; }
        if (cn.empty())
            for (size_t i = 0; i < g_extra_names.size(); ++i)
                if (_wcsicmp(base.c_str(), g_extra_names[i].second.c_str()) == 0) { cn = g_extra_names[i].first; break; }

        return cn.empty() ? en : (cn + L"(" + base + L")" + tier);
    }

    // 载入 zerotrait.ini（键=值；不存在则用默认值）
    void LoadConfig()
    {
        const std::wstring path = AsaApi::Tools::Utf8Decode(
            AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ZeroARKTrait/zerotrait.ini");
        std::wifstream f(path.c_str());
        if (!f.is_open())
        {
            Log::GetLog()->info("[ZeroARKTrait] zerotrait.ini not found, using defaults");
            return;
        }
        std::wstring line;
        while (std::getline(f, line))
        {
            const std::wstring t = Trim(line);
            if (t.empty() || t[0] == L'#' || t[0] == L';') continue;
            const size_t eq = t.find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::wstring k = Trim(t.substr(0, eq));
            const std::wstring v = Trim(t.substr(eq + 1));
            if (k == L"near_radius")            g_cfg.near_radius = (float)_wtof(v.c_str());
            else if (k == L"id_search_radius")  g_cfg.id_search_radius = (float)_wtof(v.c_str());
            else if (k == L"scan_budget_per_tick") g_cfg.scan_budget = _wtoi(v.c_str());
            else if (k == L"chat_query")        g_cfg.chat_query = !(v == L"0" || v == L"false");
            else if (k == L"chat_cooldown_ms")  g_cfg.chat_cooldown_ms = _wtoi(v.c_str());
            else if (k == L"buy_request")       g_cfg.buy_request = !(v == L"0" || v == L"false");
            else if (k == L"list_radius")       g_cfg.list_radius = (float)_wtof(v.c_str());
            else if (k == L"list_max")          g_cfg.list_max = _wtoi(v.c_str());
        }
        if (g_cfg.scan_budget < 100) g_cfg.scan_budget = 100;
        if (g_cfg.list_radius < 500.f) g_cfg.list_radius = 500.f;
        if (g_cfg.list_max < 1)   g_cfg.list_max = 1;
        if (g_cfg.list_max > 40)  g_cfg.list_max = 40;
        Log::GetLog()->info("[ZeroARKTrait] config: near={} id_search={} budget/tick={} chat_query={} buy_request={} list_radius={} list_max={}",
                            g_cfg.near_radius, g_cfg.id_search_radius, g_cfg.scan_budget, g_cfg.chat_query,
                            g_cfg.buy_request, g_cfg.list_radius, g_cfg.list_max);
    }

    // 载入 trait_names.ini（每行 中文=英文；# 开头为注释）
    // 必须按 **UTF-8 字节**读再 Utf8Decode —— 原来用 std::wifstream 会把 UTF-8 中文
    //   当成"一个字节一个 wchar_t"，内存里变成乱码，中文别名**永远匹配不上**（英文别名才碰巧能用）。
    void LoadTraitNames()
    {
        g_extra_names.clear();
        const std::wstring path = AsaApi::Tools::Utf8Decode(
            AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ZeroARKTrait/trait_names.ini");
        std::ifstream f(AsaApi::Tools::Utf8Encode(path).c_str(), std::ios::binary);
        if (!f.is_open()) return;

        std::string raw;
        while (std::getline(f, raw))
        {
            if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF
                && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
                raw = raw.substr(3);                      // 跳过 UTF-8 BOM
            const std::wstring t = Trim(AsaApi::Tools::Utf8Decode(raw));
            if (t.empty() || t[0] == L'#' || t[0] == L';') continue;
            const size_t eq = t.find(L'=');
            if (eq == std::wstring::npos) continue;
            const std::wstring cn = Trim(t.substr(0, eq));
            const std::wstring en = Trim(t.substr(eq + 1));
            if (!cn.empty() && !en.empty()) g_extra_names.push_back(std::make_pair(cn, en));
        }
        Log::GetLog()->info("[ZeroARKTrait] trait_names.ini loaded: {} entries", g_extra_names.size());
    }

    // ============================================================
    // 新增：词条商店的价目表 / 恐龙价值表 / 设置三份配置
    //   设计约束：
    //     「插件除了 asaapi 的那个官方权限插件和 modui 以外，不要有其他依赖项，
    //       能配置的都放到配置文件」
    //   ⇒ 所以**价格、物种系数、等级系数、点数来源全部外置**，
    //     别人拿到插件只要改 ini 就能用，且**不引入任何第三方插件依赖**。
    //   文件（UTF-8，**必须按字节读**，中文才不会乱码 —— 同 LoadTraitNames 的教训）：
    //     ZeroARKTrait-价目表.ini      每行：词条中文名=基础价
    //     ZeroARKTrait-恐龙价值表.ini   每行：物种名=价值系数   （物种名可用中/英，模糊匹配）
    //     ZeroARKTrait-设置.ini         键=值：tier1/tier2/tier3 系数、points_mode、points_cmd…
    // ============================================================
    static std::vector<std::pair<std::wstring, double>> g_shop_price_table;  // 词条名 → 基础价
    static std::vector<std::pair<std::wstring, double>> g_shop_species_table; // 物种名 → 价值系数
    static double g_shop_tier_coef[3] = { 1.0, 1.5, 2.0 };                   // 1级/2级/3级
    static std::wstring g_shop_points_mode = L"push";   // push（RCON 推）| db（插件自己读数据库）
    static std::wstring g_shop_points_cmd;              // （保留）cmd 模式要执行的命令
    static int g_shop_points_cmd_seconds = 30;
    // mod 数据下发周期（秒）—— 之前硬编码 30 秒，玩家按 F2 后要等半分钟才见数据
    static int g_shop_push_seconds = 5;
    // 面板开合键（可配置，默认 F2）——  插件把它随数据推给客户端 mod，
    //   mod 存成变量后再用它做按键判断（ArkShopUI 的 UiKey 就是这个思路）
static std::wstring g_shop_ui_key = L"F2";

    // 进服提醒（照 EnhancedHLNA 的做法：玩家上线/重登后主动发一条聊天提示）
    //   -settings.ini 里：join_tip=1/0 开关；join_tip_delay=<秒> 延迟（等玩家加载完再发）
    static bool g_shop_join_tip = true;
    static int  g_shop_join_tip_delay = 8;
static std::map<std::wstring, unsigned long long> g_join_tip_due;   // eos -> 到点该发的 tick
    // eos -> 上次见到的 PlayerController 指针（换了对象 = 重连 ⇒ 再提醒一次）
    static std::map<std::wstring, void*> g_join_tip_pc;

    // 点数数据库（points_mode=db 时用；全部来自配置文件）
    static std::wstring g_db_host  = L"";
    static int          g_db_port  = 3306;
    static std::wstring g_db_user  = L"";
    static std::wstring g_db_pass  = L"";
    static std::wstring g_db_name  = L"";
    static std::wstring g_db_table = L"arkshopplayers";
    static std::wstring g_db_key   = L"EosId";
    static std::wstring g_db_val   = L"Points";
    static int          g_db_refresh_seconds = 20;      // 同一玩家的缓存有效期
    // 没有 ArkShop 的服也能用
    static bool         g_db_auto_create = true;        // 没库/没表就自己建（尽力）
    static bool         g_db_auto_insert = true;        // 查不到玩家就自动建档（0 点）
    static bool         g_db_table_ready = false;       // 本进程内是否已确保过表
    // 缓存：EOS ID → (点数, 取到的时刻)
    static std::map<std::wstring, std::pair<int, DWORD>> g_db_points_cache;
    static std::mutex   g_db_mu;

    // 把宽串转成窄串（MySQL 协议都是窄字节；库/账号/密码本来就只有 ASCII）
    std::string Narrow(const std::wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s((size_t)n, 0);
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
        return s;
    }

    // 没有 ArkShop 的服 —— 插件自建库表（尽力而为，失败不影响运行）
    //   注意：库必须已存在才能连上（MySQL 协议里连库是握手的一部分），
    //   所以"建库"这步只能先连到不带库名的连接去执行。这里做两件事：
    //     ① 先按配置的库名连；连不上是 1049(unknown database) 就连到 information_schema
    //        再 CREATE DATABASE，然后再连一次目标库
    //     ② 连上目标库后 CREATE TABLE IF NOT EXISTS（列名与 ArkShop 一致）
    bool EnsureDbTable(std::string& err)
    {
        if (g_db_host.empty()) { err = "db_host not configured"; return false; }
        const std::string host = Narrow(g_db_host);
        const std::string user = Narrow(g_db_user);
        const std::string pass = Narrow(g_db_pass);
        const std::string db   = Narrow(g_db_name);
        const std::string tbl  = Narrow(g_db_table);
        const std::string key  = Narrow(g_db_key);
        const std::string val  = Narrow(g_db_val);

        // 建表语句（与 ArkShop 的 arkshopplayers 一致的列风格：主键 EosId + 整型 Points）
        const std::string createSql =
            "CREATE TABLE IF NOT EXISTS `" + tbl + "` ("
            "`" + key + "` VARCHAR(64) NOT NULL,"
            "`" + val + "` INT NOT NULL DEFAULT 0,"
            "PRIMARY KEY (`" + key + "`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

        // ① 直接连目标库
        {
            minimysql::Conn c;
            if (c.Open(host, g_db_port, user, pass, db, err))
            {
                std::string e2;
                if (c.Exec(createSql, e2)) { g_db_table_ready = true; c.Close(); return true; }
                // 表建不了（可能是只读账号）—— 不算致命，接着试 SELECT
                Log::GetLog()->warn("[ZeroARKTrait] auto_create: CREATE TABLE failed: {}", e2);
                c.Close();
                return true;         // 连得上就算"库可用"
            }
        }

        // ② 目标库不存在（1049）⇒ 连 information_schema 建库，再重试一次
        Log::GetLog()->warn("[ZeroARKTrait] auto_create: connect to `{}` failed: {}", db, err);
        if (!g_db_auto_create) return false;
        {
            minimysql::Conn c;
            std::string e0;
            if (!c.Open(host, g_db_port, user, pass, "information_schema", e0))
            {
                err = "connect via information_schema failed: " + e0;
                return false;
            }
            std::string e1;
            if (!c.Exec("CREATE DATABASE IF NOT EXISTS `" + db + "` DEFAULT CHARSET utf8mb4", e1))
            {
                err = "CREATE DATABASE failed: " + e1;
                c.Close();
                return false;
            }
            c.Close();
            Log::GetLog()->info("[ZeroARKTrait] auto_create: created database `{}`", db);
        }
        {
            minimysql::Conn c;
            std::string e3;
            if (!c.Open(host, g_db_port, user, pass, db, e3)) { err = "reconnect failed: " + e3; return false; }
            std::string e4;
            if (c.Exec(createSql, e4)) { g_db_table_ready = true; c.Close(); return true; }
            err = "CREATE TABLE failed: " + e4;
            c.Close();
            return false;
        }
    }

    // 从数据库读某玩家的点数。成功返回 true
    //   readOnly=false 时允许自动建档（没这条记录就插一条 0 点）
    bool FetchPointsFromDb(const std::wstring& eosId, int& out, bool allowAutoInsert = true)
    {
        if (g_db_host.empty() || g_db_user.empty() || eosId.empty()) return false;

        // 先看缓存
        {
            std::lock_guard<std::mutex> lock(g_db_mu);
            std::map<std::wstring, std::pair<int, DWORD>>::const_iterator it = g_db_points_cache.find(eosId);
            if (it != g_db_points_cache.end())
            {
                const DWORD age = (GetTickCount() - it->second.second) / 1000;
                if (age < (DWORD)g_db_refresh_seconds) { out = it->second.first; return true; }
            }
        }

        minimysql::Conn conn;
        std::string err;
        // 首次调用时确保库/表存在（auto_create=1 生效；失败不致命，继续试查）
        if (!g_db_table_ready)
        {
            std::string e0;
            if (EnsureDbTable(e0)) g_db_table_ready = true;
            else Log::GetLog()->warn("[ZeroARKTrait] auto_create: {}", e0);
        }
        if (!conn.Open(Narrow(g_db_host), g_db_port, Narrow(g_db_user), Narrow(g_db_pass), Narrow(g_db_name), err))
        {
            Log::GetLog()->warn("[ZeroARKTrait] db connect failed: {}", err);
            return false;
        }
        const std::string sql = "SELECT `" + Narrow(g_db_val) + "` FROM `" + Narrow(g_db_table) + "` WHERE `"
                              + Narrow(g_db_key) + "`='" + Narrow(eosId) + "' LIMIT 1";
        std::string val;
        if (!conn.QueryOne(sql, val, err))
        {
            // 查不到这条记录 ⇒ 自动建档（0 点）
            //   这是"自建数据库"的核心：让没装 ArkShop 的服也能立刻用起来
            if (err == "no rows" && allowAutoInsert && g_db_auto_insert)
            {
                std::string e2;
                const std::string ins = "INSERT IGNORE INTO `" + Narrow(g_db_table) + "` (`" + Narrow(g_db_key)
                                      + "`, `" + Narrow(g_db_val) + "`) VALUES ('" + Narrow(eosId) + "', 0)";
                if (conn.Exec(ins, e2))
                {
                    conn.Close();
                    out = 0;
                    { std::lock_guard<std::mutex> lock(g_db_mu); g_db_points_cache[eosId] = std::make_pair(0, GetTickCount()); }
                    Log::GetLog()->info("[ZeroARKTrait] db auto-created row (0 points) for {}", AsaApi::Tools::Utf8Encode(eosId));
                    return true;
                }
                Log::GetLog()->warn("[ZeroARKTrait] db auto-insert failed: {}", e2);
            }
            Log::GetLog()->warn("[ZeroARKTrait] db query failed: {} (sql={})", err, sql);
            return false;
        }
        conn.Close();
        try { out = std::stoi(val); }
        catch (...) { Log::GetLog()->warn("[ZeroARKTrait] db value not a number: [{}]", val); return false; }

        {
            std::lock_guard<std::mutex> lock(g_db_mu);
            g_db_points_cache[eosId] = std::make_pair(out, GetTickCount());
        }
        return true;
    }

    // 通用：按行读一个 UTF-8 的 ini，回调 (key, value)
    void LoadUtf8Kv(const std::wstring& fileName,
                    void (*cb)(const std::wstring&, const std::wstring&))
    {
        //  2026-09-22 踩的坑：`std::ifstream(const char*)` 在 MSVC 上按本地代码页解析路径
        //   ⇒ 文件名里有中文（本文件的三个 ini 都带中文）就永远打不开(open=0)。
        //   原来 trait_names.ini 是纯 ASCII 名，所以这个坑一直没暴露。
        //   修法：直接用 MSVC 的 wchar_t* 重载 `ifstream(const wchar_t*)` ⇒ 原生支持 Unicode 路径。
        const std::string narrow = AsaApi::Tools::GetCurrentDir() + "/ArkApi/Plugins/ZeroARKTrait/" + AsaApi::Tools::Utf8Encode(fileName);
        const std::wstring wpath = AsaApi::Tools::Utf8Decode(narrow);
        std::ifstream f(wpath.c_str(), std::ios::binary);
        if (!f.is_open())
        {
            Log::GetLog()->warn("[ZeroARKTrait] cfg NOT found: {}", narrow);
            return;
        }
        std::string raw;
        while (std::getline(f, raw))
        {
            if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF
                && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
                raw = raw.substr(3);
            std::wstring t = Trim(AsaApi::Tools::Utf8Decode(raw));
            if (t.empty() || t[0] == L'#' || t[0] == L';') continue;
            const size_t eq = t.find(L'=');
            if (eq == std::wstring::npos) continue;
            cb(Trim(t.substr(0, eq)), Trim(t.substr(eq + 1)));
        }
    }

    // ── 价目表：词条名=基础价
    // ===== v1.15.8：给玩家看的消息 —— 全部可配置 + 中英双语 =====
    //   两份语言文件：ZeroARKTrait-messages-<lang>.ini（零注释 / UTF-8 无 BOM）
    //   -settings.ini 的 msg_lang 决定读哪一份（默认 zh）；想加别的语言，复制一份改名即可
    //   模板占位符 {0}~{3}；值里的 \n 会在发出前变成真换行；每条消息自带颜色标签 ⇒ 可单独改样式
    static std::map<std::wstring, std::wstring> g_msgs;
    static std::wstring g_msg_lang = L"zh";
    static int g_msg_missing = 0;

    void LoadMessages()
    {
        g_msgs.clear();
        g_msg_missing = 0;
        const std::wstring file = L"ZeroARKTrait-messages-" + g_msg_lang + L".ini";
        LoadUtf8Kv(file.c_str(), [](const std::wstring& k, const std::wstring& v) {
            if (!k.empty()) g_msgs[k] = v;
        });
        Log::GetLog()->info("[ZeroARKTrait] messages: lang={} entries={}",
                            AsaApi::Tools::Utf8Encode(g_msg_lang), g_msgs.size());
        if (g_msgs.empty())
            Log::GetLog()->warn("[ZeroARKTrait] message file not found or empty: {}",
                                AsaApi::Tools::Utf8Encode(file));
    }

    std::wstring Msg(const std::wstring& key,
                     const std::wstring& a0 = L"", const std::wstring& a1 = L"",
                     const std::wstring& a2 = L"", const std::wstring& a3 = L"")
    {
        std::map<std::wstring, std::wstring>::iterator it = g_msgs.find(key);
        if (it == g_msgs.end())
        {
            if (g_msg_missing < 20)
            {
                ++g_msg_missing;
                Log::GetLog()->warn("[ZeroARKTrait] msg key missing: {}", AsaApi::Tools::Utf8Encode(key));
            }
            return L"[msg:" + key + L"]";
        }
        std::wstring t = it->second;
        const std::wstring* args[4] = { &a0, &a1, &a2, &a3 };
        for (int i = 0; i < 4; ++i)
        {
            const std::wstring ph = L"{" + std::to_wstring(i) + L"}";
            size_t p;
            while ((p = t.find(ph)) != std::wstring::npos) t.replace(p, ph.size(), *args[i]);
        }
        std::wstring out;
        for (size_t i = 0; i < t.size(); ++i)
        {
            if (t[i] == L'\\' && i + 1 < t.size() && t[i + 1] == L'n') { out += L'\n'; ++i; }
            else out += t[i];
        }
        return out;
    }

    // 取消息原文—— 不做 `{0}~{3}` 替换
    //   为什么需要：`Msg()` 会把模板里的 `{0}` 换成传入的（默认空）参数
    //   ⇒ 面板拿到的是 `分`（占位符被吃掉）⇒ 数字永远出不来（2026-09-28 真机破案）
    //   ⇒ `Fmt_*` 这类"格式模板"必须原样下发，让面板自己填数据
    std::wstring MsgRaw(const std::wstring& key)
    {
        std::map<std::wstring, std::wstring>::iterator it = g_msgs.find(key);
        if (it == g_msgs.end()) return Msg(key);   // 缺失时沿用 `[msg:key]` + 告警
        return it->second;
    }

    void LoadShopPriceTable()
    {
        g_shop_price_table.clear();
        LoadUtf8Kv(L"ZeroARKTrait-prices.ini", [](const std::wstring& k, const std::wstring& v) {
            if (!k.empty() && !v.empty()) g_shop_price_table.push_back(std::make_pair(k, _wtof(v.c_str())));
        });
        Log::GetLog()->info("[ZeroARKTrait] price table: {} entries", g_shop_price_table.size());
    }

    static std::wstring g_shop_trait_lang = L"zh";     // 给玩家看的词条名用中文还是英文（zh|en）
    std::wstring LangNameOf(const std::wstring& in);   //  v1.16.1（实现见下方，先声明）

    // 用插件自己读到的价目表拼出下发给客户端面板的正文。
    //   目的：没人从外部推价目时，面板照样能显示全部条目（用户要求"让插件自己读"）；
    //   外部经 RCON 推来的 g_shop_price_text 仍作为可选覆盖优先使用。
    std::wstring BuildShopDataFromOwnTable()
    {
        std::wstring s = L"POINTS|0";
        for (size_t i = 0; i < g_shop_price_table.size(); ++i)
        {
            wchar_t buf[96];
            // 按 trait_lang 决定推中文名还是英文名（玩家只看一种）
            swprintf_s(buf, L"|%s|%g", LangNameOf(g_shop_price_table[i].first).c_str(), g_shop_price_table[i].second);
            s += buf;
        }
        return s;
    }

    // 把内部名/中文名/别名统一成"当前显示语言"的名字（只给玩家看）
    //   协议 / 审计 / 数据库 永远用内部英文名 （红线不动）
    std::wstring LangNameOf(const std::wstring& in)
    {
        const bool wantEn = (g_shop_trait_lang == L"en");
        for (size_t i = 0; i < _countof(kTraitNames); ++i)
        {
            if (_wcsicmp(in.c_str(), kTraitNames[i].cn) == 0 ||
                _wcsicmp(in.c_str(), kTraitNames[i].en) == 0)
                return wantEn ? std::wstring(kTraitNames[i].en) : std::wstring(kTraitNames[i].cn);
        }
        return in;     // 认不出的原样返回（别吞掉）
    }

    // ===== v1.15.7：权限组折扣 + 官方 Permissions 插件（动态接入）=====
    //   折扣表：权限组名 -> 折扣百分比（10 = 打九折），来自 ZeroARKTrait-discounts.ini（零注释）
    static std::vector<std::pair<std::wstring, float>> g_shop_discount_table;
    static std::wstring g_shop_discount_mode = L"highest";   // highest（取最大 默认）| stack（叠加）
    static bool g_perm_has_perm_enabled = true;              // 是否用 Permissions 判权限（false=回退本地闸门）
    static std::wstring g_shop_perm_admin = L"ZeroARKTrait.admin";   // 管理权限串（Permissions 里配）

    void LoadDiscountTable()
    {
        g_shop_discount_table.clear();
        LoadUtf8Kv(L"ZeroARKTrait-discounts.ini", [](const std::wstring& k, const std::wstring& v) {
            if (!k.empty() && !v.empty()) g_shop_discount_table.push_back(std::make_pair(k, _wtof(v.c_str())));
        });
        Log::GetLog()->info("[ZeroARKTrait] discount table: {} entries, mode={}",
                            g_shop_discount_table.size(), AsaApi::Tools::Utf8Encode(g_shop_discount_mode));
    }

    // ---- 官方 Permissions：动态加载（没装也不影响启动，照本机其它插件的做法）----
    //   导出名是从 Permissions.dll 的 dumpbin /exports 原文抄下来的（没有头文件，所以按符号来）
    typedef bool (*FnPermIsPlayerHasPermission)(const FString&, const FString&);
    typedef TArray<FString>* (*FnPermGetPlayerGroups)(TArray<FString>*, const FString&);
    static HMODULE g_perm_dll = nullptr;
    static FnPermIsPlayerHasPermission g_perm_has = nullptr;
    static FnPermGetPlayerGroups      g_perm_groups = nullptr;
    static bool g_perm_tried = false;

    void LoadPermissionsApi()
    {
        if (g_perm_tried) return;
        g_perm_tried = true;
        // GetCurrentDir() 返回的是窄字符串(AsaApi 老规矩) ⇒ 用窄串拼路径 + LoadLibraryA
        const std::string dir = AsaApi::Tools::GetCurrentDir();
        const std::string path = dir + "\\ArkApi\\Plugins\\Permissions\\Permissions.dll";
        g_perm_dll = ::LoadLibraryA(path.c_str());
        if (g_perm_dll == nullptr)
        {
            Log::GetLog()->warn("[ZeroARKTrait] Permissions plugin NOT found ({}) -> 权限判断回退到本地闸门", path);
            return;
        }
        g_perm_has = (FnPermIsPlayerHasPermission)::GetProcAddress(g_perm_dll,
            "?IsPlayerHasPermission@Permissions@@YA_NAEBVFString@@0@Z");
        g_perm_groups = (FnPermGetPlayerGroups)::GetProcAddress(g_perm_dll,
            "?GetPlayerGroups@Permissions@@YA?AV?$TArray@VFString@@V?$TSizedDefaultAllocator@$0CA@@@@@AEBVFString@@@Z");
        Log::GetLog()->info("[ZeroARKTrait] Permissions plugin loaded: IsPlayerHasPermission={} GetPlayerGroups={}",
                            (g_perm_has != nullptr), (g_perm_groups != nullptr));
    }

    bool PermHas(const std::wstring& eos, const std::wstring& perm)
    {
        if (g_perm_has == nullptr || eos.empty() || perm.empty()) return false;
        FString a(eos.c_str()), b(perm.c_str());
        return g_perm_has(a, b);
    }

    std::vector<std::wstring> PermGroups(const std::wstring& eos)
    {
        std::vector<std::wstring> out;
        if (g_perm_groups == nullptr || eos.empty()) return out;
        TArray<FString> arr;
        FString a(eos.c_str());
        g_perm_groups(&arr, a);
        for (int i = 0; i < arr.Num(); ++i) out.push_back(std::wstring(*arr[i]));
        return out;
    }

    // 算这个玩家在这些权限组下的折扣百分比（highest=取最大 / stack=叠加；上限 95% 防 0 元）
    float DiscountFor(const std::vector<std::wstring>& groups)
    {
        float best = 0.0f, sum = 0.0f;
        for (size_t i = 0; i < groups.size(); ++i)
        {
            for (size_t j = 0; j < g_shop_discount_table.size(); ++j)
            {
                if (_wcsicmp(groups[i].c_str(), g_shop_discount_table[j].first.c_str()) == 0)
                {
                    const float d = g_shop_discount_table[j].second;
                    if (d > best) best = d;
                    sum += d;
                }
            }
        }
        float use = (g_shop_discount_mode == L"stack") ? sum : best;
        if (use < 0.0f) use = 0.0f;
        if (use > 95.0f) use = 95.0f;
        return use;
    }

    // 权限组查询带 30 秒缓存（别每 2 秒对每个玩家都去问一次 Permissions）
    static std::map<std::wstring, std::pair<std::vector<std::wstring>, unsigned long long>> g_perm_group_cache;
    std::vector<std::wstring> PermGroupsCached(const std::wstring& eos)
    {
        if (eos.empty()) return std::vector<std::wstring>();
        const unsigned long long now = GetTickCount64();
        std::map<std::wstring, std::pair<std::vector<std::wstring>, unsigned long long>>::iterator it = g_perm_group_cache.find(eos);
        if (it != g_perm_group_cache.end() && now - it->second.second < 30000ULL) return it->second.first;
        std::vector<std::wstring> g = PermGroups(eos);
        g_perm_group_cache[eos] = std::make_pair(g, now);
        return g;
    }

    // ── 恐龙价值表：物种=系数
    void LoadShopSpeciesTable()
    {
        g_shop_species_table.clear();
        LoadUtf8Kv(L"ZeroARKTrait-species.ini", [](const std::wstring& k, const std::wstring& v) {
            if (!k.empty() && !v.empty()) g_shop_species_table.push_back(std::make_pair(k, _wtof(v.c_str())));
        });
        Log::GetLog()->info("[ZeroARKTrait] species table: {} entries", g_shop_species_table.size());
    }

    // ── 商店设置：等级系数 + 点数来源
    // 自给自足的点数（实现写在下方，这里先声明 ⇒ 配置加载 / 启动处就能用）
    static std::wstring g_charge_mode = L"bot";        // plugin=插件自己扣 | bot=机器人扣（老方式）| none
    // 功能总开关（用户要求「功能可以开关，不能修改」 默认都保持原有行为）
    static bool g_ui_enabled = true;                   // 0 = 不自动挂面板 buff（只留聊天菜单）
    static bool g_order_enabled = true;                // 0 = 拒绝 Trait 订单（只留查询/加删）
    // （g_shop_trait_lang 的声明已提到上方）
    void LoadFilePoints(bool quiet = false);
    int  FilePointsOf(const std::wstring& eos);
    bool FilePointsAdd(const std::wstring& eos, int delta, int* nowValue);
    // 发货失败 ⇒ 把刚扣的点退回去（定义在 FilePointsAdd 之后）
    bool RefundJobIfCharged(const Job& j, const std::wstring& reason);
    bool FilePointsErase(const std::wstring& eos);
    int  ComputePrice(const std::wstring& traitCn, int tier, const std::wstring& species, const std::wstring& eos);
    int  ComputePriceWith(const std::wstring& traitCn, int tier, const std::wstring& species,
                          const std::vector<std::wstring>& groups);
    bool ChargePoints(const std::wstring& eos, int cost, std::wstring* err);

    void LoadShopSettings()
    {
        LoadUtf8Kv(L"ZeroARKTrait-settings.ini", [](const std::wstring& k, const std::wstring& v) {
            if (k == L"tier1")                 g_shop_tier_coef[0] = _wtof(v.c_str());
            else if (k == L"tier2")            g_shop_tier_coef[1] = _wtof(v.c_str());
            else if (k == L"tier3")            g_shop_tier_coef[2] = _wtof(v.c_str());
            else if (k == L"points_mode")      g_shop_points_mode = v;
            else if (k == L"points_cmd")       g_shop_points_cmd = v;
            else if (k == L"points_cmd_seconds") g_shop_points_cmd_seconds = _wtoi(v.c_str());
            else if (k == L"push_seconds")     g_shop_push_seconds = _wtoi(v.c_str());
            // 面板开合键可配置（默认 F2）—— 建议填 UE 的按键名，如 F2 / F3 / F4
else if (k == L"ui_key")           g_shop_ui_key = v;
            else if (k == L"join_tip")         g_shop_join_tip = !(v == L"0" || v == L"false");
            else if (k == L"join_tip_delay")   g_shop_join_tip_delay = _wtoi(v.c_str());
            // 权限组折扣 + 官方 Permissions 插件
            else if (k == L"discount_mode")    g_shop_discount_mode = v;                 // highest | stack
            else if (k == L"perm_admin")       g_shop_perm_admin = v;                    // 管理权限串
            else if (k == L"perm_use")         g_perm_has_perm_enabled = !(v == L"0" || v == L"false" || v == L"no");
            // 给玩家看的消息用哪份语言文件（zh / en / 你自己加的语言码）
            else if (k == L"msg_lang")         g_msg_lang = v;
            // 谁负责扣点（plugin=插件自己扣 / bot=机器人扣（老方式）/ none=不扣）
            else if (k == L"charge_mode")      g_charge_mode = v;
            // 给玩家看的词条名语言（zh 中文 / en English）—— 只影响显示
            else if (k == L"trait_lang")       g_shop_trait_lang = v;
            // 功能总开关（0 / false / no = 关）
            else if (k == L"ui_enabled")       g_ui_enabled = !(v == L"0" || v == L"false" || v == L"no");
            else if (k == L"order_enabled")    g_order_enabled = !(v == L"0" || v == L"false" || v == L"no");
            // 点数数据库（points_mode=db 用）
            else if (k == L"db_host")          g_db_host = v;
            else if (k == L"db_port")          g_db_port = _wtoi(v.c_str());
            else if (k == L"db_user")          g_db_user = v;
            else if (k == L"db_pass")          g_db_pass = v;
            else if (k == L"db_name")          g_db_name = v;
            else if (k == L"db_table")         g_db_table = v;
            else if (k == L"db_key")           g_db_key = v;
            else if (k == L"db_val")           g_db_val = v;
            else if (k == L"db_refresh_seconds") g_db_refresh_seconds = _wtoi(v.c_str());
            // 没有 ArkShop 的服也能用 —— 允许插件自建库表并自动建档
            else if (k == L"auto_create")      g_db_auto_create = !(v == L"0" || v == L"false" || v == L"no");
            else if (k == L"auto_insert")      g_db_auto_insert = !(v == L"0" || v == L"false" || v == L"no");
        });
        if (g_shop_tier_coef[0] <= 0.0) g_shop_tier_coef[0] = 1.0;
        if (g_shop_tier_coef[1] <= 0.0) g_shop_tier_coef[1] = 1.5;
        if (g_shop_tier_coef[2] <= 0.0) g_shop_tier_coef[2] = 2.0;
        if (g_shop_points_cmd_seconds < 5) g_shop_points_cmd_seconds = 5;
        if (g_shop_push_seconds < 1) g_shop_push_seconds = 1;
        if (g_shop_join_tip_delay < 0)   g_shop_join_tip_delay = 0;
        if (g_shop_join_tip_delay > 120) g_shop_join_tip_delay = 120;
        // ui_key 必须落在 F1~F12 —— 客户端 mod 里只挂了这 12 颗 InputKey 节点
        //   配了别的键（例如 P）时 mod 永远收不到 ⇒ 面板打不开，而且**没有任何报错**
        //   ⇒ 开机就喊一声（轮盘那条入口不依赖按键，照样能开面板）
        {
            bool fkey = false;
            if (g_shop_ui_key.size() >= 2 && (g_shop_ui_key[0] == L'F' || g_shop_ui_key[0] == L'f'))
            {
                const int fn = _wtoi(g_shop_ui_key.c_str() + 1);
                fkey = (fn >= 1 && fn <= 12);
            }
            if (!fkey)
                Log::GetLog()->warn("[ZeroARKTrait] ui_key={} is outside F1~F12: the client mod only binds F1~F12 (the wheel entry can still open the panel)",
                                    AsaApi::Tools::Utf8Encode(g_shop_ui_key));
        }
        Log::GetLog()->info("[ZeroARKTrait] shop settings: tier={}/{}/{} points_mode={} cmd_sec={} push_sec={} ui_key={}",
                            g_shop_tier_coef[0], g_shop_tier_coef[1], g_shop_tier_coef[2],
                            AsaApi::Tools::Utf8Encode(g_shop_points_mode), g_shop_points_cmd_seconds,
                            g_shop_push_seconds, AsaApi::Tools::Utf8Encode(g_shop_ui_key));
        // 等级系数必须还是 1/1.5/2 —— 面板那边是硬编的 `(级别+1)/2`
        //   （tier1/2/3 改了而面板没跟着改 ⇒ 面板价与实际扣费必然对不上 ⇒ 开机就喊）
        if (fabs(g_shop_tier_coef[0] - 1.0) > 0.001 ||
            fabs(g_shop_tier_coef[1] - 1.5) > 0.001 ||
            fabs(g_shop_tier_coef[2] - 2.0) > 0.001)
            Log::GetLog()->warn("[ZeroARKTrait] tier coefs are {}/{}/{} but the panel hardcodes (level+1)/2 = 1/1.5/2"
                                " -> the panel price would drift from the real charge; change both together",
                                g_shop_tier_coef[0], g_shop_tier_coef[1], g_shop_tier_coef[2]);
    }

    void LoadShopConfig()
    {
        LoadShopPriceTable();
        LoadShopSpeciesTable();
        LoadShopSettings();
        LoadDiscountTable();       // 权限组折扣表（在 settings 之后，日志里能带上 mode）
        LoadPermissionsApi();      // 官方 Permissions 插件（动态加载，没装也能跑）
        LoadMessages();            // 玩家消息（按 msg_lang 选语言文件）
        // `trait_lang=auto`（留空也认）⇒ 词条名的显示语言跟着 msg_lang 走
        //   为什么：trait_lang 和 msg_lang 是两个独立开关 ⇒ 极易出现"界面英文、词条名中文"的混排
        //   （2026-09-27 真机实测抓到）⇒ 默认一个语言开关管全部，想单独指定再显式填 zh/en
        if (g_shop_trait_lang == L"auto" || g_shop_trait_lang == L"msg" || g_shop_trait_lang.empty())
            g_shop_trait_lang = g_msg_lang;
        Log::GetLog()->info("[ZeroARKTrait] lang: msg={} trait={} (trait_lang=auto 时跟随 msg_lang)",
                            AsaApi::Tools::Utf8Encode(g_msg_lang), AsaApi::Tools::Utf8Encode(g_shop_trait_lang));
        if (g_shop_points_mode == L"file") LoadFilePoints();   // 本地点数文件（零依赖）
        Log::GetLog()->info("[ZeroARKTrait] charge_mode={} points_mode={}",
                            AsaApi::Tools::Utf8Encode(g_charge_mode), AsaApi::Tools::Utf8Encode(g_shop_points_mode));
    }

    // 目标 token：玩家名  或  id:<id1>[:<id2>]
    void ParseTargetToken(const std::wstring& tok, Job& j)
    {
        if (tok.size() > 3 && _wcsnicmp(tok.c_str(), L"id:", 3) == 0)
        {
            const std::wstring rest = tok.substr(3);
            const size_t colon = rest.find(L':');
            j.by_id = true;
            if (colon == std::wstring::npos)
            {
                j.id1 = (unsigned int)_wtoi64(rest.c_str());
                j.id2 = 0;
            }
            else
            {
                j.id1 = (unsigned int)_wtoi64(rest.substr(0, colon).c_str());
                j.id2 = (unsigned int)_wtoi64(rest.substr(colon + 1).c_str());
            }
            return;
        }
        j.by_id = false;
        j.by_name = false;
        if (tok.size() > 5 && _wcsnicmp(tok.c_str(), L"name:", 5) == 0)
        {
            j.by_name = true;
            j.name_sub = tok.substr(5);
            return;
        }
        j.player = tok;
    }

    // 所有在线玩家的位置（用于按 DinoID 搜索）
    void CollectPlayerLocs(std::vector<FVector>& out)
    {
        AShooterGameState* gs = AsaApi::GetApiUtils().GetGameState();
        if (gs == nullptr) return;
        TArray<TObjectPtr<APlayerState>>& arr = gs->PlayerArrayField();
        for (int i = 0; i < arr.Num(); ++i)
        {
            APlayerState* ps = arr[i];
            if (ps == nullptr) continue;
            AShooterPlayerState* sps = static_cast<AShooterPlayerState*>(ps);
            AShooterPlayerController* pc = sps->GetShooterController();
            if (pc == nullptr) continue;
            out.push_back(pc->CurrentPlayerCharacterLocationField());
        }
    }

    // （按 DinoID / 名字的搜索改走 SearchState 分片推进，见下方 AdvanceSearch）

    std::wstring DinoNameOnly(APrimalDinoCharacter* d)
    {
        FString tamed = d->TamedNameField();
        std::wstring name = std::wstring(*tamed);
        if (name.empty())
        {
            FString desc = d->DescriptiveNameField();
            name = std::wstring(*desc);
        }
        return name;
    }

    // 逗号/中文逗号分隔
    void SplitComma(const std::wstring& s, std::vector<std::wstring>& out)
    {
        out.clear();
        std::wstring cur;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const wchar_t c = s[i];
            if (c == L',' || c == L'，' || c == L'、' || c == L'|')
            {
                const std::wstring t = Trim(cur);
                if (!t.empty()) out.push_back(t);
                cur.clear();
            }
            else cur += c;
        }
        const std::wstring t = Trim(cur);
        if (!t.empty()) out.push_back(t);
    }

    // 大小写不敏感的子串匹配（用于按名字找龙）
    bool ContainsI(const std::wstring& hay, const std::wstring& needle)
    {
        if (needle.empty() || hay.size() < needle.size()) return false;
        for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        {
            if (_wcsnicmp(hay.c_str() + i, needle.c_str(), needle.size()) == 0) return true;
        }
        return false;
    }

    std::wstring DinoLabel(APrimalDinoCharacter* d)
    {
        std::wstring name = DinoNameOnly(d);
        if (name.empty()) name = L"(无名)";
        return name + L"  DinoID1=" + std::to_wstring(d->DinoID1Field()) +
               L" DinoID2=" + std::to_wstring(d->DinoID2Field());
    }

    // ------------------------------------------- v1.11.0 附近已驯服龙（搜索 / 显示 / 选定）
    // 服主需求（2026-09-18）：「玩家可以搜索已驯服的附近龙，会显示有多远、ID、玩家自设的名称啥的，
    //   一些龙的信息，然后可以选择龙」。落地：`trait 龙` 列出来（距离/等级/性别/幼体/词条数/归属/
    //   DinoID/名字），`trait 选 <序号>` 选定；选定后 `trait 1`(查词条) 与 `trait 买`(下单算价) 都针对它。
    //   同时提供 `#ZS|DINO|…` 结构化行（`trait 龙 zs` / RCON 带 zs）给将来的客户端 mod 渲染面板。
    struct DinoRef
    {
        unsigned int id1 = 0;
        unsigned int id2 = 0;
        std::wstring name;      // 玩家自设名字（没设则用物种描述名）
        std::wstring species;   // 物种名（英文，机器人按它算身价）
        float dist = 0.f;       // cm
        int level = 0;
        bool female = false;
        bool baby = false;
        int traits = 0;
        bool mine = false;      // 与自己同队（自己/部落）
    };

    std::map<std::wstring, std::vector<DinoRef>> g_last_list;   // 玩家名 → 上次列出的一批（供"选 <序号>"）
    std::map<std::wstring, DinoRef>             g_sel;          // 玩家名 → 当前选定

    // 物种名：**优先 DescriptiveName（完整物种名，如 Ankylosaurus）** —— 机器人拿它去匹配身价表；
    // DinoNameTag 只是缩写（Anky），靠子串匹配碰运气 ⇒ 只当兜底。（玩家自设名字走 TamedName，两码事）
    std::wstring DinoSpecies(APrimalDinoCharacter* d)
    {
        if (d == nullptr) return L"";
        FString desc = d->DescriptiveNameField();
        std::wstring s = std::wstring(*desc);
        if (!s.empty()) return s;
        FString tag = d->DinoNameTagField().ToString();
        return std::wstring(*tag);
    }

    int DinoLevelOf(APrimalDinoCharacter* d)
    {
        if (d == nullptr) return 0;
        UPrimalCharacterStatusComponent* st = d->MyCharacterStatusComponentField();
        if (st == nullptr) return 0;
        return st->BaseCharacterLevelField() + (int)st->ExtraCharacterLevelField();
    }

    // 已驯服 = TamingTeamID != 0（野生为 0；SpawnDino(force_tame) 就是给它写这个字段）
    bool IsTamedDino(APrimalDinoCharacter* d)
    {
        return d != nullptr && d->TamingTeamIDField() != 0;
    }

    std::vector<DinoRef> CollectTamedNear(AShooterPlayerController* pc, float radius, size_t max_n, int* total)
    {
        std::vector<DinoRef> out;
        if (total != nullptr) *total = 0;
        if (pc == nullptr) return out;

        //  2026-09-22 修距离 bug：原来用 CurrentPlayerCharacterLocationField()，实测不可靠
        //   （C++ 里直接读位置常拿到"相对坐标 / 没更新的值" ⇒ 距离差几十倍）。
        //   改从角色的 RootComponent 的 ComponentToWorld 取世界位置——
        //   官方扩展 ExtensionsDefinition.h:31 也是这么用的。
        FVector loc = pc->CurrentPlayerCharacterLocationField();
        if (AShooterCharacter* mine_ch = pc->GetPlayerCharacter())
        {
            if (USceneComponent* mroot = mine_ch->RootComponentField())
            {
                loc = mroot->ComponentToWorldField().GetLocation();
            }
        }
        const int my_team = pc->TargetingTeamField();
        TArray<AActor*> actors = AsaApi::GetApiUtils().GetAllActorsInRange(
            loc, radius, EServerOctreeGroup::Type::DINOPAWNS);

        std::vector<DinoRef> all;
        for (int i = 0; i < actors.Num(); ++i)
        {
            AActor* a = actors[i];
            if (a == nullptr) continue;
            APrimalDinoCharacter* d = static_cast<APrimalDinoCharacter*>(a);
            if (!IsTamedDino(d)) continue;

            USceneComponent* root = d->RootComponentField();
            if (root == nullptr) continue;
            //  2026-09-22：原来是 RelativeLocationField()（相对坐标）⇒ 距离错得离谱。
            //   改用 ComponentToWorld 的世界位置（与玩家位置同一坐标系）。
            const FVector p = root->ComponentToWorldField().GetLocation();
            const double dx = p.X - loc.X, dy = p.Y - loc.Y, dz = p.Z - loc.Z;

            DinoRef r;
            r.id1 = d->DinoID1Field();
            r.id2 = d->DinoID2Field();
            FString tn = d->TamedNameField();
            r.name = std::wstring(*tn);
            r.species = DinoSpecies(d);
            if (r.name.empty()) r.name = r.species;
            r.dist = (float)sqrt(dx * dx + dy * dy + dz * dz);
            r.level = DinoLevelOf(d);
            r.female = d->bIsFemale().Get();
            r.baby = d->bIsBaby().Get();
            r.traits = d->GeneTraitsField().Num();
            r.mine = (d->TargetingTeamField() == my_team);
            all.push_back(r);
        }
        if (total != nullptr) *total = (int)all.size();

        std::sort(all.begin(), all.end(),
                  [](const DinoRef& x, const DinoRef& y) { return x.dist < y.dist; });
        for (size_t i = 0; i < all.size() && out.size() < max_n; ++i) out.push_back(all[i]);
        return out;
    }

    std::wstring SelectedText(const std::wstring& player)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const std::map<std::wstring, DinoRef>::iterator it = g_sel.find(player);
        if (it == g_sel.end()) return L"（无）";
        return it->second.name + L" [" + it->second.species + L"] id:" +
               std::to_wstring(it->second.id1) + L":" + std::to_wstring(it->second.id2);
    }

    bool GetSelection(const std::wstring& player, DinoRef& out)
    {
        std::lock_guard<std::mutex> lock(g_mu);
        const std::map<std::wstring, DinoRef>::iterator it = g_sel.find(player);
        if (it == g_sel.end()) return false;
        out = it->second;
        return true;
    }

    std::wstring DinoListText(const std::wstring& player, const std::vector<DinoRef>& list,
                              int total, float radius)
    {
        std::wstring s = L"附近已驯服的龙（半径 " + std::to_wstring((int)radius) + L"cm 共 " +
                         std::to_wstring(total) + L" 只，列前 " + std::to_wstring(list.size()) + L" 只）：";
        for (size_t i = 0; i < list.size(); ++i)
        {
            const DinoRef& r = list[i];
            s += L"\n" + std::to_wstring(i + 1) + L") " + r.name + L" [" + r.species + L"] Lv" +
                 std::to_wstring(r.level) + (r.female ? L" ♀" : L" ♂") + (r.baby ? L" 幼体" : L"") +
                 L" " + std::to_wstring((long long)(r.dist / 100.f)) + L"m  词条" +
                 std::to_wstring(r.traits) + (r.mine ? L"  自己的" : L"  别人的") +
                 L"  id:" + std::to_wstring(r.id1) + L":" + std::to_wstring(r.id2);
        }
        if (list.empty())
            s += L"\n（半径内没有已驯服的龙 —— 把龙带到身边，或让服主调大 list_radius，现在 "
                 + std::to_wstring((int)radius) + L"cm）";
        s += L"\n选定：trait 选 <序号>   也可以 trait 选 id:<DinoID1>:<DinoID2>   取消：trait 选 0";
        s += L"\n当前已选：" + SelectedText(player);
        return s;
    }

    // 列表发给玩家：人类可读正文；structured=true 时**只**发 `#ZS|DINO|…` 结构化行（给客户端 mod）
    void SendDinoList(AShooterPlayerController* pc, const std::wstring& player, float radius,
                      bool store, bool structured)
    {
        if (pc == nullptr) return;
        int total = 0;
        std::vector<DinoRef> list = CollectTamedNear(pc, radius, (size_t)g_cfg.list_max, &total);
        if (store)
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_last_list[player] = list;
        }
        if (!structured)
        {
            Say(pc, DinoListText(player, list, total, radius));
            return;
        }
        Say(pc, L"#ZS|DINOLIST|" + std::to_wstring(total) + L"|" + std::to_wstring(list.size()) + L"|" +
                std::to_wstring((int)radius));
        for (size_t i = 0; i < list.size(); ++i)
        {
            const DinoRef& r = list[i];
            Say(pc, L"#ZS|DINO|" + std::to_wstring(i + 1) + L"|" + std::to_wstring(r.id1) + L"|" +
                    std::to_wstring(r.id2) + L"|" + std::to_wstring((long long)(r.dist / 100.f)) + L"|Lv" +
                    std::to_wstring(r.level) + L"|" + (r.female ? L"F" : L"M") + L"|" +
                    (r.baby ? L"1" : L"0") + L"|" + std::to_wstring(r.traits) + L"|" +
                    (r.mine ? L"1" : L"0") + L"|" + r.species + L"|" + r.name);
        }
    }

    // ------------------------------------------------------------------ execute
    // 把任务作用到具体某只龙上（搜寻到目标后调用）
    void ApplyJobToDino(Job& j, APrimalDinoCharacter* dino, AShooterPlayerController* pc)
    {
        UGeneTraitDefinitions* defs = TraitDefs();
        if (defs == nullptr)
        {
            Say(pc, Msg(L"no_gamedata"));
            return;
        }

        if (j.kind == 3)   // 列词条
        {
            TArray<FName>& traits = dino->GeneTraitsField();
            const int max_allowed = defs->GeneTraits_GetMaxAllowedTraitsForThisCreature(dino);
            std::wstring s = L"目标恐龙：" + DinoLabel(dino) + L"  词条 " + std::to_wstring(traits.Num()) +
                             L" 条 / 上限 " + std::to_wstring(max_allowed) + L" 条：";
            for (int i = 0; i < traits.Num(); ++i)
            {
                s += L"[" + DisplayTraitName(TraitNameOf(traits[i])) + L"] ";
            }
            if (traits.Num() == 0) s += L"（无）";
            Say(pc, s);
            return;
        }

        FString fail;
        const std::string trait_a = AsaApi::Tools::Utf8Encode(j.trait);   // FName(const char*) 才是 inline 构造
        const std::wstring order_tag = L" | 订单=" + (j.order.empty() ? std::wstring(L"-") : j.order);
        if (j.kind == 1)
        {
            const bool ok = defs->GeneTraits_AddInstanceOfGeneTraitIfPossible(
                dino, FName(trait_a.c_str()), j.tier, &fail);
            const std::wstring detail = L"目标=" + DinoLabel(dino) + L" | 词条=" + j.trait +
                                        L"[" + std::to_wstring(j.tier) + L"]" + order_tag +
                                        L" | 结果=" + (ok ? L"成功" : (L"失败:" + std::wstring(*fail)));
            AuditLog(L"加词条 | " + detail);
            if (ok) Say(pc, Msg(L"add_ok", DinoLabel(dino), DisplayTraitName(j.trait),
                                std::to_wstring(j.tier + 1),
                                j.order.empty() ? std::wstring(L"") : (L"  订单号 " + j.order)));
            else
            {
                // 引擎拒绝（如已满 stack limit）⇒ 把刚扣的点退回去
                const bool refunded = RefundJobIfCharged(j, std::wstring(*fail));
                const std::wstring tail =
                    (refunded ? (L"  （已退回 " + std::to_wstring(j.cost) + L" 点）") : std::wstring(L"")) + order_tag;
                Say(pc, Msg(L"add_fail", std::wstring(*fail), tail));
            }
        }
        else if (j.kind == 2)
        {
            // 删除要能吃下"带层级"的名字：存储里的形态是 `Vampiric[2]`
            // 策略：①直接按给定名字删 ②按 词条[0..2] 三种形态试 ③遍历龙身上的实际名字，按基名匹配后用**原样名字**删
            bool ok = defs->GeneTraits_RemoveInstanceOfGeneTraitIfItExists(
                dino, FName(trait_a.c_str()), &fail);
            std::wstring used = j.trait;
            if (!ok)
            {
                for (int tr = 0; tr <= 2 && !ok; ++tr)
                {
                    const std::string n2 = AsaApi::Tools::Utf8Encode(
                        j.trait + L"[" + std::to_wstring(tr) + L"]");
                    FString f2;
                    ok = defs->GeneTraits_RemoveInstanceOfGeneTraitIfItExists(
                        dino, FName(n2.c_str()), &f2);
                    if (ok) used = j.trait + L"[" + std::to_wstring(tr) + L"]";
                    else    fail = f2;
                }
            }
            if (!ok)
            {
                TArray<FName>& traits = dino->GeneTraitsField();
                for (int i = 0; i < traits.Num() && !ok; ++i)
                {
                    const std::wstring full = TraitNameOf(traits[i]);
                    if (full.compare(0, j.trait.size(), j.trait) == 0)   // 基名匹配
                    {
                        const std::string full_a = AsaApi::Tools::Utf8Encode(full);
                        FString f3;
                        ok = defs->GeneTraits_RemoveInstanceOfGeneTraitIfItExists(
                            dino, FName(full_a.c_str()), &f3);
                        if (ok) used = full;
                        else    fail = f3;
                    }
                }
            }

            const std::wstring detail = L"目标=" + DinoLabel(dino) + L" | 词条=" + j.trait +
                                        L"（实删=" + used + L"）" + order_tag +
                                        L" | 结果=" + (ok ? std::wstring(L"成功") : (L"失败:" + std::wstring(*fail)));
            AuditLog(L"删词条 | " + detail);
            if (ok) Say(pc, Msg(L"del_ok", DinoLabel(dino), DisplayTraitName(used)));
            else    Say(pc, Msg(L"del_fail", std::wstring(*fail)));
        }
        else if (j.kind == 6)   // 词条礼包：一次给同一只龙加多条
        {
            int ok_cnt = 0;
            std::wstring detail = L"目标=" + DinoLabel(dino) +
                                  L" | 词条数=" + std::to_wstring(j.bundle.size()) + order_tag;
            for (size_t i = 0; i < j.bundle.size(); ++i)
            {
                FString f2;
                const std::string ta = AsaApi::Tools::Utf8Encode(j.bundle[i].first);
                const bool ok = defs->GeneTraits_AddInstanceOfGeneTraitIfPossible(
                    dino, FName(ta.c_str()), j.bundle[i].second, &f2);
                if (ok) ++ok_cnt;
                detail += L" " + DisplayTraitName(j.bundle[i].first) +
                          L"[" + std::to_wstring(j.bundle[i].second) + L"]=" +
                          (ok ? std::wstring(L"ok") : (L"fail:" + std::wstring(*f2)));
            }
            detail += L" | 成功 " + std::to_wstring(ok_cnt) + L"/" + std::to_wstring(j.bundle.size());
            AuditLog(L"礼包 | " + detail);
            Say(pc, Msg(L"bundle_done", std::to_wstring(ok_cnt), std::to_wstring(j.bundle.size()), DinoLabel(dino)));
        }

        //  v1.23.2（C）：这一单已经送到头（成功/失败上面都处理过）⇒ 放掉"连点计数"这道门
        DecrementPendingCharge(j);
    }

    // 立刻可完成的：目标用玩家名（取该玩家最近的龙，代价小） / 或查单（不需要目标）
    // ------------------------------------------------- v1.9.7 「面板」：客户端对话框（F2 面板的地基）
    //  为什么必须走客户端对话框：插件跑在**服务器**进程里，服务器自己 new 的 UMG widget 只会活在
    //   服务器进程里（那儿没有玩家的屏幕）⇒ 想上玩家的屏，只能调 `Client*` 这类 RPC（在客户端执行）。
    //   `ClientDisplayNotificationDialog(Message, Title, …)` = 游戏自带的客户端对话框 先拿它当地基。
    //  关于 F2：**服务器看不到远程客户端的按键**（按键状态不上传；SDK 里 FKey 还是不透明结构，
    //   连构造一个 "F2" 都做不到）⇒ 本版入口 = 聊天 `trait 5`（或控制台/RCON `Trait 面板 <玩家>`）。
    // v1.9.8：面板标题/正文（机器人可用 `Trait 面板文本` 推送覆盖；空则用内置示例文案）
    std::wstring g_panel_title = L"词条商店";
    std::wstring g_panel_body;

    void ShowTraitPanel(AShooterPlayerController* pc, bool auto_close = false)
    {
        if (pc == nullptr) return;
        const std::wstring who = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc));
        const FString title(g_panel_title.c_str());
        const std::wstring sample =
            L"这一行是**服务器推到你屏幕上的客户端对话框** （面板地基已通）\n"
            L"\n"
            L"· 嗜血(Vampiric) T2 @ 霸王龙 ≈ 3024 点\n"
            L"· 突变-近战 T2 @ 南方巨兽龙 ≈ 13200 点\n"
            L"· 完整价目：机器人里发 /词条价（例：/词条价 霸王龙）\n"
            L"\n"
            L"下一步：机器人可推整张价目到这里（Trait 面板文本）；点选下单需要客户端侧能力（待定）";
        const std::wstring bodyw = g_panel_body.empty() ? sample : g_panel_body;
        const FString msg(bodyw.c_str());
        Log::GetLog()->info("[ZeroARKTrait] 面板 → 玩家 [{}]：发客户端对话框（bAutoClose={}）",
                            AsaApi::Tools::Utf8Encode(who), auto_close ? 1 : 0);
        // 参数顺序：(Message, Title, bIsError, OnOkGoToMainMenu, bAutoClose, bCanBeTop, bHideXBoxFooter)
        pc->ClientDisplayNotificationDialog(&msg, &title, false, false, auto_close, true, false);
        AsaApi::GetApiUtils().SendServerMessage(pc, FLinearColor(0.25f, 0.9f, 1.f, 1.f),
            L"[ZeroARKTrait] 面板已发送（没弹窗就把这条反馈给服主）");
        AuditLog(L"面板 | 玩家=" + who);
    }

    void RunJobNow(Job& j)
    {
        if (j.kind == 7)   // 查单：直接回审计内容
        {
            Say(nullptr, QueryAuditByOrder(j.order));
            return;
        }

        if (j.kind == 11)   // v1.9.2 无敌（走 cheat manager 的 CheatAction）
        {
            AShooterPlayerController* pc_g = FindPlayer(j.player);
            if (pc_g == nullptr) { Say(nullptr, Msg(L"no_player_found", j.player)); return; }
            UShooterCheatManager* cm = AsaApi::GetApiUtils().GetCheatManagerByPC(pc_g);
            if (cm == nullptr) { Say(pc_g, Msg(L"no_cheatmanager")); return; }
            FString g1(L"God");            FString g2(L"InfiniteStats");
            cm->CheatAction(&g1);
            cm->CheatAction(&g2);
            Say(pc_g, Msg(L"cheat_on"));
            AuditLog(L"无敌 | 玩家=" + j.player + L" | 已下发 God+InfiniteStats（无返回值）");
            return;
        }

        if (j.kind == 13)  // v1.11.0 附近已驯服龙列表（发到该玩家屏幕；tier!=0 ⇒ 只发结构化行）
        {
            AShooterPlayerController* pc_l = FindPlayer(j.player);
            if (pc_l == nullptr) { Say(nullptr, Msg(L"no_player_found", j.player)); return; }
            const std::wstring who = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc_l));
            const float rad = (j.radius > 0.f) ? j.radius : g_cfg.list_radius;
            SendDinoList(pc_l, who, rad, true, j.tier != 0);
            AuditLog(L"龙列表 | 玩家=" + j.player + L" | 半径=" + std::to_wstring((int)rad));
            return;
        }

        if (j.kind == 12)  // v1.9.7 面板：给该玩家弹客户端对话框（tier!=0 ⇒ bAutoClose 自动关）
        {
            AShooterPlayerController* pc_p = FindPlayer(j.player);
            if (pc_p == nullptr) { Say(nullptr, Msg(L"no_player_found", j.player)); return; }
            ShowTraitPanel(pc_p, j.tier != 0);
            return;
        }

        const std::wstring order_tag = L" | 订单=" + (j.order.empty() ? std::wstring(L"-") : j.order);
        const std::wstring what = (j.kind == 2) ? L"删词条" : ((j.kind == 6) ? L"礼包" : L"加词条");
        const std::wstring trait_part = (j.kind == 6)
            ? (L" | 词条数=" + std::to_wstring(j.bundle.size()) + order_tag)
            : (L" | 词条=" + j.trait + L"[" + std::to_wstring(j.tier) + L"]" + order_tag);

        //  v1.23.2（A）：按 DinoID 的购买单 —— 先"在这些 actor 里直找"，找不到才回退分片搜索
        //   为什么必须这样：面板下单带的是玩家选定的那只龙的 ID，而老代码只把 id1/id2 塞进任务、
        //   没置 by_id/by_name ⇒ 走到下面 NearestDino ⇒ 词条加到离玩家最近的那只
        //   （2026-09-26 19:17-19:18 审计里两只龙名不同 = 铁证）
        if (j.kind == 1 && j.id1 != 0)
        {
            APrimalDinoCharacter* pd = FindDinoById(j.id1, j.id2);
            if (pd != nullptr)
            {
                ApplyJobToDino(j, pd, FindPlayer(j.player));
                return;
            }
            StartSearch(j);   // 不在当前 actor 列表里 ⇒ 交给分片搜索（按 DinoID 扫）
            return;
        }

        AShooterPlayerController* pc = FindPlayer(j.player);
        if (pc == nullptr)
        {
            AuditLog(what + L" | 目标玩家=" + j.player + trait_part + L" | 结果=失败:玩家不在线");
            RefundJobIfCharged(j, L"玩家不在线");   // 扣过的点退回去
            DecrementPendingCharge(j);               //  v1.23.2（C）：连点计数也放掉
            // 人不在线 ⇒ 把"失败 + 原因"排进待发队列，他一上线立刻推给他
            NotifyLater(j.player, Msg(L"notify_offline",
                        j.order.empty() ? std::wstring(L"-") : j.order,
                        j.trait.empty() ? std::wstring(L"-") : DisplayTraitName(j.trait)));
            // v1.9.2：**订单（机器人下单）不再全服喊** —— 找不到人就没人可通知，
            // 结果已经写进审计（机器人会 `Trait 查单` 回查并告诉玩家/退点）。
            // 商店消息只该进游戏里的个人；手动/控制台加的仍保留原提示。
            if (j.order.empty()) Say(nullptr, Msg(L"no_player_found", j.player));
            return;
        }
        APrimalDinoCharacter* dino = NearestDino(pc, g_cfg.near_radius);
        if (dino == nullptr)
        {
            AuditLog(what + L" | 目标玩家=" + j.player + trait_part + L" | 结果=失败:附近无恐龙");
            RefundJobIfCharged(j, L"附近无恐龙");   // 扣过的点退回去
            DecrementPendingCharge(j);               //  v1.23.2（C）
            Say(pc, Msg(L"no_dino_near", std::to_wstring((int)g_cfg.near_radius)));
            return;
        }
        ApplyJobToDino(j, dino, pc);
    }

    // 按 DinoID / 名字搜索：**分片推进**，每 Tick 最多查 scan_budget 个 actor（保护服务器）
    void StartSearch(const Job& j)
    {
        g_search = SearchState();
        g_search.active = true;
        g_search.job = j;
        g_search.by_name = j.by_name;
        g_search.name = j.name_sub;
        g_search.started = GetTickCount64();
        g_search.last_report = g_search.started;
    }

    void AdvanceSearch()
    {
        SearchState& s = g_search;
        if (s.locs.empty()) CollectPlayerLocs(s.locs);

        int budget = g_cfg.scan_budget;
        while (budget > 0)
        {
            if (s.player_idx >= (int)s.locs.size())
            {
                const unsigned long long ms = GetTickCount64() - s.started;
                const std::wstring who = s.by_name ? s.name
                    : (std::to_wstring(s.job.id1) + L":" + std::to_wstring(s.job.id2));
                const std::wstring miss = Msg(L"search_miss", who, std::to_wstring(s.scanned),
                                              std::to_wstring((int)(ms / 1000)),
                                              std::to_wstring((int)g_cfg.id_search_radius));
                AShooterPlayerController* pc_m = FindPlayer(s.job.player);
                if (pc_m != nullptr) Say(pc_m, miss); else NotifyLater(s.job.player, miss);
                // 搜索没命中 = 这单发不出去 ⇒ 已扣的点必须退（原来不退 = 白扣）
                RefundJobIfCharged(s.job, L"按 DinoID 没找到那只龙");
                DecrementPendingCharge(s.job);
                AuditLog(L"搜索未命中 | " +
                         std::wstring(s.by_name ? L"按名字=" : L"按DinoID=") +
                         (s.by_name ? s.name : (std::to_wstring(s.job.id1) + L":" + std::to_wstring(s.job.id2))) +
                         L" | 已扫=" + std::to_wstring(s.scanned) + L" | 耗时秒=" + std::to_wstring((int)(ms / 1000)));
                s.active = false;
                return;
            }

            s.actors = AsaApi::GetApiUtils().GetAllActorsInRange(
                s.locs[s.player_idx], g_cfg.id_search_radius, EServerOctreeGroup::Type::DINOPAWNS);

            for (; s.actor_cursor < s.actors.Num() && budget > 0; ++s.actor_cursor, --budget, ++s.scanned)
            {
                AActor* a = s.actors[s.actor_cursor];
                if (a == nullptr) continue;
                APrimalDinoCharacter* d = static_cast<APrimalDinoCharacter*>(a);

                bool hit = false;
                if (s.by_name)
                {
                    const std::wstring nm = DinoNameOnly(d);
                    hit = !s.name.empty() && ContainsI(nm, s.name);
                }
                else
                {
                    hit = (d->DinoID1Field() == s.job.id1) &&
                          (s.job.id2 == 0 || d->DinoID2Field() == s.job.id2);
                }

                if (hit)
                {
                    // 按 DinoID 找到的龙也要把结果回给玩家（原来传 nullptr ⇒ 玩家看不到结果）
                    ApplyJobToDino(s.job, d, FindPlayer(s.job.player));
                    const unsigned long long ms = GetTickCount64() - s.started;
                    Log::GetLog()->info("[ZeroARKTrait] search hit after scanning {} actors in {} ms",
                                        s.scanned, (int)ms);
                    s.active = false;
                    return;
                }
            }

            if (s.actor_cursor >= s.actors.Num())
            {
                ++s.player_idx;
                s.actor_cursor = 0;
                s.actors = TArray<AActor*>();
            }

            // 进度回执（默认每 5 秒一条，写日志 + 发起者若有玩家则提示他）
            const unsigned long long now = GetTickCount64();
            if (now - s.last_report >= 5000)
            {
                s.last_report = now;
                const std::wstring prog = L"搜索进行中：已扫 " + std::to_wstring(s.scanned) +
                                          L" 只（玩家 " + std::to_wstring(s.player_idx + 1) + L"/" +
                                          std::to_wstring((int)s.locs.size()) + L"）";
                Log::GetLog()->info("[ZeroARKTrait] {}", AsaApi::Tools::Utf8Encode(prog));
                AShooterPlayerController* who = FindPlayer(s.job.player);
                if (who != nullptr) Say(who, prog);
            }
        }
        // 预算用完：本 Tick 到此为止，下个 Tick 接着扫（进度记在 g_search 里）
    }

    // ---- v1.12.0 M4b 全局缓存（必须先声明：下面的函数要用）----
    static std::wstring g_shop_price_text;    // 价目表全文（分块拼接）
    static bool         g_shop_dirty = false; // 有新数据待下发

    // ---- v1.13.0 M4c：按玩家下发的点数表（机器人推"在线玩家余额"，键=角色名）----
    static std::map<std::wstring, int> g_player_points;
    // 按 EOS ID 存的点数表（比角色名可靠 —— ListPlayers 第二列就是它，push 时能直接拿到）
    static std::map<std::wstring, int> g_player_points_eos;
    // 本次服务器运行里"玩家 → buff 处理状态"（1=已摘旧 buff 待重挂 / 2=已处理）
    //   目的：每次玩家重新进来都摘掉旧 buff 再挂新的⇒ 客户端拿到刷新过的副本 ⇒ 不用重开游戏
    static std::map<std::wstring, int> g_join_state;
    // 每个玩家上一次推过去的载荷—— 数据没变就别再推
    //   原因：面板每次收到推送都会往控件里写（`||UI|` 那段）⇒ 会把玩家正在输入的搜索框内容刷掉
    static std::map<std::wstring, std::wstring> g_last_pushed;
static std::map<std::wstring, int> g_force_push_left;   // v1.20.9：新会话还要"无视去重"推几次（5 次 ≈ 10 秒就够面板接住）
    // g_force_push_left / g_last_pushed 会被命令回调线程(RCON/控制台) 与
    //   Tick 主线程同时读写 ⇒ 必须加锁（std::map 并发写 = 可能崩服）。
    //   用专用锁 g_push_mu（不与 g_mu 嵌套 ⇒ 无锁顺序问题）
    // 🔒 这把锁的声明已挪到文件开头（g_jobs 旁边）—— 下单预检也要用它，
    //   而预检在 EnqueueBuyReq 里（比这一行靠前）⇒ 只在开头留一处定义

static std::map<std::wstring, unsigned long long> g_reattach_until;   // v1.21.0：进服后"反复摘挂 buff"的窗口截止
static std::map<std::wstring, unsigned long long> g_reattach_last;    // v1.21.0：上次摘挂时刻
    static std::wstring g_shop_miss_name;   // "没查到点数"的日志对每个新名字只打一次，别每 2 秒刷屏

    // 命令：`Trait 缓存点数 <角色名> <点数>`（增量、可反复推）；`Trait 缓存点数 clear` 清空
    void CachePlayerPoints(const std::wstring& tail)
    {
        std::vector<std::wstring> tt;
        Split(Trim(tail), tt);
        if (tt.empty()) return;
        if (tt[0] == L"clear" || tt[0] == L"清空")
        {
            g_player_points.clear();
            Log::GetLog()->info("[ZeroARKTrait] player points table cleared");
            return;
        }
        if (tt.size() < 2) return;
        // 32 位十六进制 = EOS ID ⇒ 存进 EOS 表（按角色名匹配容易失手）
                // 角色名允许带空格 ⇒ 只有最后一段是参数（点数），其余合并成名字
        //   （EOS ID 无空格 ⇒ 行为与旧版完全一致）
        if (tt.size() >= 3 && tt[0] != L"clear" && tt[0] != L"清空")
        {
            std::wstring nm = tt[0];
            for (size_t i = 1; i + 1 < tt.size(); ++i) nm += L" " + tt[i];
            std::vector<std::wstring> nt;
            nt.push_back(nm);
            nt.push_back(tt.back());
            tt.swap(nt);
        }
        const bool isEos = (tt[0].size() == 32 &&
                            tt[0].find_first_not_of(L"0123456789abcdefABCDEF") == std::wstring::npos);
        if (isEos) g_player_points_eos[tt[0]] = _wtoi(tt[1].c_str());
        else       g_player_points[tt[0]] = _wtoi(tt[1].c_str());
        Log::GetLog()->info("[ZeroARKTrait] player points updated ({}) byName={} byEos={}",
                            (isEos ? "eos" : "name"), g_player_points.size(), g_player_points_eos.size());
    }

    // ===== v1.16.0 自给自足的点数：本地点数文件 + 插件自己扣点 =====
    //   用户要求：「没有机器人扣点的方式也要做进配置」
    //   - points_mode=file  ⇒ 插件目录 ZeroARKTrait-points.txt（一行 `EOS=点数` 零第三方依赖）
    //   - charge_mode=plugin ⇒ 玩家下单时插件自己扣（db 走条件 UPDATE / file 改写本地文件）
    //   （g_charge_mode 的声明已提到上方）
    static std::map<std::wstring, int> g_file_points;  // EOS -> 点数（file 模式）
    static bool g_file_points_loaded = false;

    //  文件才是 file 模式的唯一事实来源
    //   旧版只读一次（`g_file_points_loaded` 一置就不再读）⇒ 管理员手改
    //   `ZeroARKTrait-points.txt` 之后插件还在用内存里的旧值 （2026-09-28 验收实测：
    //   copy 回 3444 的备份后 `Trait 查点数` 还是 3393）
    //   ⇒ 现在每次取值都重读文件；`quiet=true` 时不打日志（避免每 2 秒刷屏）
    void LoadFilePoints(bool quiet)
    {
        g_file_points.clear();
        g_file_points_loaded = true;
        LoadUtf8Kv(L"ZeroARKTrait-points.txt", [](const std::wstring& k, const std::wstring& v) {
            if (!k.empty()) g_file_points[k] = _wtoi(v.c_str());
        });
        if (!quiet)
            Log::GetLog()->info("[ZeroARKTrait] file points loaded: {} entries", g_file_points.size());
    }

    void SaveFilePoints()
    {
        // 路径是纯 ASCII（D:\...\ArkApi\Plugins\ZeroARKTrait\...）⇒ 窄路径安全
        const std::string path = AsaApi::Tools::GetCurrentDir() +
                                 "/ArkApi/Plugins/ZeroARKTrait/ZeroARKTrait-points.txt";
        std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
        if (!f) { Log::GetLog()->warn("[ZeroARKTrait] file points SAVE FAILED: {}", path); return; }
        std::string out;
        for (std::map<std::wstring, int>::iterator it = g_file_points.begin(); it != g_file_points.end(); ++it)
            out += AsaApi::Tools::Utf8Encode(it->first) + "=" + std::to_string(it->second) + "\r\n";
        f.write(out.c_str(), (std::streamsize)out.size());
    }

    int FilePointsOf(const std::wstring& eos)
    {
        LoadFilePoints(true);   // 每次重读（文件是唯一事实来源）
        std::map<std::wstring, int>::iterator it = g_file_points.find(eos);
        return (it == g_file_points.end()) ? 0 : it->second;
    }

    bool FilePointsAdd(const std::wstring& eos, int delta, int* nowValue)
    {
        LoadFilePoints(true);   // 先重读再改（防覆盖别人手改的内容）
        const int v = FilePointsOf(eos) + delta;
        if (v < 0) return false;
        g_file_points[eos] = v;
        SaveFilePoints();
        if (nowValue) *nowValue = v;
        return true;
    }

    bool FilePointsErase(const std::wstring& eos)
    {
        LoadFilePoints(true);   // 先重读再删
        const size_t n = g_file_points.erase(eos);
        if (n > 0) SaveFilePoints();
        return n > 0;
    }

    //  唯一权威算价公式（面板显示 / 实际扣费 都用它 ⇒ 不会"看着打折、扣原价"）
    //   实付 = 基础价 × 层级系数(1 / 1.5 / 2) × 物种系数 × (1 - 权限组折扣/100)，四舍五入到整数
    int ComputePrice(const std::wstring& traitCn, int tier, const std::wstring& species, const std::wstring& eos)
    {
        // 真实扣费走带 30 秒缓存的组（够快）；查折扣那条命令走直查的组
        //   两条路都用下面这个同一份实现 ⇒ 不会出现"左边 0% 右边还按 10% 算"
        return ComputePriceWith(traitCn, tier, species, PermGroupsCached(eos));
    }

    //  发货失败必须退费—— charge_mode=plugin 是"先扣费、再把任务排队发货"，
    //   而发货可能被引擎拒绝（例如 `Adding this trait would exceed max stack limit`）、
    //   玩家掉线、或附近没有龙 ⇒ 这几种情况都得原样退回，否则就是白扣钱
    //   （2026-09-24 真机实测：一单成功、一单被引擎拒绝，两单都被扣了 204 点）
    bool RefundJobIfCharged(const Job& j, const std::wstring& reason)
    {
        if (g_charge_mode != L"plugin" || j.cost <= 0 || j.eos.empty()) return false;
        if (g_shop_points_mode != L"file")
        {
            Log::GetLog()->error("[ZeroARKTrait] REFUND NEEDED but points_mode={} order={} +{} —— 需人工处理 ",
                                 AsaApi::Tools::Utf8Encode(g_shop_points_mode),
                                 AsaApi::Tools::Utf8Encode(j.order), j.cost);
            return false;
        }
        int now = 0;
        if (!FilePointsAdd(j.eos, j.cost, &now))
        {
            Log::GetLog()->error("[ZeroARKTrait] REFUND FAILED order={} eos={} +{}",
                                 AsaApi::Tools::Utf8Encode(j.order),
                                 AsaApi::Tools::Utf8Encode(j.eos), j.cost);
            return false;
        }
        AuditLog(L"插件退费 | 订单=" + (j.order.empty() ? std::wstring(L"-") : j.order) +
                 L" | 退回=" + std::to_wstring(j.cost) + L" | 余额=" + std::to_wstring(now) +
                 L" | 原因=" + reason);
        Log::GetLog()->warn("[ZeroARKTrait] refunded {} to {} (order={}, reason={}) => now {}",
                            j.cost, AsaApi::Tools::Utf8Encode(j.eos),
                            AsaApi::Tools::Utf8Encode(j.order),
                            AsaApi::Tools::Utf8Encode(reason), now);
        return true;
    }

    // 按 DinoID 直接找那只龙（客户端面板下单用）
    //   一次购买只扫一遍，购买是低频事件 ⇒ 可以接受（原分片搜索是给反复查询用的）
    APrimalDinoCharacter* FindDinoById(unsigned int id1, unsigned int id2)
    {
        if (id1 == 0) return nullptr;
        UWorld* w = AsaApi::GetApiUtils().GetWorld();
        if (w == nullptr) return nullptr;
        TArray<AActor*> list;
        UGameplayStatics::GetAllActorsOfClass(w, APrimalDinoCharacter::StaticClass(), &list);
        for (int i = 0; i < list.Num(); ++i)
        {
            APrimalDinoCharacter* d = static_cast<APrimalDinoCharacter*>(list[i]);
            if (d == nullptr) continue;
            if (d->DinoID1Field() == id1 && d->DinoID2Field() == id2) return d;
        }
        return nullptr;
    }

    // 物种系数查表 —— 算价和推给面板的表都走它，
    //   保证"面板拿到的系数"与"扣费时用的系数"永远同源
    //   规则同 ComputePriceWith 老逻辑：ContainsI（**第一个命中的键赢**），没命中 = 1.0
    double SpeciesCoefOf(const std::wstring& species)
    {
        if (species.empty()) return 1.0;
        for (size_t i = 0; i < g_shop_species_table.size(); ++i)
            if (ContainsI(species, g_shop_species_table[i].first))
                return g_shop_species_table[i].second;
        return 1.0;
    }

    // 推给面板的"键"要消毒 —— 龙名/物种名里若混进 `|` 或 `=` 会把
    //   整个载荷的分段结构搞坏（玩家的自定龙名可以随便打）
    std::wstring SanitizeShopKey(const std::wstring& s)
    {
        std::wstring o;
        for (size_t i = 0; i < s.size() && o.size() < 40; ++i)
        {
            const wchar_t c = s[i];
            if (c == L'|' || c == L'=' || c == L'\r' || c == L'\n' || c == L'\t') continue;
            o.push_back(c);
        }
        while (!o.empty() && o[0] == L' ') o.erase(o.begin());
        while (!o.empty() && o.back() == L' ') o.pop_back();
        return o;
    }

    int ComputePriceWith(const std::wstring& traitCn, int tier, const std::wstring& species,
                         const std::vector<std::wstring>& groups)
    {
        //  价格表的键可能是中文（用户 ini 里就是 `嗜血=800`），而"下单"那条路
        //   传进来的是内部英文名Vampiric ⇒ 直接 _wcsicmp 永远比不中 ⇒ 误报"这个词条还没定价"
        //   （面板显示那条路本来就走 LangNameOf ⇒ 所以"面板有价、下单说没定价"这种鬼现象）
        //   ⇒ 两边都先过 NormalizeTraitName 归一化成内部英文名再比
        const std::wstring wantEn = NormalizeTraitName(traitCn);
        float base = -1.0f;
        for (size_t i = 0; i < g_shop_price_table.size(); ++i)
            if (_wcsicmp(g_shop_price_table[i].first.c_str(), traitCn.c_str()) == 0 ||
                _wcsicmp(NormalizeTraitName(g_shop_price_table[i].first).c_str(), wantEn.c_str()) == 0)
            { base = g_shop_price_table[i].second; break; }
        if (base < 0.0f) return -1;                       // 没定价 ⇒ 交给调用方按失败处理
        const int ti = (tier < 0) ? 0 : ((tier > 2) ? 2 : tier);
        const double sp = SpeciesCoefOf(species);
        const float disc = DiscountFor(groups);
        //  与面板**同一条算式**（设计口径：面板显示的数 = 实际扣的数）
        //   面板那边算的是：`折后单价(整数, 插件推过去的) × 等级系数 × 物种系数` ⇒ 面板先把
        //   折后单价取整了（价格串里就是整数），所以这里**也必须先取整再乘**，
        //   否则又会出现"面板 72.6 / 实际扣 73"这种 1 点误差 （真机 2026-09-28 定案）
        //   等级系数 = g_shop_tier_coef[ti]，面板用的是 (级别+1)/2 = 1/1.5/2 ⇒ 只要 ini
        //   保持 1/1.5/2，两边就逐点相同 （改了 ini 会警告，见 LoadSettings）
        const int unit = (int)(base * (1.0f - disc / 100.0f) + 0.5f);
        float p = (float)unit * g_shop_tier_coef[ti] * (float)sp;
        if (p < 0.0f) p = 0.0f;
        return (int)(p + 0.5f);
    }

    // 扣点（charge_mode=plugin）：db 用条件 UPDATE⇒ 绝不可能扣成负数；file 改写本地文件
    bool ChargePoints(const std::wstring& eos, int cost, std::wstring* err)
    {
        if (cost <= 0) return true;
        if (eos.empty()) { if (err) *err = L"读不到你的 EOS ID"; return false; }

        if (g_shop_points_mode == L"file")
        {
            if (FilePointsOf(eos) < cost) { if (err) *err = L"点数不足"; return false; }
            int now = 0;
            if (!FilePointsAdd(eos, -cost, &now)) { if (err) *err = L"扣点失败"; return false; }
            Log::GetLog()->info("[ZeroARKTrait] charge(file) eos={} -{} => {}",
                                AsaApi::Tools::Utf8Encode(eos), cost, now);
            return true;
        }

        // db 模式
        minimysql::Conn c;
        std::string e;
        if (!c.Open(Narrow(g_db_host), g_db_port, Narrow(g_db_user), Narrow(g_db_pass), Narrow(g_db_name), e))
        {
            if (err) *err = L"数据库连不上";
            Log::GetLog()->warn("[ZeroARKTrait] charge db connect failed: {}", e);
            return false;
        }
        const std::string tbl = Narrow(g_db_table), kcol = Narrow(g_db_key), vcol = Narrow(g_db_val);
        const std::string key = "'" + Narrow(eos) + "'";
        std::string val;
        if (!c.QueryOne("SELECT " + vcol + " FROM " + tbl + " WHERE " + kcol + " = " + key, val, e))
        {
            if (err) *err = L"查点数失败";
            Log::GetLog()->warn("[ZeroARKTrait] charge db query failed: {}", e);
            return false;
        }
        const int have = atoi(val.c_str());
        if (have < cost) { if (err) *err = L"点数不足（现有 " + std::to_wstring(have) + L"）"; return false; }
        const std::string sql = "UPDATE " + tbl + " SET " + vcol + " = " + vcol + " - " + std::to_string(cost) +
                                " WHERE " + kcol + " = " + key + " AND " + vcol + " >= " + std::to_string(cost);
        if (!c.Exec(sql, e))
        {
            if (err) *err = L"扣点失败";
            Log::GetLog()->warn("[ZeroARKTrait] charge db update failed: {}", e);
            return false;
        }
        Log::GetLog()->info("[ZeroARKTrait] charge(db) eos={} -{} (had {})",
                            AsaApi::Tools::Utf8Encode(eos), cost, have);
        return true;
    }

    // 把价目串开头 `POINTS|<点数>|` 里的点数换成该玩家的余额（找不到就填 0）
    //    只做 FString ← std::wstring 这一向的转换（全文件都是这个方向，安全）
    std::wstring ShopDataForPlayer(const FString& player, const std::wstring& eosId = L"",
                                   AShooterPlayerController* pcForAlias = nullptr)
    {
        // 外部没推价目时 ⇒ 用插件自己读到的价目表（让插件自给自足）
        std::wstring base = g_shop_price_text;
        if (base.empty()) base = BuildShopDataFromOwnTable();
        if (base.compare(0, 7, L"POINTS|") != 0) return base;
        const size_t bar = base.find(L'|', 7);
        if (bar == std::wstring::npos) return base;

        int pts = 0;
        bool got = false;
        // points_mode=file ⇒ 用插件目录里的本地点数文件（零依赖 一行 `EOS=点数`）
        if (g_shop_points_mode == L"file" && !eosId.empty())
        {
            pts = FilePointsOf(eosId);
            got = true;
        }
        // points_mode=db ⇒ 插件自己读数据库（按 EOS ID，带缓存与超时）
        if (!got && g_shop_points_mode == L"db" && !eosId.empty())
        {
            got = FetchPointsFromDb(eosId, pts);
            if (!got) Log::GetLog()->warn("[ZeroARKTrait] db points failed for {}, falling back to push table", AsaApi::Tools::Utf8Encode(eosId));
        }
        // push 模式（默认），或 db 读失败时降级
        if (!got)
        {
            pts = 0;
            // 先按 EOS ID 查（最可靠），查不到再按角色名逐个比
            if (!eosId.empty())
            {
                std::map<std::wstring, int>::const_iterator ie = g_player_points_eos.find(eosId);
                if (ie != g_player_points_eos.end()) { pts = ie->second; got = true; }
            }
            if (!got)
            {
                for (std::map<std::wstring, int>::const_iterator it = g_player_points.begin(); it != g_player_points.end(); ++it)
                {
                    const FString key(it->first.c_str());
                    if (key == player) { pts = it->second; got = true; break; }
                }
            }
            if (!got)
            {
                // 诊断：把插件眼里"玩家的名字 / EOS / 两张表的条数"打出来（每个新名字只报一次）
                const std::wstring wn(*player);
                if (wn != g_shop_miss_name)
                {
                    g_shop_miss_name = wn;
                    Log::GetLog()->warn("[ZeroARKTrait] points NOT found: name=[{}] eos=[{}] byName={} byEos={}",
                                        AsaApi::Tools::Utf8Encode(wn), AsaApi::Tools::Utf8Encode(eosId),
                                        g_player_points.size(), g_player_points_eos.size());
                }
            }
        }
        std::wstring out = base.substr(0, 7) + std::to_wstring(pts) + base.substr(bar);

        // 按这个玩家的权限组打折 —— 面板显示的就是折后价
        //   out 结构: POINTS|<余额>|名|价|名|价|…  ⇒ 索引 0/1 原样，之后偶数=名、奇数=价
        //   取整用 floor(p*k+0.5) —— 和后面"实际扣费"必须用同一个公式
        {
            const float disc = DiscountFor(PermGroupsCached(eosId));
            if (disc > 0.0f)
            {
                const float k = 1.0f - disc / 100.0f;
                std::wstring rebuilt;
                size_t pos = 0; int idx = 0;
                for (;;)
                {
                    const size_t bar2 = out.find(L'|', pos);
                    const std::wstring tok = (bar2 == std::wstring::npos) ? out.substr(pos) : out.substr(pos, bar2 - pos);
                    if (idx == 0)      rebuilt = tok;
                    else if (idx == 1) rebuilt += L"|" + tok;
                    else if (idx % 2 == 0) rebuilt += L"|" + tok;                 // 词条名
                    else
                    {
                        const float p = (float)_wtof(tok.c_str());
                        wchar_t pbuf[32];
                        swprintf_s(pbuf, L"%d", (int)(p * k + 0.5f));
                        rebuilt += L"|"; rebuilt += pbuf;
                    }
                    ++idx;
                    if (bar2 == std::wstring::npos) break;
                    pos = bar2 + 1;
                }
                out = rebuilt;
            }
        }

        // 把恐龙价值表和等级系数一起带给面板，
        //   让面板能在本地算出"这只龙、这个词条、这个等级"的实付总价。
        //   格式（用 `||` 分段，面板按 `||` 切）：
        //     ||SPECIES|物种=系数|物种=系数…
        //     ||TIERS|1=1|2=1.5|3=2
        //   两张表都来自插件自己的 ini（零第三方依赖）。
        //  先推这只玩家附近每一只已驯服龙的"名字 → 系数"别名
        //   真机铁证（2026-09-28）：面板底部价 = 折后单价 × 等级系数 × 物种系数，而面板的"物种串"
        //   取的是 `Get Dino Descriptive Name` —— **驯服龙带自定名时它给的就是龙名**（如「带自定名的龙」）⇒
        //   拿龙名去比物种表（`Dodo` / `渡渡鸟`）永远比不中 ⇒ 面板 SpeciesCoef 回落到 1.0
        //   于是面板显示 1360（= 680 × 2 × 1.0）而实际扣 204（= 800 × 2 × 0.15 × 0.85）⇒ 差 6.6 倍
        //   别名两张都推：① 玩家自定龙名（TamedName）② 插件口径的物种名（DinoSpecies）
        //   ⇒ 面板不管手里拿到哪一个，都能命中和扣费同一个系数
        //   注意：别名必须排在物种表**最前面**（面板的匹配是"第一个命中的键赢"）
        std::wstring aliasSeg;
        if (pcForAlias != nullptr)
        {
            int nearTotal = 0;
            //  注意：变量名**不能叫 `near`** —— Windows 老 SDK 里 `near`/`far` 是空宏
            //   ⇒ `near[i]` 会被预处理成 `[i]` ⇒ C2059 语法错误 （本次编译实测踩过）
            const std::vector<DinoRef> nearList = CollectTamedNear(pcForAlias, 30000.f, 120, &nearTotal);
            // 收集 (键, 系数)，然后按键长降序排 —— 面板的匹配是"第一个命中的键赢"，
            //   长键更具体 ⇒ 必须先比 否则一只叫「Par」的龙会把它那份系数套到 Parasaur 头上
            std::vector<std::pair<std::wstring, std::wstring> > kv;
            for (size_t i = 0; i < nearList.size(); ++i)
            {
                wchar_t vbuf[32];
                swprintf_s(vbuf, L"%g", SpeciesCoefOf(nearList[i].species));
                const std::wstring cand[2] = { SanitizeShopKey(nearList[i].name), SanitizeShopKey(nearList[i].species) };
                for (int k = 0; k < 2; ++k)
                {
                    if (cand[k].empty()) continue;
                    bool dup = false;
                    for (size_t j = 0; j < kv.size(); ++j)
                        if (kv[j].first == cand[k]) { dup = true; break; }
                    if (dup) continue;
                    kv.push_back(std::make_pair(cand[k], std::wstring(vbuf)));
                }
            }
            std::stable_sort(kv.begin(), kv.end(),
                [](const std::pair<std::wstring, std::wstring>& x,
                   const std::pair<std::wstring, std::wstring>& y) { return x.first.size() > y.first.size(); });
            for (size_t i = 0; i < kv.size(); ++i)
            {
                aliasSeg += L"|"; aliasSeg += kv[i].first; aliasSeg += L"="; aliasSeg += kv[i].second;
            }
        }
        if (!g_shop_species_table.empty() || !aliasSeg.empty())
        {
            out += L"||SPECIES";
            out += aliasSeg;                       //  别名在前（第一个命中就赢）
            for (size_t i = 0; i < g_shop_species_table.size(); ++i)
            {
                wchar_t buf[64];
                swprintf_s(buf, L"|%s=%g", g_shop_species_table[i].first.c_str(), g_shop_species_table[i].second);
                out += buf;
            }
        }
        {
            wchar_t buf[128];
            swprintf_s(buf, L"||TIERS|1=%g|2=%g|3=%g",
                       g_shop_tier_coef[0], g_shop_tier_coef[1], g_shop_tier_coef[2]);
            out += buf;
        }
        // 把"开面板的按键名"一起带给面板（面板存成变量，CCA 用它判断按键）
        //     格式：||KEY|F2        （可配置：-settings.ini 的 ui_key）
        if (!g_shop_ui_key.empty())
        {
            wchar_t buf[64];
            swprintf_s(buf, L"||KEY|%s", g_shop_ui_key.c_str());
            out += buf;
        }
        // 把商店名带给面板（面板显示在标题上；名字来自语言文件的 shop_name）
        {
            const std::wstring title = Msg(L"shop_name");
            if (!title.empty() && title.compare(0, 5, L"[msg:") != 0)
            {
                wchar_t buf[256];
                swprintf_s(buf, L"||TITLE|%s", title.c_str());
                out += buf;
            }
        }
        // 把面板底部的使用提示带给面板（来自语言文件的 ui_hint；中英各一份）
        {
            const std::wstring hint = Msg(L"ui_hint");
            if (!hint.empty() && hint.compare(0, 5, L"[msg:") != 0)
            {
                wchar_t buf[256];
                swprintf_s(buf, L"||HINT|%s", hint.c_str());
                out += buf;
            }
        }
        //  v1.23.2（B）：把上一次下单被拒的原因搭车带回面板（客户端 mod 还没认这个 KEY 时它静默忽略）
        if (!eosId.empty())
        {
            std::wstring err;
            {
                std::lock_guard<std::mutex> lk(g_push_mu);
                std::map<std::wstring, std::wstring>::iterator ie = g_pending_error.find(eosId);
                if (ie != g_pending_error.end()) { err = ie->second; g_pending_error.erase(ie); }   // 只带一次
            }
            if (!err.empty()) out += L"||ERR|" + err;
        }

        // 轮盘条目上显示的名字（来自语言文件的 wheel_entry；中英各一份）
        {
            const std::wstring we = Msg(L"wheel_entry");
            if (!we.empty() && we.compare(0, 5, L"[msg:") != 0)
            {
                wchar_t buf[256];
                swprintf_s(buf, L"||WHEEL|%s", we.c_str());
                out += buf;
            }
        }
        // 把面板所有静态文案一次性带过去
        //   格式：||UI|控件名=文案|控件名=文案…
        //   面板侧用 Get Widget From Name 按名字套用 ⇒ 以后加文案只要 ini 加一行 + 控件名字对上
        {
            static const wchar_t* kUiKeys[] = {
                L"Text_LeftTitle", L"Text_RightTitle",
                L"Text_T1", L"Text_T2", L"Text_T3", L"Text_Clear",
                L"Text_Confirm", L"Text_Close", L"Text_Selection",
                L"Text_SearchDinoHint", L"Text_SearchTraitHint",
                //  v1.23.16/17：给面板的三条"格式模板"（面板拿它填自己的数据）
                //   `Fmt_Points` = 右上角点数格（面板那块 `Format Text("Points: {0}")` 读它）
                //   `Fmt_Lvl`    = 龙行等级（`{0} lvl` / `{0} 级`）
                //   `Fmt_Sel`    = 选中行（`Selected: {0} {1} · Tier {2} · {3} pts`）
                //   为什么不用整句推送：点数/等级/选中行都是**面板本地拼的**，插件不知道值与顺序
                L"Fmt_Points", L"Fmt_Lvl", L"Fmt_Sel", L"Fmt_Price",
                nullptr
            };
            std::wstring ui = L"||UI";
            bool any = false;
            for (int i = 0; kUiKeys[i] != nullptr; ++i)
            {
                //  v1.23.16/17：`Fmt_*` 是"格式模板"（带 {0}/{1} 占位符，面板自己填数据）
                //   原来是硬编在 mod 里的中文 （`点数：{0}` / `{0} 级` / `已选：…`）⇒ 改成插件推、随 msg_lang 切
                // `Fmt_*` 必须走 `MsgRaw()` —— 走 `Msg()` 会让占位符被替换成空串
                //   （2026-09-28 真机：面板显示 `分`、龙行显示 `级` 却没有数字 ⇒ 根因就是这个）
                const bool is_fmt = (std::wstring(kUiKeys[i]).compare(0, 4, L"Fmt_") == 0);
                const std::wstring v = is_fmt ? MsgRaw(kUiKeys[i]) : Msg(kUiKeys[i]);
                if (v.empty() || v.compare(0, 5, L"[msg:") == 0) continue;
                ui += L"|"; ui += kUiKeys[i]; ui += L"="; ui += v;
                any = true;
            }
            if (any) out += ui;
        }
        return out;
    }

    // ===== v1.12.0 M4b：把缓存的价目下发给客户端 mod =====
    //   通道：客户端 mod 的 buff 上有个「在拥有的客户端上运行」事件 ApplyShopData(Data: String)
    //   ⇒ 服务器侧 ProcessEvent 调它，UE 会自己把它 RPC 到该 buff 的 owning client
    //   （写法照抄官方 Actor.h:1944 的 ProcessEvent(ClassField()->FindFunctionByName(...), &params)）
    //
    //   v1.13.0 M4c：**改成按玩家下发** ——
    //    坑：APrimalBuff 基类**没有** owner 访问器（只有子类 APrimalBuff_Grappled 有 MyOwner）
    //     ⇒ 不能"遍历 buff 再问它属于谁" ⇒ **反过来查**：
    //     遍历 AShooterPlayerController ⇒ 取它的角色 ⇒ 在这个角色的 BuffsField() 里找我们的 buff
    static UClass* g_shop_buff_class = nullptr;
static int g_auto_attach_done = 0;   // 自动挂 TraitShop_Buff 只允许一次（防每 30 秒重复挂）

// DIAG：排查"面板自己消失 + 输入锁死（UI Only 卡住）"
//   怀疑 shop buff 会被重建/过期 ⇒ 面板跟着没了，而输入模式没人恢复。
//   这里记录 shop buff 的实例地址/数量/寿命，只在变化时打印一行。
static void* g_diag_buff_addr = nullptr;
static int   g_diag_buff_count = -1;
    static unsigned long long g_shop_push_last = 0;
static int g_shop_buff_state = 0;   // 0=未知 1=已加载 -1=找不到（只打一次日志，别每 30 秒刷屏）

    struct FApplyShopDataParams { FString Data; };   // UE 按事件参数声明顺序摆参数块

    // 给单个玩家推一小段载荷（面板报价用）
    //   为什么能推"半截"：面板的 ApplyShopData 是按 `||` 分段逐段处理的，
    //   只带 `UI|…` 一段 ⇒ 只有那一段生效，其它（价目/物种表/列表）**不会被清空**
    bool PushShopPayload(AShooterPlayerController* pc, const std::wstring& payload)
    {
        if (pc == nullptr) return false;
        AShooterCharacter* ch = pc->GetPlayerCharacter();
        if (ch == nullptr) return false;
        TArray<APrimalBuff*>& buffs = ch->BuffsField();
        for (int i = 0; i < buffs.Num(); ++i)
        {
            APrimalBuff* b = buffs[i];
            if (b == nullptr) continue;
            if (b->bDeactivated().Get()) continue;                 // 死 buff 不算
            UClass* bc = b->ClassField();
            if (bc == nullptr) continue;
            const FString n = bc->NameField().ToString();
            if (std::wstring(*n).find(L"TraitShop_Buff") == std::wstring::npos) continue;
            UFunction* fn = bc->FindFunctionByName("ApplyShopData", EIncludeSuperFlag::IncludeSuper);
            if (fn == nullptr) return false;
            FApplyShopDataParams p;
            p.Data = FString(payload.c_str());
            b->ProcessEvent(fn, &p);
            return true;
        }
        return false;
    }

    // 面板报价：把 `UI|键=值|键=值` 这段推给某个玩家
    // **必须推整条载荷**，不能只推 `UI|…`
    //   2026-09-28 真机踩雷（真机实测抓到）：v1.23.21 的 `算价` 只推 `UI|Text_Selection=…|PRICE=204`
    //   ⇒ 面板按 `||` 切段后，**第 0 段被当成 `POINTS|点数|词条|价|…` 解析**
    //   ⇒ 点数栏显示成 `Text_Selection=已选：…`、右栏词条清空、还多出一行 `PRICE=204`
    //   （根因：`ApplyShopData` 的循环跑完后无条件用「第 0 段」重建点数+词条列表）
    //   修法：先取和周期推送一模一样的整条载荷`ShopDataForPlayer(...)`，再把 `||UI|…` 追到**末尾**
    //   —— 末尾的 UI 段在重建列表之前就已生效，且重建只动列表、不动 `Text_Selection`
    bool PushPanelUi(AShooterPlayerController* pc, const std::wstring& uiBody)
    {
        if (pc == nullptr) return false;
        const FString cname = pc->GetCharacterName();
        FString eos = pc->GetEOSId();
        const std::wstring full = ShopDataForPlayer(cname, std::wstring(*eos), pc) + L"||UI|" + uiBody;
        return PushShopPayload(pc, full);
    }

    void PushShopDataToBuff()
    {
        // 外部没推时，插件自己的价目表也算"有数据" ——
        //   否则这一行 return 会把自动挂 buff一起跳过，玩家按开面板键毫无反应
        if (g_shop_price_text.empty() && g_shop_price_table.empty()) return;

        UWorld* world = AsaApi::GetApiUtils().GetWorld();
        if (world == nullptr) return;

        if (g_shop_buff_class == nullptr)
        {
            g_shop_buff_class = UVictoryCore::BPLoadClass(
                FString(L"Blueprint'/ZeroTraitUI/TraitShop_Buff.TraitShop_Buff_C'"));
            if (g_shop_buff_class == nullptr)
            {
                if (g_shop_buff_state != -1)   //  只在状态变化时记一次，免得 mod 不在场时每 30 秒一行
                {
                    g_shop_buff_state = -1;
                    Log::GetLog()->warn("[ZeroARKTrait] mod buff class NOT found (ZeroTraitUI not installed?)");
                }
                return;
            }
            g_shop_buff_state = 1;
            Log::GetLog()->info("[ZeroARKTrait] mod buff class loaded OK");
        }

        TArray<AActor*> pcs;
        UGameplayStatics::GetAllActorsOfClass(world, AShooterPlayerController::StaticClass(), &pcs);
        if (pcs.Num() == 0) return;

        // 日志要打真实载荷长度—— 原来打的是 g_shop_price_text.size()（外部缓存）
        //    ⇒ 插件自读价目时那行永远是 len=0，误导排查
        size_t payloadLen = 0;

        int pushed = 0;
        int skipped = 0;   // 因为"载荷没变"而跳过的次数（日志要打出来，免得再误导）
        int online = 0;
        std::map<std::wstring, int> pass_seen;   // 本轮在线集合（用来清理"已下线"的状态）
        for (AActor* a : pcs)
        {
            AShooterPlayerController* pc = static_cast<AShooterPlayerController*>(a);
            if (pc == nullptr) continue;
            AShooterCharacter* ch = pc->GetPlayerCharacter();
            if (ch == nullptr) continue;
            ++online;

            const FString cname = pc->GetCharacterName();
            const std::wstring eosP = std::wstring(*pc->GetEOSId());   // 本轮要用它做"载荷没变就不推"
            TArray<APrimalBuff*>& buffs = ch->BuffsField();

            // 修"每 30 秒重复挂 buff"的真凶。
            //   实测（2026-09-22）：日志同时出现 `has=false byName=false`（判据失败）
            //   与 `pushed=1`（同一个判据却成功）—— 说明两处遍历看到的列表不是同一批，
            //   而且蓝图实例的类 与 BPLoadClass 返回的类对象并不相等，
            //   FName 直接比也不可靠。结果：每 30 秒新挂一个 buff，
            //   数据却推给新挂的那个⇒ 新 buff 的 mod 侧变量（PanelRef）是空的
            //   ⇒ 面板的 ApplyShopData 永远收不到数据 ⇒ 右栏永远空。
            //   修法：用类名字符串包含 TraitShop_Buff这个判据（我们自己 mod 的类名固定），
            //   并让"判有没有""推给谁"两处用同一个函数。
            auto IsShopBuff = [](APrimalBuff* bb) -> bool {
                if (bb == nullptr) return false;
                UClass* bc = bb->ClassField();
                if (bc == nullptr) return false;
                if (g_shop_buff_class != nullptr && bc == g_shop_buff_class) return true;
                const FString n = bc->NameField().ToString();
                const std::wstring s(*n);
                return s.find(L"TraitShop_Buff") != std::wstring::npos;
            };

            //  玩家（重新）进来时，先把他身上那个旧shop buff 摘掉 ⇒ 下一轮强制重挂
            //   为什么必须这样：服务器侧的 buff 一直存在 ⇒ 原来"有就不挂" ⇒ 客户端手里那份是旧副本、
            //   永远不刷新 ⇒ 每次都得重开游戏才能看到轮盘条目
            //
            bool force_attach = false;
            {
                const std::wstring eosJ = std::wstring(*pc->GetEOSId());
                if (!eosJ.empty())
                {
                    pass_seen[eosJ] = 1;
                    std::map<std::wstring, int>::iterator itj = g_join_state.find(eosJ);
                    if (itj == g_join_state.end())
                    {
                        bool has_old = false;
                        for (int i = 0; i < buffs.Num(); ++i)
                            if (IsShopBuff(buffs[i])) { has_old = true; break; }
                        if (false && has_old)   // 照 ArkShopUI 对照结论 —— 只"没有才挂"，绝不摘旧 buff（摘掉后客户端反而接不住 ⇒ 轮盘/F3 全没）
                        {
                            for (int i = 0; i < buffs.Num(); ++i)
                                if (IsShopBuff(buffs[i])) buffs[i]->Deactivate();
                            g_join_state[eosJ] = 1;      // 1 = 已摘掉，下一轮强制重挂
                            Log::GetLog()->info("[ZeroARKTrait] rejoin: deactivated stale shop buff for {}",
                                                AsaApi::Tools::Utf8Encode(eosJ));
                        }
                        else { g_join_state[eosJ] = 2;
                            // 新会话（含重连）⇒ 强制推 5 次：不摘 buff（照 ArkShopUI / HLNA）但面板会重建 ⇒ 必须让新面板拿到数据
{ std::lock_guard<std::mutex> lk(g_push_mu); g_force_push_left[eosJ] = 5; }
                            // 进服提醒 —— 排一个"延迟发"的队
                            if (g_shop_join_tip)
                            {
                                g_join_tip_due[eosJ] = GetTickCount64() + (unsigned long long)g_shop_join_tip_delay * 1000ULL;
                                Log::GetLog()->info("[ZeroARKTrait] join tip queued for {} (+{}s)", AsaApi::Tools::Utf8Encode(eosJ), g_shop_join_tip_delay);
                            } }
                    }
                    else if (itj->second == 1)
                    {
                        itj->second = 2;
                        force_attach = true;             // 下一轮不看向量状态，直接挂新的
                    }
                }
            }

            // 重连时"离线"根本观察不到（实测 226 行 online=1、online=0 一行都没有）
            //   ⇒ 用PlayerController 对象换了一个当"新会话"的第二判据 （重连必然是新对象）
            //   只在"以前见过、现在换了对象"时排队（第一次见由上面的新会话分支负责 不重复）
            if (g_shop_join_tip && !eosP.empty())
            {
                const void* pcv = (const void*)pc;
                std::map<std::wstring, void*>::iterator itp = g_join_tip_pc.find(eosP);
                if (itp == g_join_tip_pc.end() || itp->second != pcv)
                {
                    if (itp != g_join_tip_pc.end())
                    {
                        g_join_tip_due[eosP] = GetTickCount64() + (unsigned long long)g_shop_join_tip_delay * 1000ULL;
                        Log::GetLog()->info("[ZeroARKTrait] join tip queued for {} (+{}s, pc changed)",
                                            AsaApi::Tools::Utf8Encode(eosP), g_shop_join_tip_delay);
                        //  重连（pc 换了对象）必须"无视去重"推首包
                        //   2026-09-28 真机实证 ：客户端 07:27 重启后重连，插件这边载荷没变
                        //   ⇒ 一直 `pushed=0 skippedUnchanged=1` ⇒ 新会话**从没收到过** `||KEY|<ui_key>`
                        //   ⇒ 客户端 UiKey 是空的 ⇒ buff 的 `Input Key` 比对不过
                        //   ⇒ **F3 完全打不开面板**（日志里连 `client asked for shop data` 都没有）
                        //   救急 = RCON `Trait 推商店数据 <玩家>`（force push x3）；
                        //   根治 = 这里跟上面"首次见到该玩家"的分支一样，强制推 5 次（≈10 秒）
                        //   注意：强制推不会刷掉搜索框 —— 只推 5 次就回到去重（v1.20.6 的规矩不变）
                        { std::lock_guard<std::mutex> lk(g_push_mu); g_force_push_left[eosP] = 5; }
                        Log::GetLog()->info("[ZeroARKTrait] rejoin push: force-push x5 for {} (pc changed)",
                                            AsaApi::Tools::Utf8Encode(eosP));
                    }
                    g_join_tip_pc[eosP] = (void*)pc;
                }
            }

// 到点就发"进服提醒"（照 HLNA 的通道：SendServerMessage 彩色一行）
if (g_shop_join_tip && !eosP.empty())
{
                std::map<std::wstring, unsigned long long>::iterator itd = g_join_tip_due.find(eosP);
                if (itd != g_join_tip_due.end() && GetTickCount64() >= itd->second)
                {
                    Say(pc, Msg(L"join_tip", g_shop_ui_key));
                    Log::GetLog()->info("[ZeroARKTrait] join tip sent to {}", AsaApi::Tools::Utf8Encode(eosP));
                    g_join_tip_due.erase(itd);
                }
            }

            {
                bool hasShopBuff = false;
                int foundAt = -1;
                for (int i = 0; i < buffs.Num(); ++i)
                {
                    if (!IsShopBuff(buffs[i])) continue;
                    //  已失效（deactivated）的 buff 要当成没有
                    //   否则"有就不挂"会让一个死掉的 buff 永久占位 ⇒ 轮盘条目再也不出现
                    if (buffs[i]->bDeactivated().Get()) continue;
                    hasShopBuff = true; foundAt = i; break;
                }
                if ((!hasShopBuff || force_attach) && g_ui_enabled)   // 重连时强制重挂
                {
                    TSubclassOf<APrimalBuff> cls(g_shop_buff_class);
                    // 第 5 个参数 = bForceOnClient 必须传 true
                    //   传 false ⇒ buff 只挂在服务端，客户端身上没有这个 buff
                    //   ⇒ 轮盘条目（客户端从自己身上的 buff 收 GetMultiUseEntriesFromBuffs）
                    //   永远不出现 （单机因为客户端=服务端，所以看起来正常）
                    APrimalBuff* nb = APrimalBuff::StaticAddBuff(cls, ch, nullptr, nullptr, true);
                    Log::GetLog()->info("[ZeroARKTrait] auto-attach TraitShop_Buff -> {}  [DIAG buffs={} online={}]",
                                        (nb != nullptr ? "OK" : "FAILED"), buffs.Num(), online);
                    // 刚挂上新 buff ⇒ 立刻推一次（别让玩家按 F2 干等一个周期）
                    g_shop_push_last = 0;
                    { std::lock_guard<std::mutex> lk(g_push_mu); g_last_pushed.erase(eosP); }   // 新 buff 没数据 ⇒ 必须重推一次
                { std::lock_guard<std::mutex> lk(g_push_mu); g_force_push_left[eosP] = 5; }   // 新会话强制推 5 次（≈10 秒）⇒ 面板新建后必然接住，之后立刻恢复去重、不再刷列表
                }
            }

            for (int i = 0; i < buffs.Num(); ++i)
            {
                APrimalBuff* b = buffs[i];
                if (b == nullptr) continue;
                if (!IsShopBuff(b)) continue;

                //  注意：这里必须用窄字符串"ApplyShopData" —— AsaApi 只导出了 FName 的
                //   窄字符构造；宽字符版 FName(const wchar_t*, EFindName) 会 LNK2019（官方范例
                //   Actor.h:1944 的 ProcessEvent(FindFunctionByName("IsDead", ...)) 也是窄串）
                UFunction* fn = b->ClassField()->FindFunctionByName("ApplyShopData", EIncludeSuperFlag::IncludeSuper);
                if (fn == nullptr) continue;
                FApplyShopDataParams p;
                // 把玩家 EOS ID 也带上 ⇒ points_mode=db 时插件自己查数据库
                //  注意：AsaApi 的 FString 没有 c_str() ⇒ 用 `*fstr` 取 const wchar_t*
                FString eos = pc->GetEOSId();
                const std::wstring eosW(*eos);
                const std::wstring payload = ShopDataForPlayer(cname, eosW, pc);
                payloadLen = payload.size();
                //  载荷没变 ⇒ **不再重复推** （面板每收一次都会往控件里写，
                //   把玩家正在输入的搜索框内容刷掉）
                int forceLeft = 0; bool unchanged = false;
                { std::lock_guard<std::mutex> lk(g_push_mu);
                  forceLeft = g_force_push_left[eosP];
                  std::map<std::wstring, std::wstring>::iterator ip = g_last_pushed.find(eosP);
                  unchanged = (ip != g_last_pushed.end() && ip->second == payload); }
                if (forceLeft <= 0 && unchanged && !force_attach) { ++skipped; continue; }
                { std::lock_guard<std::mutex> lk(g_push_mu); g_last_pushed[eosP] = payload; }
                p.Data = FString(payload.c_str());
                b->ProcessEvent(fn, &p);
                { std::lock_guard<std::mutex> lk(g_push_mu); if (g_force_push_left[eosP] > 0) --g_force_push_left[eosP]; }   // 强制次数用完就恢复"载荷没变不推"
                ++pushed;
            }

            // DIAG（只在变化时打印）：shop buff 的实例地址 / 数量 / 寿命
            //   **故意只用"直接字段读取"**（地址/数量/DeactivateAfterTime），
            //   不再调用 GetRemainingTime() —— 那是一次"按名字找引擎函数"的 NativeCall，
            //   万一解析不到会在 Tick hook 里出事，把 hook 整个搞哑（本次就是这么怀疑的）。
            {
                void* addr = nullptr; int cnt = 0; int live = 0; float dat = -1.f;
                bool deact = false, hasBP = false, allowSelf = false;
                std::wstring cls = L"?";
                for (int i = 0; i < buffs.Num(); ++i)
                {
                    APrimalBuff* bb = buffs[i];
                    if (!IsShopBuff(bb)) continue;
                    const bool dead = bb->bDeactivated().Get();
                    if (cnt == 0)
                    {
                        addr = bb; dat = bb->DeactivateAfterTimeField(); deact = dead;
                        allowSelf = bb->bAllowMultiUseEntriesFromSelf().Get();
                        UClass* bc = bb->ClassField();
                        if (bc != nullptr)
                        {
                            const FString n = bc->NameField().ToString();
                            cls = std::wstring(*n);
                            hasBP = (bc->FindFunctionByName("BPGetMultiUseEntries", EIncludeSuperFlag::IncludeSuper) != nullptr);
                        }
                    }
                    if (!dead) ++live;
                    ++cnt;
                }
                static int diagLive = -1;
                if (addr != g_diag_buff_addr || cnt != g_diag_buff_count || live != diagLive)
                {
                    g_diag_buff_addr = addr; g_diag_buff_count = cnt; diagLive = live;
                    Log::GetLog()->info("[ZeroARKTrait] DIAG shopbuff count={} live={} deact={} hasBPGetMultiUse={} allowSelf={} DeactivateAfter={:.1f} cls={} addr={}",
                        cnt, live, deact, hasBP, allowSelf, dat,
                        AsaApi::Tools::Utf8Encode(cls), (void*)addr);
                }
            }
        }
        // 清理已下线玩家的状态 ⇒ 他下次进来又算"新会话"，再走一次摘+重挂
        for (std::map<std::wstring, int>::iterator itj = g_join_state.begin(); itj != g_join_state.end(); )
        {
            if (pass_seen.find(itj->first) == pass_seen.end()) itj = g_join_state.erase(itj);
            else ++itj;
        }
        // 载荷缓存也要随下线清理
        for (std::map<std::wstring, std::wstring>::iterator ip = g_last_pushed.begin(); ip != g_last_pushed.end(); )
        {
            if (pass_seen.find(ip->first) == pass_seen.end()) ip = g_last_pushed.erase(ip);
            else ++ip;
        }
// 没发出去的"进服提醒"也要随下线清理
for (std::map<std::wstring, unsigned long long>::iterator itt = g_join_tip_due.begin(); itt != g_join_tip_due.end(); )
{
    if (pass_seen.find(itt->first) == pass_seen.end()) itt = g_join_tip_due.erase(itt);
    else ++itt;
}
        // PC 指针表同样随下线清理 （下次进来 = 新对象 ⇒ 会重新提醒）
        for (std::map<std::wstring, void*>::iterator itv = g_join_tip_pc.begin(); itv != g_join_tip_pc.end(); )
        {
            if (pass_seen.find(itv->first) == pass_seen.end()) itv = g_join_tip_pc.erase(itv);
            else ++itv;
        }

        Log::GetLog()->info("[ZeroARKTrait] pushed shop data: pushed={} skippedUnchanged={} online={} payloadLen={} (extCache={})",
                            pushed, skipped, online, payloadLen, g_shop_price_text.size());
    }

    // ===== v1.12.0 M4b：给 mod 下发的数据缓存（由机器人经 RCON 推上来）=====
    //   （g_shop_price_text / g_shop_dirty 的声明已提到本段上方）

    // 价目表分块缓存（flag=0 首块重置，其余追加）—— 与 Trait 面板文本同一套约定
    void CacheShopPrice(const std::wstring& tail)
    {
        std::vector<std::wstring> tt;
        Split(Trim(tail), tt);
        if (tt.size() < 2) return;
        const int flag = _wtoi(tt[0].c_str());
        if (flag == 0) g_shop_price_text.clear();
        std::wstring body = Trim(tail.substr(tail.find(L' ') + 1));
        g_shop_price_text += body;
        g_shop_dirty = true;
        g_shop_push_last = 0;   // 刚收到新数据 ⇒ 下一个 Tick（≤1 秒）立刻下发
        Log::GetLog()->info("[ZeroARKTrait] shop price cached: block flag={} len={}", flag, g_shop_price_text.size());
    }

    // ===== v1.12.0 M4a 起步：找到客户端 mod 的 CCA（最小验证）=====
    //   客户端 mod（ZeroTraitUI）的 CCA 是 ASaveGameActor 子类（Actor.h:10105 AActor 派生）
    //   ⇒ 用 UGameplayStatics::GetAllActorsOfClass(Other.h:1168) 按类枚举即可。
    //   蓝图路径格式参考 ArkApiUtils.h:712 的 BPLoadClass 用法（Blueprint'<path>.<name>'）。
    static UClass* g_mod_cca_class = nullptr;   // 缓存（找到一次就记住）
    static int      g_mod_cca_state = 0;        // -1=没找到(只报一次) 1=已找到
    static int      g_mod_cca_count = -1;       // 上次看到的数量（变化才打日志）

    void ScanModCca()
    {
        if (g_mod_cca_class == nullptr)
        {
            g_mod_cca_class = UVictoryCore::BPLoadClass(
                FString(L"Blueprint'/ZeroTraitUI/CCA_ZeroTraitUI.CCA_ZeroTraitUI_C'"));
            if (g_mod_cca_class == nullptr)
            {
                if (g_mod_cca_state != -1)
                {
                    g_mod_cca_state = -1;
                    Log::GetLog()->warn("[ZeroARKTrait] mod CCA class NOT found (ZeroTraitUI not installed?)");
                }
                return;
            }
            Log::GetLog()->info("[ZeroARKTrait] mod CCA class loaded OK");
        }

        TArray<AActor*> found;
        UGameplayStatics::GetAllActorsOfClass(AsaApi::GetApiUtils().GetWorld(), g_mod_cca_class, &found);

        if (found.Num() != g_mod_cca_count)
        {
            g_mod_cca_count = found.Num();
            g_mod_cca_state = 1;
            Log::GetLog()->info("[ZeroARKTrait] mod CCA actors = {}", found.Num());
        }
    }
    // ===== M4a 验证段结束 =====

    void Tick()
    {
        // HB：心跳（每 10 次 Tick 打一行 = 约 10 秒一行）
        //   排查"玩家在线但插件零日志"：先确认 Tick 到底有没有在被调用。
        {
            static int hb = 0;
            if (++hb % 10 == 1)
                Log::GetLog()->info("[ZeroARKTrait] HB tick alive #{} status={}", hb, (int)AsaApi::GetApiUtils().GetStatus());
        }
        if (AsaApi::GetApiUtils().GetStatus() != AsaApi::ServerStatus::Ready) return;

        PumpNotifications();   // 把排队中的"结果+原因"推给已上线的玩家

        ScanModCca();   // M4a：每秒扫一次 mod 的 CCA（Tick 本身是 1 秒一次）

        // M4b：每 30 秒把缓存的价目下发给所有客户端 mod 的 buff（无数据时内部直接返回）
        {
            const unsigned long long now = GetTickCount64();
            if (g_shop_push_last == 0 || now - g_shop_push_last >= (unsigned long long)g_shop_push_seconds * 1000ULL)
            {
                g_shop_push_last = now;
                PushShopDataToBuff();
            }
        }


        // 有搜索在进行时，本 Tick 只推进搜索（保证每 Tick 工作量有硬上限）
        if (g_search.active)
        {
            AdvanceSearch();
            return;
        }

        Job j;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (g_jobs.empty()) return;
            j = g_jobs.front();
            g_jobs.erase(g_jobs.begin());
        }

        if (j.by_id || j.by_name) StartSearch(j);
        else                      RunJobNow(j);
    }

    std::wstring HelpText()
    {
        // 回执尽量走 ASCII（RCON 通道只支持 ASCII；游戏内控制台看得到中文部分）
        return L"ZeroARKTrait: Trait <list|liebiao> <target> | <add|jia> <target> <trait> [level 1-3] | "
               L"<del|shan> <target> <trait> | <order|dingdan> <订单号> <target> <trait> [level] | "
               L"<pack|libao> <target> <词条1,词条2,…> | <query|chadan> <订单号> | <menu|caidan> | <names|词条名> | "
               L"<panel|面板> <玩家> [auto] | <say|提示> <玩家> <文本> | <data|数据> <玩家> <文本> "
               L"| <买> <玩家> <词条> [级别1/2/3] / <取单> / <单结> <id> [fail] / <单清> "
               L"| <龙|long/near> <玩家> [半径] [zs] "
               L"| target = <player name> | id:<DinoID1>[:<DinoID2>] | name:<恐龙名子串> "
               L"| 中文: 列表/加/删/订单/礼包/查单/菜单/帮助/面板/买/取单/单结/龙；审计 → trait_audit.log；"
               L"玩家在聊天里输入 trait 会出**游戏内编号菜单**（trait 5 = 面板；trait 买 = 游戏内下单；"
               L"trait 龙 = 我附近的已驯服龙，trait 选 <序号> 选定）";
    }

    // kind: 1=add 2=del 3=list
    void Enqueue(int kind, const std::wstring& rest, std::wstring& reply)
    {
        std::vector<std::wstring> t;
        Split(Trim(rest), t);

        Job j;
        j.kind = kind;

        if ((kind == 1 || kind == 2) && t.size() >= 2)
        {
            ParseTargetToken(t[0], j);
            ParseTraitToken(t[1], j.trait, j.tier);
            if (t.size() >= 3 && j.tier == 0) j.tier = _wtoi(t[2].c_str());   // 支持"词条 层级"空格写法
            j.trait = NormalizeTraitName(j.trait);
        }
        else if (kind == 3 && t.size() >= 1)
        {
            ParseTargetToken(t[0], j);
        }
        else if (kind == 5 && t.size() >= 3)      // 订单：<订单号> <目标> <词条> [层级]
        {
            if (!g_order_enabled) { reply = L"ERR: 订单功能已被服主关闭（order_enabled=0）"; return; }
            j.order = t[0];
            ParseTargetToken(t[1], j);
            ParseTraitToken(t[2], j.trait, j.tier);
            j.trait = NormalizeTraitName(j.trait);
            j.kind = 1;                            // 订单 = 加词条（带审计）
        }
        else if (kind == 6 && t.size() >= 2)      // 礼包：<目标> <词条1,词条2,…>
        {
            ParseTargetToken(t[0], j);
            for (size_t i = 1; i < t.size(); ++i)
            {
                std::vector<std::wstring> parts;
                SplitComma(t[i], parts);
                for (size_t k = 0; k < parts.size(); ++k)
                {
                    std::wstring nm; int tr = 0;
                    ParseTraitToken(parts[k], nm, tr);
                    nm = NormalizeTraitName(nm);
                    if (!nm.empty()) j.bundle.push_back(std::make_pair(nm, tr));
                }
            }
            if (j.bundle.empty()) { reply = HelpText(); return; }
        }
        else if (kind == 7 && t.size() >= 1)      // 查单：<订单号>
        {
            j.order = t[0];
            j.kind = 7;
        }
        else if (kind == 11 && t.size() >= 1)     // v1.9.2 无敌：<玩家>
        {
            j.player = t[0];
        }
        else if (kind == 12 && t.size() >= 1)     // v1.9.7 面板：<玩家> [auto|自动]
        {
            j.player = t[0];
            // v1.9.9：第二个词写 auto/自动 ⇒ 用 bAutoClose=true 弹窗（免得玩家找不到关掉的办法）
            if (t.size() >= 2 && (t[1] == L"auto" || t[1] == L"Auto" || t[1] == L"自动")) j.tier = 1;
        }
        else if (kind == 13 && t.size() >= 1)     // v1.11.0 龙列表：<玩家> [半径] [zs]
        {
            j.player = t[0];
            for (size_t i = 1; i < t.size(); ++i)
            {
                if (t[i] == L"zs" || t[i] == L"ZS" || t[i] == L"mod" || t[i] == L"Mod" || t[i] == L"数据")
                    j.tier = 1;                                   // 只发 #ZS| 结构化行
                else
                {
                    const float r = (float)_wtof(t[i].c_str());
                    if (r > 0.f) j.radius = r;
                }
            }
        }
        else
        {
            reply = HelpText();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_jobs.push_back(j);
        }
        reply = L"OK: accepted (result -> that player's screen + server log)";
        Log::GetLog()->info("[ZeroARKTrait] queued kind={} player={} trait={}", kind,
                            AsaApi::Tools::Utf8Encode(j.player), AsaApi::Tools::Utf8Encode(j.trait));
    }

    // ---------------------------------------------- v1.9.8 面板文本（机器人推送 → 玩家 trait 5 看）
    // 价目/文案的**唯一权威在机器人侧**（trait_shop.py 读价目表 JSON）；插件只当显示器：
    // 机器人分块推 `Trait 面板文本 <0首块|1续块> <文本>`，插件缓存；玩家 `trait 5` 时用缓存内容弹窗。
    void SetPanelTextChunk(const std::wstring& rest, std::wstring& reply)
    {
        const std::wstring t = Trim(rest);
        const size_t sp = t.find(L' ');
        if (sp == std::wstring::npos)
        {
            reply = L"用法: Trait 面板文本 <0首块|1续块> <文本>";
            return;
        }
        const bool reset = (_wtoi(t.substr(0, sp).c_str()) == 0);   // 0 = 首块 ⇒ 清空重来
        const std::wstring body = t.substr(sp + 1);
        std::wstring out;
        for (size_t i = 0; i < body.size(); ++i)                    // 允许用字面 \n 表示换行
        {
            if (body[i] == L'\\' && i + 1 < body.size() && body[i + 1] == L'n') { out += L'\n'; ++i; }
            else out += body[i];
        }
        if (reset) g_panel_body.clear();
        g_panel_body += out;
        reply = L"OK: panel text " + std::wstring(reset ? L"reset+" : L"append+") +
                std::to_wstring(out.size()) + L" chars, total=" + std::to_wstring(g_panel_body.size());
        Log::GetLog()->info("[ZeroARKTrait] 面板文本 {} {} 字（累计 {} 字）",
                            reset ? "重置" : "追加", out.size(), g_panel_body.size());
    }

    // ---------------------------------------------- v1.10.0 游戏内下单：请求队列（机器人 RCON 轮询拉取）
    // 分工：插件只**入队**；扣点/发货仍由机器人做（点数在 ArkShop 的 MySQL 里，只有机器人够得着）。
    // 为什么用"插件存队列 + 机器人拉取"当回传通道：**游戏服连不到 NAS**（只有 NAS→游戏服单向通），
    // 但**机器人能 RCON 连游戏服** ⇒ 这样零新依赖、也不碰生产聊天库。
    // 请求里带"身边最近那只龙"的物种名 ⇒ 机器人能按真实身价算价（不然只能按默认系数，会少收钱）。
    struct BuyReq
    {
        std::wstring id;
        std::wstring player;    // 游戏内角色名
        std::wstring trait;     // 内部英文名（已 Normalize）
        int tier = 0;
        std::wstring dino;      // 目标龙的显示名（玩家自设名，没设则物种名）
        std::wstring species;   // v1.11.0 物种名（机器人按它算身价 —— 自设名匹配不到身价表）
        unsigned int id1 = 0;   // v1.11.0 目标龙 DinoID（0,0 = 让机器人用该玩家身边最近的）
        unsigned int id2 = 0;
        unsigned long long at = 0;
    };
    std::vector<BuyReq> g_buyreqs;
    unsigned long long g_req_seq = 0;

    std::wstring NextReqId()
    {
        ++g_req_seq;
        return L"Q" + std::to_wstring(GetTickCount64() % 1000000) + L"-" + std::to_wstring(g_req_seq);
    }

    void ReapBuyReqs()          // 丢弃超时（10 分钟）的请求
    {
        const unsigned long long now = GetTickCount64();
        for (size_t i = 0; i < g_buyreqs.size(); )
        {
            if (now - g_buyreqs[i].at > 10ull * 60 * 1000)
            {
                Log::GetLog()->info("[ZeroARKTrait] 购买请求超时丢弃 id={}", AsaApi::Tools::Utf8Encode(g_buyreqs[i].id));
                g_buyreqs.erase(g_buyreqs.begin() + i);
            }
            else ++i;
        }
    }

    // 返回空串 = 入队成功；否则是给玩家的失败原因
    // 数这只龙身上一共有几条词条 —— 引擎对"词条总数已满/物种不支持"这类拒绝
    //   不给原因（实测 2026-09-27：reason 是空串）⇒ 自己数出来，给玩家一句可行动的话
    //   注意：kTraitNames 里有别名行（顽强/坚韧=Tenacious、游泳=Swimmer、吸血=嗜血=Vampiric）
    //   ⇒ 必须按英文名去重，否则同一条词条会被数两次
    int CountTraitsOnDino(APrimalDinoCharacter* dn)
    {
        if (dn == nullptr) return 0;
        UGeneTraitDefinitions* defs = TraitDefs();
        if (defs == nullptr) return 0;
        std::map<std::wstring, int> seen;
        int total = 0;
        for (size_t i = 0; i < _countof(kTraitNames); ++i)
        {
            const std::wstring en = kTraitNames[i].en;
            if (seen.find(en) != seen.end()) continue;   // 别名行跳过
            seen[en] = 1;
            const std::string a = AsaApi::Tools::Utf8Encode(en);
            total += defs->GeneTraits_GetCountOfGeneTraitStacksThisCreatureHas(dn, FName(a.c_str()));
        }
        return total;
    }

    std::wstring EnqueueBuyReq(const std::wstring& player, const std::wstring& trait, int tier,
                               const std::wstring& dino, const std::wstring& species = L"",
                               unsigned int id1 = 0, unsigned int id2 = 0,
                               APrimalDinoCharacter* dinoPtr = nullptr)
    {
        if (player.empty()) return L"读不到你的玩家名";
        if (trait.empty()) return L"词条名不对";
        if (tier < 0 || tier > 2) return L"层级只能是 0/1/2";
        // charge_mode=plugin ⇒ 插件自己算价 + 自己扣点 + 自己按恐龙 ID 发货（全程不经过机器人）
        if (g_charge_mode == L"plugin")
        {
            AShooterPlayerController* pcBuy = FindPlayer(player);
            std::wstring eos;
            if (pcBuy != nullptr) { FString e0 = pcBuy->GetEOSId(); eos = std::wstring(*e0); }
            if (eos.empty()) return Msg(L"notify_offline", L"-", DisplayTraitName(trait));
            const int cost = ComputePrice(trait, tier, species, eos);
            if (cost < 0) return Msg(L"trait_no_price", DisplayTraitName(trait));

            //  v1.23.2（B）：下单前预检—— 引擎先说"能不能加"，能加才扣钱
            //   为什么：老代码只有"扣钱 → 发货失败 → 再退费"这条路 ⇒ 玩家在面板上是
            //   「点一次没反应、扣了又退」，审计里刷 `失败:Adding this trait would exceed max
            //   stack limit`（2026-09-26 19:18-19:20 真机）—— 看着就像"一直在给同一只龙加词条"
            //   ⇒ 现在这几种情况一分钱不扣 + 直接把原因回给玩家 + 下次推数据时带到面板：
            //     ① 引擎说加不上（词条堆叠已满 / 该物种不支持 / 其它）
            //     ② 同一玩家+同一只龙+同一条词条 还有已扣费没发货的单（面板连点两下）
            //   代价：预检通过那一瞬间被别人抢先加满 ⇒ 还是走老路（扣了再退） 钱不会丢
            //    老命令 `Trait 买 <玩家> <词条>`（聊天/机器人）没带 ID ⇒ 这里现取"选定的那只 / 最近的"
            {
                std::wstring why;
                APrimalDinoCharacter* pre = nullptr;
                int have_all = -1, max_all_pre = -1;   // 词条总数 / 上限（拒绝时给玩家看）
                // 带 id 的（= 面板单）不许退化到"最近的龙"——
                //   解析不到那只龙就直接拒（不然钱按 A 龙收、货发到 B 龙）
                const bool noFallback = (dinoPtr != nullptr) || (id1 != 0);
                if (dinoPtr != nullptr)        pre = dinoPtr;
                else if (id1 != 0)             pre = FindDinoById(id1, id2);
                if (pre == nullptr && pcBuy != nullptr && !noFallback)
                {
                    DinoRef selPre;
                    const std::wstring mePre = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pcBuy));
                    if (GetSelection(mePre, selPre) && selPre.id1 != 0) pre = FindDinoById(selPre.id1, selPre.id2);
                    if (pre == nullptr) pre = NearestDino(pcBuy, g_cfg.near_radius);
                }
                if (pre == nullptr)
                {
                    why = Msg(L"pre_no_dino");
                }
                else
                {
                    UGeneTraitDefinitions* defsPre = TraitDefs();
                    if (defsPre != nullptr)
                    {
                        FString reason;
                        const std::string ta = AsaApi::Tools::Utf8Encode(trait);
                        if (!defsPre->GeneTraits_CanTraitBeAdded(pre, FName(ta.c_str()), &reason, false))
                        {
                            const std::wstring eng(*reason);
                            if (ContainsI(eng, L"max stack"))
                                why = Msg(L"pre_stack_full", DisplayTraitName(trait));
                            else if (eng.empty())
                            {
                                // 引擎对"词条总数已满 / 该物种不支持"不给原因
                                //   ⇒ 自己数一遍，把能照着做的结论给玩家 （面板单会走 Say 直接显示）
                                have_all = CountTraitsOnDino(pre);
                                max_all_pre = defsPre->GeneTraits_GetMaxAllowedTraitsForThisCreature(pre);
                                // 下面这几句原来硬编码中文 ⇒ 改用消息键（跟着 msg_lang 走）
                                if (max_all_pre > 0 && have_all >= max_all_pre)
                                    why = Msg(L"pre_trait_full", std::to_wstring(have_all), std::to_wstring(max_all_pre));
                                else
                                    why = Msg(L"pre_engine_no_reason", std::to_wstring(have_all), std::to_wstring(max_all_pre));
                            }
                            else
                                why = Msg(L"pre_engine_reason", eng);
                        }
                    }
                    if (why.empty())
                    {
                        const unsigned int k2 = (id2 != 0) ? id2 : pre->DinoID2Field();
                        const std::wstring pk = PendingKey(eos, trait, pre->DinoID1Field(), k2);
                        int pendingNow = 0;
                        { std::lock_guard<std::mutex> lk(g_push_mu); pendingNow = g_pending_charge[pk]; }
                        if (pendingNow > 0)
                            why = Msg(L"pre_pending");
                    }
                }
                if (!why.empty())
                {
                    SetPendingError(eos, why);   // 下次推商店数据时搭 ||ERR| 回面板
                    AuditLog(L"下单预检拒绝 | 玩家=" + player + L" | 词条=" + trait + L"[" +
                             std::to_wstring(tier) + L"] | 龙=" + dino + L" | 原因=" + why);
                    Log::GetLog()->info("[ZeroARKTrait] pre-check REJECT player={} trait={} tier={} dino={} traits={}/{} reason={}",
                                        AsaApi::Tools::Utf8Encode(player), AsaApi::Tools::Utf8Encode(trait),
                                        tier, AsaApi::Tools::Utf8Encode(dino), have_all, max_all_pre,
                                        AsaApi::Tools::Utf8Encode(why));
                    return Msg(L"buy_fail", why);
                }
            }

            std::wstring cerr;
            if (!ChargePoints(eos, cost, &cerr))
            {
                // ChargePoints 给的原因是硬编码中文 （msg_lang=en 时会"中英混着"）
                //   ⇒ 这里统一换成本地化文案 （认不出来就退回通用"扣费失败"）
                std::wstring whyC = Msg(L"charge_fail");
                if (ContainsI(cerr, L"点数不足"))          whyC = Msg(L"charge_no_points");
                else if (ContainsI(cerr, L"读不到"))       whyC = Msg(L"charge_no_eos");
                else if (ContainsI(cerr, L"数据库连不上")) whyC = Msg(L"charge_db_down");
                else if (ContainsI(cerr, L"查点数失败"))   whyC = Msg(L"charge_db_query");
                return Msg(L"buy_no_points", std::to_wstring(cost), whyC);
            }
            //  v1.23.2（C）：占住"这一单还没发货"的位置 ⇒ 面板连点第二下会被上面预检拦住
            { std::lock_guard<std::mutex> lk(g_push_mu); ++g_pending_charge[PendingKey(eos, trait, id1, id2)]; }

            const std::wstring oid = L"PLUGIN-" + std::to_wstring(GetTickCount64() % 100000000ULL);
            Job jj;
            jj.kind = 1;                       // 加词条（带订单号 ⇒ 走审计与"结果必推"那条路）
            jj.player = player;
            jj.trait = trait;
            jj.tier = tier;
            jj.order = oid;
            jj.eos = eos;      // 记下来，发货失败好退费
            jj.cost = cost;    //  v1.20.2
            //  v1.23.2（A）：两个 flag 一个都不能少 —— 它们才是"按 ID 发货"的开关
            if (id1 != 0) {
                jj.id1 = id1; jj.id2 = id2;
                jj.by_id = true;
                jj.by_name = false;
            }
            {
                std::lock_guard<std::mutex> lock(g_mu);
                g_jobs.push_back(jj);
            }
            AuditLog(L"插件扣费 | 订单=" + oid + L" | 玩家=" + player + L" | 词条=" + trait +
                     L"[" + std::to_wstring(tier) + L"] | 实付=" + std::to_wstring(cost) +
                     L" | 点数模式=" + g_shop_points_mode);
            Log::GetLog()->info("[ZeroARKTrait] plugin-charge OK: player={} trait={} tier={} cost={} mode={}",
                                AsaApi::Tools::Utf8Encode(player), AsaApi::Tools::Utf8Encode(trait), tier, cost,
                                AsaApi::Tools::Utf8Encode(g_shop_points_mode));
            return L"";
        }
        ReapBuyReqs();
        for (size_t i = 0; i < g_buyreqs.size(); ++i)
            if (_wcsicmp(g_buyreqs[i].player.c_str(), player.c_str()) == 0)
                return L"你还有一条请求没处理完，等它好了再发";
        if (g_buyreqs.size() >= 30) return L"当前排队太多，稍后再试";
        BuyReq r;
        r.id = NextReqId();
        r.player = player;
        r.trait = trait;
        r.tier = tier;
        r.dino = dino;
        r.species = species;
        r.id1 = id1;
        r.id2 = id2;
        r.at = GetTickCount64();
        g_buyreqs.push_back(r);
        Log::GetLog()->info("[ZeroARKTrait] 购买请求入队 id={} player={} trait={} tier={} dino={} species={} id={}:{}",
                            AsaApi::Tools::Utf8Encode(r.id), AsaApi::Tools::Utf8Encode(player),
                            AsaApi::Tools::Utf8Encode(trait), tier, AsaApi::Tools::Utf8Encode(dino),
                            AsaApi::Tools::Utf8Encode(species), id1, id2);
        AuditLog(L"购买请求 | id=" + r.id + L" | 玩家=" + player + L" | 词条=" + trait +
                 L"[" + std::to_wstring(tier) + L"] | 龙=" + dino + L" | 物种=" + species +
                 L" | DinoID=" + std::to_wstring(id1) + L":" + std::to_wstring(id2));
        return L"";
    }

    // 机器人拉取：`Trait 取单` → 每行 `REQ|id|player|trait|tier|dino|age秒|species|id1|id2`；空队列回 `REQ|none`
    // 在**末尾追加** species/id1/id2 —— 机器人旧解析按前 7 个字段取，追加不破坏兼容
    std::wstring BuyReqListText()
    {
        ReapBuyReqs();
        if (g_buyreqs.empty()) return L"REQ|none";
        std::wstring out;
        const unsigned long long now = GetTickCount64();
        for (size_t i = 0; i < g_buyreqs.size(); ++i)
        {
            const BuyReq& r = g_buyreqs[i];
            if (!out.empty()) out += L"\n";
            out += L"REQ|" + r.id + L"|" + r.player + L"|" + r.trait + L"|" + std::to_wstring(r.tier) +
                   L"|" + r.dino + L"|" + std::to_wstring((now - r.at) / 1000) +
                   L"|" + r.species + L"|" + std::to_wstring(r.id1) + L"|" + std::to_wstring(r.id2);
        }
        return out;
    }

    // 机器人回执：`Trait 单结 <id> [fail]`
    bool AckBuyReq(const std::wstring& id, bool ok, std::wstring& reply)
    {
        for (size_t i = 0; i < g_buyreqs.size(); ++i)
        {
            if (g_buyreqs[i].id == id)
            {
                AuditLog(std::wstring(ok ? L"购买请求完成 | id=" : L"购买请求失败 | id=") + id +
                         L" | 玩家=" + g_buyreqs[i].player + L" | 词条=" + g_buyreqs[i].trait +
                         L"[" + std::to_wstring(g_buyreqs[i].tier) + L"]");
                reply = std::wstring(L"OK: acked ") + (ok ? L"done " : L"failed ") + id;
                g_buyreqs.erase(g_buyreqs.begin() + i);
                return true;
            }
        }
        reply = L"没有这个请求 id：" + id;
        return false;
    }

    // ASCII 命令名 + 中文动作词（ArkApi 的 RCON 派发匹配不了非 ASCII 命令名，所以中文只能放在参数里）
    void EnqueueDispatch(const std::wstring& rest, std::wstring& reply)
    {
        std::vector<std::wstring> t;
        Split(Trim(rest), t);
        if (t.empty())
        {
            reply = HelpText();
            return;
        }

        const std::wstring a = t[0];
        std::wstring tail;
        for (size_t i = 1; i < t.size(); ++i)
        {
            if (i > 1) tail += L" ";
            tail += t[i];
        }

        // 动作词：英文 / 拼音 / 中文（中文只在游戏内控制台可用——ArkApi 的 RCON 通道是 ASCII-only）
        if (a == L"加" || a == L"添加" || a == L"add" || a == L"Add" || a == L"jia" || a == L"Jia")
            Enqueue(1, tail, reply);
        else if (a == L"删" || a == L"删除" || a == L"del" || a == L"Del" || a == L"shan" || a == L"Shan")
            Enqueue(2, tail, reply);
        else if (a == L"列表" || a == L"查看" || a == L"list" || a == L"List" || a == L"liebiao" || a == L"Liebiao")
            Enqueue(3, tail, reply);
        else if (a == L"订单" || a == L"dingdan" || a == L"order" || a == L"Order")
            Enqueue(5, tail, reply);   // 机器人下单：订单号 + 目标 + 词条 + 层级
        else if (a == L"礼包" || a == L"libao" || a == L"pack" || a == L"Pack")
            Enqueue(6, tail, reply);   // 词条礼包：目标 + 词条1,词条2,…
        else if (a == L"查单" || a == L"chadan" || a == L"query" || a == L"Query")
            Enqueue(7, tail, reply);   // 按订单号回查审计
        else if (a == L"无敌" || a == L"wudi" || a == L"god" || a == L"God")
            Enqueue(11, tail, reply);  // v1.9.2 给玩家开无敌 + 无限状态
        else if (a == L"面板" || a == L"mianban" || a == L"panel" || a == L"Panel" || a == L"ui" || a == L"UI")
            Enqueue(12, tail, reply);  // v1.9.7 面板：给该玩家弹客户端对话框（面板地基）
        else if (a == L"龙" || a == L"龙列表" || a == L"long" || a == L"Long" || a == L"near" || a == L"Near")
            Enqueue(13, tail, reply);  // v1.11.0 附近已驯服的龙（距离/等级/名字/DinoID；带 zs = 结构化行）
        else if (a == L"面板文本" || a == L"paneltext" || a == L"paneltxt")
            SetPanelTextChunk(tail, reply);   // v1.9.8 机器人推送面板正文（同步处理，不进队列）
        // ---- v1.12.0 M4b：把"价目表"缓存下来，等玩家请求时下发给客户端 mod ----
        else if (a == L"缓存价目" || a == L"pricecache" || a == L"pricecache0")
        {
            CacheShopPrice(tail);
            reply = L"OK: price cached";
            return;
        }
        // ---- v1.15.0：发放点数（没有 ArkShop 的服也能用；只注册在 RCON/控制台）----
        //   用法：Trait 设点数 <EOS ID> <点数>   /   Trait 加点 <EOS ID> <增量>
        else if (a == L"设点数" || a == L"sheDianShu" || a == L"shedianshu" ||
                 a == L"setpoints" || a == L"setpoint" ||
                 a == L"加点" || a == L"jiaDian" || a == L"jiadian" ||
                 a == L"addpoints" || a == L"addpoint")
        {
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.size() < 2) { reply = L"用法: Trait 设点数 <EOS ID> <点数>  ／  Trait 加点 <EOS ID> <增量>"; return; }
            const std::wstring eos = tt[0];
            const int n = _wtoi(tt[1].c_str());
            const bool isAdd = (a == L"加点" || a == L"jiaDian" || a == L"jiadian" ||
                                a == L"addpoints" || a == L"addpoint");
            // points_mode=file ⇒ 直接改插件目录里的本地点数文件（零依赖）
            if (g_shop_points_mode == L"file")
            {
                int now = 0;
                const bool okf = isAdd ? FilePointsAdd(eos, n, &now)
                                       : FilePointsAdd(eos, n - FilePointsOf(eos), &now);
                reply = okf ? ((isAdd ? L"OK: added " : L"OK: set to ") + std::to_wstring(n) +
                               L" ⇒ now " + std::to_wstring(now) + L"  (file mode)")
                            : L"ERR: file points update failed（会扣成负数？）";
                return;
            }
            minimysql::Conn c;
            std::string e0;
            if (!g_db_table_ready) { std::string e1; if (EnsureDbTable(e1)) g_db_table_ready = true; }
            if (!c.Open(Narrow(g_db_host), g_db_port, Narrow(g_db_user), Narrow(g_db_pass), Narrow(g_db_name), e0))
            {
                reply = L"ERR: db connect failed: " + AsaApi::Tools::Utf8Decode(e0);
                return;
            }
            const std::string tbl = Narrow(g_db_table), key = Narrow(g_db_key), val = Narrow(g_db_val);
            const std::string eosN = Narrow(eos);
            std::string e2;
            std::string sql;
            if (isAdd)
                sql = "INSERT INTO `" + tbl + "` (`" + key + "`, `" + val + "`) VALUES ('" + eosN + "', " + std::to_string(n)
                    + ") ON DUPLICATE KEY UPDATE `" + val + "` = `" + val + "` + " + std::to_string(n);
            else
                sql = "INSERT INTO `" + tbl + "` (`" + key + "`, `" + val + "`) VALUES ('" + eosN + "', " + std::to_string(n)
                    + ") ON DUPLICATE KEY UPDATE `" + val + "` = " + std::to_string(n);
            if (!c.Exec(sql, e2)) { c.Close(); reply = L"ERR: " + AsaApi::Tools::Utf8Decode(e2); return; }
            c.Close();
            { std::lock_guard<std::mutex> lock(g_db_mu); g_db_points_cache.erase(eos); }
            int now = 0;
            if (FetchPointsFromDb(eos, now))
                reply = (isAdd ? L"OK: added " : L"OK: set to ") + std::to_wstring(n) + L" ⇒ now " + std::to_wstring(now);
            else
                reply = L"OK: written（但回读失败，看日志）";
            return;
        }
        // ---- v1.15.0：测试"插件自己读数据库"（不用等玩家上线）----
        //   用法：Trait 查点数 <EOS ID>     例：Trait 查点数 <你的EOS 32位hex>
        else if (a == L"查点数" || a == L"chaDianShu" || a == L"chadianshu" ||
                 a == L"dbpoints" || a == L"dbpoint" || a == L"getpoints" || a == L"getpoint")
        {
            const std::wstring eos = Trim(tail);
            if (eos.empty()) { reply = L"用法: Trait 查点数 <EOS ID>（mode=" + g_shop_points_mode + L"）"; return; }
            // file 模式 ⇒ 直接读本地点数文件
            if (g_shop_points_mode == L"file")
            {
                // 允许用角色名查（旧版按名字查恒回 0 —— file 表的键是 EOS）
                //   不是 32 位 hex 就当成角色名 ⇒ 经在线玩家映射出 EOS
                std::wstring key = eos;
                bool hex32 = (key.size() == 32);
                for (size_t i = 0; i < key.size() && hex32; ++i)
                {
                    const wchar_t c = key[i];
                    const bool okc = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
                    if (!okc) hex32 = false;
                }
                if (!hex32)
                {
                    AShooterPlayerController* pc_n = FindPlayer(key);
                    if (pc_n != nullptr) { FString e0 = pc_n->GetEOSId(); key = std::wstring(*e0); }
                    else { reply = L"ERR: 既不是 32 位 EOS，也不是在线玩家名：" + eos; return; }
                }
                reply = L"OK: file points = " + std::to_wstring(FilePointsOf(key)) + L"  (ZeroARKTrait-points.txt)";
                return;
            }
            int pts = 0;
            // 绕过缓存直查一次
            { std::lock_guard<std::mutex> lock(g_db_mu); g_db_points_cache.erase(eos); }
            if (FetchPointsFromDb(eos, pts))
                reply = L"OK: db points = " + std::to_wstring(pts) + L"  (" + g_db_host + L"/" + g_db_name + L"." + g_db_table + L"." + g_db_val + L")";
            else
                reply = L"ERR: db read failed（看 ArkApi 日志里的 db connect/query failed 行；mode=" + g_shop_points_mode + L"）";
            return;
        }
        // ---- v1.15.0：删除某个点数记录（清测试残留 / 清理账号用）----
        else if (a == L"删点数" || a == L"shanDianShu" || a == L"shandianshu" ||
                 a == L"delpoints" || a == L"delpoint" || a == L"rmpoints")
        {
            const std::wstring eos = Trim(tail);
            if (eos.empty()) { reply = L"用法: Trait 删点数 <EOS ID>"; return; }
            // file 模式 ⇒ 从本地点数文件里删掉这条
            if (g_shop_points_mode == L"file")
            {
                reply = FilePointsErase(eos) ? (L"OK: deleted file points row for " + eos)
                                             : (L"OK: no such row in file (" + eos + L")");
                return;
            }
            minimysql::Conn c;
            std::string e0;
            if (!g_db_table_ready) { std::string e1; if (EnsureDbTable(e1)) g_db_table_ready = true; }
            if (!c.Open(Narrow(g_db_host), g_db_port, Narrow(g_db_user), Narrow(g_db_pass), Narrow(g_db_name), e0))
            { reply = L"ERR: db connect failed: " + AsaApi::Tools::Utf8Decode(e0); return; }
            std::string e2;
            if (!c.Exec("DELETE FROM `" + Narrow(g_db_table) + "` WHERE `" + Narrow(g_db_key) + "`='" + Narrow(eos) + "'", e2))
            { c.Close(); reply = L"ERR: " + AsaApi::Tools::Utf8Decode(e2); return; }
            c.Close();
            { std::lock_guard<std::mutex> lock(g_db_mu); g_db_points_cache.erase(eos); }
            reply = L"OK: deleted points row for " + eos;
            return;
        }
        // ---- v1.16.2：查某个玩家的权限组 + 折扣 + 样例价（管理员排查 / 自测用）----
        //   用法：Trait 查折扣 <EOS ID>
        else if (a == L"查折扣" || a == L"chaZheKou" || a == L"chazhekou" ||
                 a == L"discount" || a == L"getdiscount")
        {
            const std::wstring eos = Trim(tail);
            if (eos.empty()) { reply = L"用法: Trait 查折扣 <EOS ID>"; return; }
            std::vector<std::wstring> gs = PermGroups(eos);      // 直查一次（不走 30 秒缓存）
            std::wstring glist;
            for (size_t i = 0; i < gs.size(); ++i) { if (i) glist += L","; glist += gs[i]; }
            const float disc = DiscountFor(gs);
            const int p800 = ComputePriceWith(L"嗜血", 0, L"", gs);  // 样例：嗜血 1 级（基础价 800）
                                                                     // 用同一份直查的组算 ⇒ 和左边那栏同源
            reply = L"groups=[" + (glist.empty() ? std::wstring(L"无") : glist) + L"]"
                  + L"  discount=" + std::to_wstring((int)disc) + L"%"
                  + L"  mode=" + g_shop_discount_mode
                  + L"  perm_ok=" + ((g_perm_groups != nullptr) ? L"1" : L"0")
                  + L"  prices=" + std::to_wstring(g_shop_discount_table.size())
                  + L"  嗜血1级=" + ((p800 < 0) ? std::wstring(L"未定价") : std::to_wstring(p800));
            return;
        }
        // ---- v1.15.0：看当前商店配置（价目/物种表/系数）----
        else if (a == L"商店状态" || a == L"shangDianZhuangTai" || a == L"shangdianzhuangtai" ||
                 a == L"shopinfo" || a == L"shopstatus" || a == L"dbstatus")
        {
            reply = L"mode=" + g_shop_points_mode
                  + L"  charge=" + g_charge_mode
                  + L"  prices=" + std::to_wstring(g_shop_price_table.size())
                  + L"  species=" + std::to_wstring(g_shop_species_table.size())
                  + L"  tier=" + std::to_wstring((int)g_shop_tier_coef[0]) + L"/"
                  + std::to_wstring(g_shop_tier_coef[1]) + L"/" + std::to_wstring(g_shop_tier_coef[2])
                  + L"  discounts=" + std::to_wstring(g_shop_discount_table.size()) + L"/" + g_shop_discount_mode
                  + L"  msgs=" + std::to_wstring(g_msgs.size()) + L"/" + g_msg_lang
                  + L"  traitlang=" + g_shop_trait_lang
                  + L"  ui=" + (g_ui_enabled ? L"1" : L"0")
                  + L" order=" + (g_order_enabled ? L"1" : L"0")
                  + L"  shopname=" + ((g_msgs.find(L"shop_name") != g_msgs.end()) ? g_msgs[L"shop_name"] : std::wstring(L"-"))
                  + (g_shop_points_mode == L"file"
                        ? (L"  file_points=" + std::to_wstring(g_file_points.size()) + L" (ZeroARKTrait-points.txt)")
                        : (L"  db=" + g_db_host + L"/" + g_db_name + L"/" + g_db_table));
            return;
        }
        // ---- v1.23.2（D）：只读预检（管理员排查"面板下单为什么被拒"用）----
        //   `Trait 预检 <玩家> <词条> [级别1/2/3] [id:<id1>:<id2>]`
        //   报告：选定/最近的龙 + 引擎的 CanTraitBeAdded 判决与原文原因 + 已堆叠数量 + 总量上限
        else if (a == L"预检" || a == L"yujian" || a == L"precheck" || a == L"PreCheck")
        {
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.size() < 2) { reply = L"用法: Trait 预检 <玩家> <词条> [级别1/2/3] [id:<id1>:<id2>]"; return; }
            std::wstring tr;
            int tier = 0;
            ParseTraitToken(tt[1], tr, tier);
            if (tt.size() >= 3)
            {
                const std::wstring& t2 = tt[2];
                if (t2 == L"1" || t2 == L"2" || t2 == L"3")
                {
                    tier = _wtoi(t2.c_str()) - 1;
                    if (tier < 0) tier = 0;
                    if (tier > 2) tier = 2;
                }
            }
            tr = NormalizeTraitName(tr);
            AShooterPlayerController* pc_pk = FindPlayer(tt[0]);
            if (pc_pk == nullptr) { reply = L"FAILED: player not found: " + tt[0]; return; }
            APrimalDinoCharacter* dn_pk = nullptr;
            for (size_t i = 2; i < tt.size(); ++i)
            {
                if (tt[i].compare(0, 3, L"id:") != 0) continue;
                // 同上 —— 面板拼的 id 带千位分隔符（476,676,959）⇒ 只认数字字符
                {
                    const std::wstring rest = tt[i].substr(3);
                    std::wstring na, nb;
                    bool second = false;
                    for (size_t k = 0; k < rest.size(); ++k)
                    {
                        const wchar_t ch = rest[k];
                        if (ch == L':') { second = true; continue; }
                        if (ch >= L'0' && ch <= L'9') { if (second) nb.push_back(ch); else na.push_back(ch); }
                    }
                    if (!na.empty()) dn_pk = FindDinoById((unsigned int)_wtoi64(na.c_str()),
                                                          (unsigned int)_wtoi64(nb.c_str()));
                }
            }
            if (dn_pk == nullptr) dn_pk = NearestDino(pc_pk, g_cfg.near_radius);
            if (dn_pk == nullptr) { reply = L"FAILED: no dino near that player"; return; }
            UGeneTraitDefinitions* defs_pk = TraitDefs();
            if (defs_pk == nullptr) { reply = L"FAILED: no PrimalGameData"; return; }
            const std::string ta_pk = AsaApi::Tools::Utf8Encode(tr);
            FString fr_pk;
            const bool can_pk = defs_pk->GeneTraits_CanTraitBeAdded(dn_pk, FName(ta_pk.c_str()), &fr_pk, false);
            const int have_pk = defs_pk->GeneTraits_GetCountOfGeneTraitStacksThisCreatureHas(
                                    dn_pk, FName(ta_pk.c_str()));
            const int max_pk = defs_pk->GeneTraits_GetMaxAllowedTraitsForThisCreature(dn_pk);
            const int all_pk = CountTraitsOnDino(dn_pk);                                   // 这条龙现在一共几条词条
            reply = L"precheck=" + std::wstring(can_pk ? L"OK" : L"NO")
                  + L"  dino=" + DinoNameOnly(dn_pk) + L" (" + std::to_wstring(dn_pk->DinoID1Field())
                  + L":" + std::to_wstring(dn_pk->DinoID2Field()) + L")"
                  + L"  trait=" + tr + L"[" + std::to_wstring(tier) + L"]"
                  + L"  stacks=" + std::to_wstring(have_pk) + L"/max_total=" + std::to_wstring(max_pk)
                  + L"  traits=" + std::to_wstring(all_pk) + L"  reason=" + (can_pk ? std::wstring(L"-") : std::wstring(*fr_pk));
            return;
        }
        // ---- v1.13.0 M4c：把"在线玩家余额表"缓存下来（按玩家下发点数用）----
        // 客户端一开面板就主动"要一次数据"（mod 侧 Execute Console Command 触发）
        // 客户端一开面板就主动"要一次数据"（mod 侧 Execute Console Command 触发）
        //   为什么必须这样：插件无法知道"面板什么时候被建出来"⇒ 任何按时间推都会错过
        //   ⇒ 照 ArkShopUI 的 ROS_ 模式：由客户端主动要 （我们借现成的控制台通道）
        else if (a == L"推商店数据" || a == L"tuiShangDianShuJu" || a == L"tuishangdianshuju" ||
                 a == L"pushshopdata" || a == L"pushdata")
        {
            // 玩家名允许带空格 ⇒ 整段 tail 当名字，不再 Split 取第一个词
            //   为什么：Split 后 "Dark Knight" 只剩 "Dark"，而 FindPlayer 会退到
            //   FindPlayerFromCharacterName(非精确) ⇒ 可能认错人把数据推给别的玩家
            const std::wstring who = Trim(tail);
            if (who.empty()) { reply = L"用法: Trait 推商店数据 <玩家名>"; return; }
            AShooterPlayerController* pc_p = FindPlayer(who);
            if (pc_p == nullptr) { reply = L"FAILED: player not found: " + who; return; }
            const std::wstring eos_p(*pc_p->GetEOSId());
            { std::lock_guard<std::mutex> lk(g_push_mu); g_force_push_left[eos_p] = 3; }      // 无视去重，连推 3 次（≈6 秒）⇒ 新面板必然接住
            g_shop_push_last = 0;              // 下一个 tick 立刻推，不等 2 秒周期
            Log::GetLog()->info("[ZeroARKTrait] client asked for shop data: {} (force push x3)", AsaApi::Tools::Utf8Encode(who));
            reply = L"OK: pushing shop data to " + who + L" x3";
            return;
        }
        else if (a == L"缓存点数" || a == L"pointscache" || a == L"pointcache")
        {
            CachePlayerPoints(tail);
            reply = L"OK: player points updated (" + std::to_wstring(g_player_points.size() + g_player_points_eos.size()) + L")";
            return;
        }
        // ---- v1.23.21： 面板报价（设计口径：**价格一律由插件算，面板只显示**）----
        //   用法（客户端 mod 走 Execute Console Command 触发，和 `推商店数据` 同一条通道）：
        //     Trait 算价 <玩家> <词条> [级别1/2/3] [id:<DinoID1>:<DinoID2>]
        //   做的事：定位那只龙（优先显式 id）→ 用扣费的同一个 ComputePrice算实付
        //   → 把整句（龙名/词条/等级/价，全部插件口径）推回面板的 `Text_Selection`
        //   ⇒ 面板不再自己算 ⇒ "面板显示的数" 与 "实际扣的数" 永远同一个来源
        else if (a == L"算价" || a == L"suanjia" || a == L"price" || a == L"Price" ||
                 a == L"quote" || a == L"Quote")
        {
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.size() < 2) { reply = L"用法: Trait 算价 <玩家> <词条> [级别1/2/3] [id:<id1>:<id2>]"; return; }
            // token 切法完全照抄 `买`（先摘 id:、再看尾部是不是级别、其余合并成玩家名）
            std::wstring idTok;
            {
                std::vector<std::wstring> keep;
                keep.reserve(tt.size());
                for (size_t i = 0; i < tt.size(); ++i)
                {
                    if (tt[i].compare(0, 3, L"id:") == 0) { idTok = tt[i]; continue; }
                    keep.push_back(tt[i]);
                }
                tt.swap(keep);
            }
            //  级别只认 1/2/3 —— 越界数字不能被"名字合并"吞掉
            //   实测（2026-09-28 验收）：`买 123 迟钝 9 id:…` 被切成玩家="123 迟钝"、词条="9"
            //   ⇒ 报"面板没把龙 ID 带上来"（完全误导）⇒ 这里先明确拒掉
            if (tt.size() >= 3)
            {
                const std::wstring& lastTok = tt.back();
                bool allDigits = !lastTok.empty();
                for (size_t i = 0; i < lastTok.size(); ++i)
                    if (lastTok[i] < L'0' || lastTok[i] > L'9') { allDigits = false; break; }
                if (allDigits && lastTok != L"1" && lastTok != L"2" && lastTok != L"3")
                {
                    reply = L"ERR: 级别只能是 1/2/3（收到的是 " + lastTok + L"）";
                    return;
                }
            }
            {
                int tailCount = 1;
                if (tt.size() >= 3) { const std::wstring& lastTok = tt.back(); if (lastTok == L"1" || lastTok == L"2" || lastTok == L"3") tailCount = 2; }
                if ((int)tt.size() > tailCount + 1)
                {
                    const size_t paramFrom = tt.size() - (size_t)tailCount;
                    std::wstring nm = tt[0];
                    for (size_t i = 1; i < paramFrom; ++i) nm += L" " + tt[i];
                    std::vector<std::wstring> nt;
                    nt.push_back(nm);
                    for (size_t i = paramFrom; i < tt.size(); ++i) nt.push_back(tt[i]);
                    tt.swap(nt);
                }
            }
            std::wstring tr;
            int tier = 0;
            ParseTraitToken(tt[1], tr, tier);
            if (tt.size() >= 3) { tier = _wtoi(tt[2].c_str()) - 1; if (tier < 0) tier = 0; if (tier > 2) tier = 2; }
            tr = NormalizeTraitName(tr);

            unsigned int want1 = 0, want2 = 0;
            if (!idTok.empty())
            {
                const std::wstring rest = idTok.substr(3);
                std::wstring na, nb;
                bool second = false;
                for (size_t k = 0; k < rest.size(); ++k)
                {
                    const wchar_t ch = rest[k];
                    if (ch == L':') { second = true; continue; }
                    if (ch >= L'0' && ch <= L'9') { if (second) nb.push_back(ch); else na.push_back(ch); }
                }
                if (!na.empty()) want1 = (unsigned int)_wtoi64(na.c_str());
                if (!nb.empty()) want2 = (unsigned int)_wtoi64(nb.c_str());
            }

            AShooterPlayerController* pc_q = FindPlayer(tt[0]);
            if (pc_q == nullptr) { reply = L"FAILED: player not found: " + tt[0]; return; }
            const std::wstring eos_q(*pc_q->GetEOSId());

            std::wstring dinoName, species;
            if (want1 != 0)
            {
                APrimalDinoCharacter* d_q = FindDinoById(want1, want2);
                if (d_q == nullptr) { reply = L"ERR: dino not found"; return; }
                dinoName = DinoNameOnly(d_q);
                species = DinoSpecies(d_q);
            }
            else
            {
                // 没带 id ⇒ 退到"选定 / 身边最近"（与下单同规则）
                const std::wstring me_q = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc_q));
                DinoRef sel_q;
                if (GetSelection(me_q, sel_q) && sel_q.id1 != 0) { dinoName = sel_q.name; species = sel_q.species; }
                else
                {
                    APrimalDinoCharacter* dn_q = NearestDino(pc_q, g_cfg.near_radius);
                    if (dn_q != nullptr) { dinoName = DinoNameOnly(dn_q); species = DinoSpecies(dn_q); }
                }
            }
            const int price = ComputePrice(tr, tier, species, eos_q);
            if (price < 0) { reply = L"ERR: no price for this trait"; return; }
            const std::wstring line = Msg(L"sel_line", dinoName.empty() ? species : dinoName,
                                          DisplayTraitName(tr), std::to_wstring(tier + 1),
                                          std::to_wstring(price));
            const bool okPush = PushPanelUi(pc_q, L"Text_Selection=" + line + L"|PRICE=" + std::to_wstring(price));
            Log::GetLog()->info("[ZeroARKTrait] quote eos={} dino=[{}] species=[{}] trait={} tier={} price={} pushed={}",
                                AsaApi::Tools::Utf8Encode(eos_q), AsaApi::Tools::Utf8Encode(dinoName),
                                AsaApi::Tools::Utf8Encode(species), AsaApi::Tools::Utf8Encode(tr), tier + 1, price, okPush);
            reply = L"OK: price=" + std::to_wstring(price);
            return;
        }
        // ---- v1.10.0 游戏内下单：入队 / 拉取 / 回执 / 提示 ----
        else if (a == L"买" || a == L"mai" || a == L"buy" || a == L"Buy")
        {
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.size() < 2) { reply = L"用法: Trait 买 <玩家> <词条> [级别1/2/3]"; return; }
            //  切 token 的正确顺序（v1.23.1 → v1.23.3 两次都栽在这里）
            //   ① 先把可选参数 `id:<id1>:<id2>`摘出来单独留着（它不该参与名字合并）
            //   ② 再看剩下的尾巴是不是级别 1/2/3⇒ 是就把它也留作参数
            //   ③ 其余全部 token 合并成一个"玩家名"（名字没空格时行为与旧版完全一致）
            //   实测教训：不摘 id ⇒ `买 玩家 词条 1 id:…` 会被切成名字="玩家 词条 1"、词条=id 串
            //   ⇒ 回一句"你当时不在线"，看着像掉线，其实解析全错 （2026-09-26 19:46 BUY diag 铁证）
            std::wstring idTok;
            {
                std::vector<std::wstring> keep;
                keep.reserve(tt.size());
                for (size_t i = 0; i < tt.size(); ++i)
                {
                    if (tt[i].compare(0, 3, L"id:") == 0) { idTok = tt[i]; continue; }   // 摘出可选参数
                    keep.push_back(tt[i]);
                }
                tt.swap(keep);
            }
            //  级别只认 1/2/3 —— 越界数字不能被"名字合并"吞掉
            //   实测（2026-09-28 验收）：`买 123 迟钝 9 id:…` 被切成玩家="123 迟钝"、词条="9"
            //   ⇒ 报"面板没把龙 ID 带上来"（完全误导）⇒ 这里先明确拒掉
            if (tt.size() >= 3)
            {
                const std::wstring& lastTok = tt.back();
                bool allDigits = !lastTok.empty();
                for (size_t i = 0; i < lastTok.size(); ++i)
                    if (lastTok[i] < L'0' || lastTok[i] > L'9') { allDigits = false; break; }
                if (allDigits && lastTok != L"1" && lastTok != L"2" && lastTok != L"3")
                {
                    reply = L"ERR: 级别只能是 1/2/3（收到的是 " + lastTok + L"）";
                    return;
                }
            }
            {
                int tailCount = 1;
                if (tt.size() >= 3) { const std::wstring& lastTok = tt.back(); if (lastTok == L"1" || lastTok == L"2" || lastTok == L"3") tailCount = 2; }
                if ((int)tt.size() > tailCount + 1)
                {
                    const size_t paramFrom = tt.size() - (size_t)tailCount;
                    std::wstring nm = tt[0];
                    for (size_t i = 1; i < paramFrom; ++i) nm += L" " + tt[i];
                    std::vector<std::wstring> nt;
                    nt.push_back(nm);
                    for (size_t i = paramFrom; i < tt.size(); ++i) nt.push_back(tt[i]);
                    tt.swap(nt);
                }
            }
            if (!idTok.empty()) tt.push_back(idTok);   // 放回尾部 ⇒ 后面那段 id 解析原样可用
            std::wstring tr;
            int tier = 0;
            ParseTraitToken(tt[1], tr, tier);
            // 修正：这里的第 3 个参数是玩家看到的 1/2/3 级⇒ 内部 tier 要 -1 （原来直接赋值 ⇒ 差一级）
            if (tt.size() >= 3) { tier = _wtoi(tt[2].c_str()) - 1; if (tier < 0) tier = 0; if (tier > 2) tier = 2; }
            tr = NormalizeTraitName(tr);
            // 控制台来源（客户端 mod 调用）也要带上"该玩家选定 / 最近的龙 + 物种名 + DinoID"
            //   原来固定传 L"" ⇒ ComputePrice 拿不到物种名（按默认系数 ⇒ 少收钱）、jj.id1 为 0（发货定位不到指定龙）。
            //   这里的逻辑与聊天入口 `trait 买 <词条> [级别]` 完全一致 （符号都在本行之前声明）
            std::wstring dino_c, species_c;
            unsigned int sid1_c = 0, sid2_c = 0;
            // 允许显式指定恐龙 ID（客户端面板走这条路，不然只能退化用"最近那只"）：
            //   `Trait 买 <玩家> <词条> <级别> id:<DinoID1>:<DinoID2>`
            unsigned int want1 = 0, want2 = 0;
            for (size_t i = 3; i < tt.size(); ++i)
            {
                if (tt[i].compare(0, 3, L"id:") != 0) continue;
                // 面板用 Format Text 拼 int 会自动加千位分隔符（`476,676,959`）
                //   ⇒ 只认数字字符，逗号/空格一律跳过；中间的 `:` 切两半
                {
                    const std::wstring rest = tt[i].substr(3);
                    std::wstring na, nb;
                    bool second = false;
                    for (size_t k = 0; k < rest.size(); ++k)
                    {
                        const wchar_t ch = rest[k];
                        if (ch == L':') { second = true; continue; }
                        if (ch >= L'0' && ch <= L'9') { if (second) nb.push_back(ch); else na.push_back(ch); }
                    }
                    if (!na.empty()) want1 = (unsigned int)_wtoi64(na.c_str());
                    if (!nb.empty()) want2 = (unsigned int)_wtoi64(nb.c_str());
                }
            }
            // `BUY diag` 诊断日志已摘（排查完毕后按服主要求清掉 —— 2026-09-28）
            //   想临时开回来：把下面三行注释放开即可
            //   std::wstring tk; for (size_t i = 0; i < tt.size(); ++i) tk += L"[" + tt[i] + L"]";
            //   Log::GetLog()->info("[ZeroARKTrait] BUY diag: ntok={} tokens={} trait=[{}] tier={} want={}:{}",
            //                       tt.size(), AsaApi::Tools::Utf8Encode(tk), AsaApi::Tools::Utf8Encode(tr), tier, want1, want2);
            AShooterPlayerController* pc_c = FindPlayer(tt[0]);
            // 命令里显式带了 id就绝不许再退化 —— 不然又是"看着成功、货给错龙"
            //   ① `id:0:0`（面板没把龙 ID 带上）② id 非 0 但 FindDinoById 找不到那只龙
            //   ⇒ 两种都直接拒 + 写审计 + 回明确的错，绝不悄悄发给"最近那只"
            if (!idTok.empty())
            {
                APrimalDinoCharacter* dn_id = (want1 != 0) ? FindDinoById(want1, want2) : nullptr;
                if (dn_id != nullptr)
                {
                    dino_c = DinoNameOnly(dn_id);
                    species_c = DinoSpecies(dn_id);
                    sid1_c = dn_id->DinoID1Field();
                    sid2_c = dn_id->DinoID2Field();
                }
                else
                {
                    const std::wstring why_id = (want1 == 0)
                        ? Msg(L"pre_id_missing", idTok)
                        : Msg(L"pre_dino_gone", std::to_wstring(want1), std::to_wstring(want2));
                    Log::GetLog()->info("[ZeroARKTrait] BUY explicit-id REJECT idTok={} want={}:{} player={}",
                                        AsaApi::Tools::Utf8Encode(idTok), want1, want2, AsaApi::Tools::Utf8Encode(tt[0]));
                    AuditLog(L"下单拒绝（显式 id） | 玩家=" + tt[0] + L" | 词条=" + tr + L"[" + std::to_wstring(tier) + L"] | " + why_id);
                    // 面板走的是控制台回执 ⇒ `reply` 玩家屏幕上看不到
                    //   ⇒ 另外把原因塞进下次推商店数据的 `||ERR|` 段，面板会直接把它显示出来
if (pc_c != nullptr)
{
    FString e0 = pc_c->GetEOSId();
    SetPendingError(std::wstring(*e0), why_id);
}
                    // 面板单（带 `id:`）走控制台回执 ⇒ `reply` 玩家屏幕上看不到
                    //   ⇒ 把原因直接发到玩家屏幕上
                    if (pc_c != nullptr) Say(pc_c, Msg(L"buy_fail", why_id));
reply = L"ERR: " + why_id;
                    return;
                }
            }
            else if (pc_c != nullptr)
            {
                const std::wstring me_c = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc_c));
                DinoRef sel_c;
                if (GetSelection(me_c, sel_c) && sel_c.id1 != 0)
                {
                    dino_c = sel_c.name; species_c = sel_c.species; sid1_c = sel_c.id1; sid2_c = sel_c.id2;
                }
                else
                {
                    APrimalDinoCharacter* dn_c = NearestDino(pc_c, g_cfg.near_radius);
                    if (dn_c != nullptr)
                    {
                        dino_c = DinoNameOnly(dn_c);
                        species_c = DinoSpecies(dn_c);
                        sid1_c = dn_c->DinoID1Field();
                        sid2_c = dn_c->DinoID2Field();
                    }
                }
            }
const std::wstring err = EnqueueBuyReq(tt[0], tr, tier, dino_c, species_c, sid1_c, sid2_c);
reply = err.empty()
? (L"OK: queued id=" + std::to_wstring(sid1_c) + L":" + std::to_wstring(sid2_c) +
L" dino=" + dino_c + L" species=" + species_c)
: (L"ERR: " + err);
            // 面板单（带 `id:`）走控制台回执 ⇒ 玩家屏幕上看不到失败原因
            //   （点数不足 / 预检拒绝 / 不在线…）⇒ 直接 Say 给他 （聊天入口不重复发）
            if (!err.empty() && !idTok.empty() && pc_c != nullptr)
                Say(pc_c, (err.find(L'<') != std::wstring::npos) ? err : Msg(L"buy_fail", err));
return;
        }
        else if (a == L"取单" || a == L"qudan" || a == L"reqs" || a == L"Reqs")
        {
            reply = BuyReqListText();
            return;
        }
        else if (a == L"单结" || a == L"danjie" || a == L"ack" || a == L"Ack")
        {
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.empty()) { reply = L"用法: Trait 单结 <请求id> [fail]"; return; }
            const bool ok = !(tt.size() >= 2 && (tt[1] == L"fail" || tt[1] == L"False" || tt[1] == L"false" || tt[1] == L"0"));
            AckBuyReq(tt[0], ok, reply);
            return;
        }
        else if (a == L"单清" || a == L"qingdan" || a == L"clearreqs")
        {
            reply = L"OK: cleared " + std::to_wstring(g_buyreqs.size()) + L" pending requests";
            g_buyreqs.clear();
            return;
        }
        else if (a == L"数据" || a == L"shuju" || a == L"data")
        {
            // v1.10.1：给未来的客户端 mod 用的**结构化数据通道** —— 以 `#ZS|` 前缀发到玩家聊天栏，
            // mod 解析后渲染进自己的面板、并把这些前缀消息在聊天栏隐藏（不刷屏）；机器人是唯一数据源。
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.size() < 2) { reply = L"用法: Trait 数据 <玩家> <文本>（以 #ZS| 前缀发到该玩家聊天栏）"; return; }
            std::wstring body = Trim(tail.substr(tail.find(L' ') + 1));
            std::wstring out;
            for (size_t i = 0; i < body.size(); ++i)
            {
                if (body[i] == L'\\' && i + 1 < body.size() && body[i + 1] == L'n') { out += L'\n'; ++i; }
                else out += body[i];
            }
            AShooterPlayerController* pc_d = FindPlayer(tt[0]);
            if (pc_d == nullptr) { reply = L"找不到玩家：" + tt[0]; return; }
            Say(pc_d, L"#ZS|" + out);
            reply = L"OK: pushed to " + tt[0];
            return;
        }
        else if (a == L"提示" || a == L"tishi" || a == L"say")
        {
            // 机器人让插件把一句话发到某个玩家屏幕上：`Trait 提示 <玩家> <文本>`（\n 会还原成换行）
            std::vector<std::wstring> tt;
            Split(Trim(tail), tt);
            if (tt.size() < 2) { reply = L"用法: Trait 提示 <玩家> <文本>"; return; }
            std::wstring body = Trim(tail.substr(tail.find(L' ') + 1));
            std::wstring out;
            for (size_t i = 0; i < body.size(); ++i)
            {
                if (body[i] == L'\\' && i + 1 < body.size() && body[i + 1] == L'n') { out += L'\n'; ++i; }
                else out += body[i];
            }
            AShooterPlayerController* pc_t = FindPlayer(tt[0]);
            if (pc_t == nullptr) { reply = L"找不到玩家：" + tt[0]; return; }
            Say(pc_t, out);
            reply = L"OK: said to " + tt[0];
            return;
        }
        else
            reply = HelpText();   // 帮助 / help / bangzhu / 其它
    }

    // ------------------------------------------------- 玩家侧 UI（文字菜单，v1.5）
    // 玩家在聊天里输入 trait（或 词条）→ 屏幕上出编号菜单 → 回 `trait 1` 之类即可。
    // 设计原则：**整块菜单只读**（查词条 / 查订单 / 看词条名对照 / 用法），改词条仍只在管理侧。
    // 冷却表（玩家指针 → 上次查询时间）
    std::map<const void*, unsigned long long> g_chat_last;

    std::wstring MenuText()
    {
        return Msg(L"menu");
    }

    std::wstring TraitNameListText()
    {
        // v1.9.6：列**全部官方中文名**。内置表里同一个英文名可能有多条（第一条 = 官方名，后面是旧简称别名），
        // 这里按英文名去重 ⇒ 一次列全 51 条（旧简称不重复出现）。ARK 聊天栏可滚动查看。
        std::wstring s = L"词条中文名总表（中文=英文内部名）：";
        std::vector<std::wstring> seen;
        size_t n = 0;
        for (size_t i = 0; i < _countof(kTraitNames); ++i)
        {
            const std::wstring en = kTraitNames[i].en;
            bool dup = false;
            for (size_t k = 0; k < seen.size(); ++k)
                if (_wcsicmp(seen[k].c_str(), en.c_str()) == 0) { dup = true; break; }
            if (dup) continue;
            seen.push_back(en);
            s += L"\n  " + std::wstring(kTraitNames[i].cn) + L"=" + en;
            ++n;
        }
        s += L"\n（共 " + std::to_wstring(n) + L" 条；输入中文或英文都认，价格看机器人 /词条价）";
        return s;
    }

    // 玩家聊天回调：trait [1|2|3|4|单 <订单号>]
    void ChatQuery(AShooterPlayerController* pc, FString* cmd, int /*a*/, int /*b*/)
    {
        if (pc == nullptr || !g_cfg.chat_query) return;

        std::wstring body;
        if (cmd != nullptr)
        {
            body = StripAnyCmd(std::wstring(**cmd));
            body = Trim(body);
        }

        // v1.11.0：`trait 选 …` 是纯本地操作（只改内存里的"选定"，不碰引擎），**不受查询冷却限制** ——
        //   否则玩家刚发完 `trait 龙`，紧接着 `trait 选 1` 会被 5 秒冷却静默吃掉，看起来像"没反应"
        const bool is_select = (body.size() >= 1 && body[0] == L'选');
        if (!is_select)
        {
            const unsigned long long now = GetTickCount64();
            const std::map<const void*, unsigned long long>::iterator it = g_chat_last.find(pc);
            if (it != g_chat_last.end() && now - it->second < (unsigned long long)g_cfg.chat_cooldown_ms) return;
            g_chat_last[pc] = now;
        }

        if (body.empty()) { Say(pc, MenuText()); return; }

        if (body == L"1" || body == L"查" || body == L"列表")
        {
            const FString pn = AsaApi::GetApiUtils().GetSteamName(pc);
            Job j; j.kind = 3; j.player = std::wstring(*pn);
            if (j.player.empty()) { Say(pc, Msg(L"no_player_name")); return; }
            // v1.11.0：有选定就精确查那一只（按 DinoID 搜索），没选则维持"身边最近那只"
            DinoRef sel;
            if (GetSelection(j.player, sel)) { j.by_id = true; j.id1 = sel.id1; j.id2 = sel.id2; }
            std::lock_guard<std::mutex> lock(g_mu);
            g_jobs.push_back(j);
            return;
        }
        if (body == L"2")
        {
            Say(pc, Msg(L"order_query_tip"));
            return;
        }
        if (body.size() >= 2 && body[0] == L'单')
        {
            Say(pc, QueryAuditByOrder(Trim(body.substr(1))));
            return;
        }
        if (body == L"3") { Say(pc, TraitNameListText()); return; }
        if (body == L"4" || body == L"帮助" || body == L"help") { Say(pc, MenuText()); return; }
        if (body == L"5" || body == L"面板" || body == L"ui" || body == L"UI"
            || body.compare(0, 2, L"5 ") == 0)
        {
            // v1.9.7：面板（客户端对话框）。走任务队列 ⇒ 和其他任务一样在 Tick 里跑，安全。
            // v1.9.9：`trait 5 auto`（或 自动）= 用 bAutoClose 弹窗，免得玩家找不到关掉的办法。
            const bool auto_close = (body.find(L"auto") != std::wstring::npos)
                                    || (body.find(L"自动") != std::wstring::npos);
            const FString pn5 = AsaApi::GetApiUtils().GetSteamName(pc);
            Job j5; j5.kind = 12; j5.player = std::wstring(*pn5); j5.tier = auto_close ? 1 : 0;
            if (j5.player.empty()) { Say(pc, Msg(L"no_player_name")); return; }
            std::lock_guard<std::mutex> lock(g_mu);
            g_jobs.push_back(j5);
            return;
        }
        // v1.11.0 我附近的已驯服龙：`trait 龙`（带 zs/mod/数据 ⇒ 只发 #ZS| 结构化行给客户端 mod）
        if (body.size() >= 1 && body[0] == L'龙')
        {
            const std::wstring me_l = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc));
            if (me_l.empty()) { Say(pc, Msg(L"no_player_name")); return; }
            const bool structured = (body.find(L"zs") != std::wstring::npos)
                                    || (body.find(L"ZS") != std::wstring::npos)
                                    || (body.find(L"mod") != std::wstring::npos)
                                    || (body.find(L"数据") != std::wstring::npos);
            float rad = g_cfg.list_radius;
            const size_t sp = body.find(L' ');
            if (sp != std::wstring::npos)
            {
                const float r2 = (float)_wtof(body.substr(sp + 1).c_str());
                if (r2 >= 500.f) rad = r2;
            }
            SendDinoList(pc, me_l, rad, true, structured);
            return;
        }
        // v1.11.0 选定要操作的龙：`trait 选 <序号>` / `trait 选 id:<DinoID1>[:<DinoID2>]` / `trait 选 0` 取消
        if (body.size() >= 1 && body[0] == L'选')
        {
            const std::wstring me_s = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc));
            if (me_s.empty()) { Say(pc, Msg(L"no_player_name")); return; }
            const std::wstring arg = Trim(body.substr(1));
            if (arg.empty() || arg == L"?" || arg == L"帮助")
            {
                Say(pc, Msg(L"sel_usage"));
                return;
            }
            if (arg == L"0" || arg == L"取消" || arg == L"清除" || arg == L"clear" || arg == L"Clear")
            {
                { std::lock_guard<std::mutex> lock(g_mu); g_sel.erase(me_s); }
                Say(pc, Msg(L"sel_cancel"));
                return;
            }
            if (arg.size() > 3 && _wcsnicmp(arg.c_str(), L"id:", 3) == 0)
            {
                const std::wstring rest2 = arg.substr(3);
                const size_t colon2 = rest2.find(L':');
                DinoRef r;
                r.id1 = (unsigned int)_wtoi64(rest2.substr(0, colon2).c_str());
                r.id2 = (colon2 == std::wstring::npos)
                            ? 0u : (unsigned int)_wtoi64(rest2.substr(colon2 + 1).c_str());
                r.name = L"id:" + std::to_wstring(r.id1) + L":" + std::to_wstring(r.id2);
                { std::lock_guard<std::mutex> lock(g_mu); g_sel[me_s] = r; }
                Say(pc, Msg(L"sel_ok", r.name));
                return;
            }
            const int idx = _wtoi(arg.c_str());
            DinoRef pick;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                const std::map<std::wstring, std::vector<DinoRef>>::iterator it = g_last_list.find(me_s);
                if (it != g_last_list.end() && idx >= 1 && (size_t)idx <= it->second.size())
                {
                    pick = it->second[(size_t)idx - 1];
                    found = true;
                }
                if (found) g_sel[me_s] = pick;
            }
            if (!found)
            {
                Say(pc, Msg(L"sel_bad_index"));
                return;
            }
            Say(pc, Msg(L"sel_ok_detail", pick.name, pick.species,
                        std::to_wstring(pick.level),
                        std::to_wstring(pick.id1) + L":" + std::to_wstring(pick.id2)));
            return;
        }
        // v1.10.0 游戏内下单：`trait 买 <词条> [层级0/1/2]` → 入队，等机器人拉取处理（扣点+发货）
        // 修 bug：原来写 `body.compare(0, 3, L"买 ")` —— 拿 3 个 wchar 去比 2 个 wchar 的
        //   字面量（"买"+空格），长度不等 ⇒ 永远不相等 ⇒ 玩家发 trait 买 被当成没匹配、回了菜单。
        //   现在改成"首字符是 买 就算"（后面有没有空格都行）。
        if (body.size() >= 1 && body[0] == L'买')
        {
            if (!g_cfg.buy_request)
            {
                Say(pc, Msg(L"buy_disabled"));
                return;
            }
            std::vector<std::wstring> tt;
            Split(Trim(body.substr(1)), tt);
            if (tt.empty())
            {
                Say(pc, Msg(L"buy_usage"));
                return;
            }
            std::wstring tr;
            int tier = 0;
            ParseTraitToken(tt[0], tr, tier);
            //  对外"级别 1/2/3" ⇒ 内部 tier 0/1/2（`名字[层级]` 那种写法仍是内部 0/1/2，协议不动）
            if (tt.size() >= 2)
            {
                const int lv = _wtoi(tt[1].c_str());
                if (lv < 1 || lv > 3)
                {
                    Say(pc, Msg(L"buy_bad_tier"));
                    return;
                }
                tier = lv - 1;
            }
            tr = NormalizeTraitName(tr);
            const std::wstring me = std::wstring(*AsaApi::GetApiUtils().GetSteamName(pc));

            // v1.11.0：优先用玩家**选定**的那只龙（trait 龙 → trait 选 N）；没选才退回“身边最近那只”。
            // 同时取**物种名**（DinoNameTag）与 DinoID：机器人用物种名算真实身价、用 id 精确发货
            //（只给自设名字的话身价表匹配不到 ⇒ 会按默认系数少收钱）。
            std::wstring dino, species;
            unsigned int sid1 = 0, sid2 = 0;
            bool use_sel = false;
            DinoRef sel;
            if (GetSelection(me, sel) && sel.id1 != 0)
            {
                dino = sel.name; species = sel.species; sid1 = sel.id1; sid2 = sel.id2;
                use_sel = true;
            }
            if (!use_sel)
            {
                APrimalDinoCharacter* d_nearest = NearestDino(pc, g_cfg.near_radius);
                if (d_nearest != nullptr)
                {
                    dino = DinoNameOnly(d_nearest);
                    species = DinoSpecies(d_nearest);
                    sid1 = d_nearest->DinoID1Field();
                    sid2 = d_nearest->DinoID2Field();
                }
            }
            const std::wstring err = EnqueueBuyReq(me, tr, tier, dino, species, sid1, sid2);
            if (!err.empty()) { Say(pc, Msg(L"buy_fail", err)); return; }
            Say(pc, Msg(L"buy_ok", DisplayTraitName(tr), std::to_wstring(tier + 1),
                        dino.empty() ? std::wstring(L"")
                                     : (use_sel ? (L"（按你选定的「" + dino + L" / " + species + L"」算价）")
                                                : (L"（按你身边最近的「" + dino + L" / " + species + L"」算价）"))));
            return;
        }
        Say(pc, MenuText());
    }

    // 查单可以直接在回调里做完（只读文件、不碰引擎 ⇒ 任何线程都安全），
    // 这样 RCON 回执里就能带回审计内容，机器人不用去读文件。
    bool TryHandleQuery(const std::wstring& body, std::wstring& out)
    {
        std::vector<std::wstring> t;
        Split(Trim(body), t);
        if (t.empty()) return false;
        const std::wstring& a = t[0];
        if (a == L"菜单" || a == L"caidan" || a == L"menu" || a == L"Menu") { out = MenuText(); return true; }
        if (a == L"词条名" || a == L"名单" || a == L"names") { out = TraitNameListText(); return true; }
        if (!(a == L"查单" || a == L"chadan" || a == L"query" || a == L"Query")) return false;
        out = QueryAuditByOrder(t.size() >= 2 ? t[1] : L"");
        return true;
    }

    // --------------------------------------------------------- console callbacks
    // 权限闸门：本插件**不向玩家开放**（删词条尤其不开放）。
    //   pc == nullptr  => RCON 或服务端控制台（管理侧）⇒ 放行
    //   pc != nullptr  => 有玩家在敲：只有拿到 CheatManager（= 管理员/已开作弊）才放行
    bool IsAdminCaller(APlayerController* pc)
    {
        if (pc == nullptr) return true;
        if (pc->CheatManagerField() != nullptr) return true;
        Log::GetLog()->warn("[ZeroARKTrait] refused a non-admin console command (pc={})",
                            static_cast<const void*>(pc));
        return false;
    }

    void CmdDispatch(APlayerController* pc, FString* cmd, bool /*log*/)
    {
        if (cmd == nullptr || !IsAdminCaller(pc)) return;
        const std::wstring body = StripAnyCmd(std::wstring(**cmd));
        std::wstring r;
        if (TryHandleQuery(body, r)) { Say(nullptr, r); return; }   // 菜单/名单/查单：直接回
        EnqueueDispatch(body, r);
        Say(nullptr, r);
    }
    void CmdAdd(APlayerController* pc, FString* cmd, bool /*log*/)
    {
        if (cmd == nullptr || !IsAdminCaller(pc)) return;
        std::wstring r;
        Enqueue(1, StripAnyCmd(std::wstring(**cmd)), r);
    }
    void CmdDel(APlayerController* pc, FString* cmd, bool /*log*/)
    {
        if (cmd == nullptr || !IsAdminCaller(pc)) return;
        std::wstring r;
        Enqueue(2, StripAnyCmd(std::wstring(**cmd)), r);
    }
    void CmdList(APlayerController* pc, FString* cmd, bool /*log*/)
    {
        if (cmd == nullptr || !IsAdminCaller(pc)) return;
        std::wstring r;
        Enqueue(3, StripAnyCmd(std::wstring(**cmd)), r);
    }
    void CmdHelp(APlayerController* pc, FString* /*cmd*/, bool /*log*/)
    {
        if (!IsAdminCaller(pc)) return;
        Say(nullptr, HelpText());
    }

    // ------------------------------------------------------------ RCON callbacks
    void RconReply(RCONClientConnection* conn, RCONPacket* packet, const std::wstring& text)
    {
        FString out = text.c_str();
        conn->SendMessageW(packet->Id, 0, &out);
    }
    void RconAdd(RCONClientConnection* conn, RCONPacket* packet, UWorld* /*w*/)
    {
        std::wstring r;
        Enqueue(1, StripAnyCmd(std::wstring(*packet->Body)), r);
        RconReply(conn, packet, r);
    }
    void RconDel(RCONClientConnection* conn, RCONPacket* packet, UWorld* /*w*/)
    {
        std::wstring r;
        Enqueue(2, StripAnyCmd(std::wstring(*packet->Body)), r);
        RconReply(conn, packet, r);
    }
    void RconList(RCONClientConnection* conn, RCONPacket* packet, UWorld* /*w*/)
    {
        std::wstring r;
        Enqueue(3, StripAnyCmd(std::wstring(*packet->Body)), r);
        RconReply(conn, packet, r);
    }
    void RconHelp(RCONClientConnection* conn, RCONPacket* packet, UWorld* /*w*/)
    {
        RconReply(conn, packet, HelpText());
    }
    void RconDispatch(RCONClientConnection* conn, RCONPacket* packet, UWorld* /*w*/)
    {
        const std::wstring body = StripAnyCmd(TryFixUtf8Mojibake(std::wstring(*packet->Body)));
        Log::GetLog()->info("[ZeroARKTrait] RCON body = [{}]", AsaApi::Tools::Utf8Encode(body));

        std::wstring r;
        if (TryHandleQuery(body, r)) { RconReply(conn, packet, r); return; }   // 查单：直接回内容
        EnqueueDispatch(body, r);
        RconReply(conn, packet, r);
    }
} // namespace

extern "C" __declspec(dllexport) void Plugin_Init()
{
    Log::Get().Init("ZeroARKTrait");
    LoadConfig();
    LoadTraitNames();
    LoadShopConfig();       // 价目表 / 恐龙价值表 / 商店设置（全部外置，零第三方依赖）

    API::Timer::Get().RecurringExecute(FString(L"zeroark_trait_tick"), &Tick, 1, -1, false);

    // 命令名一律用 ASCII（ArkApi 的 RCON 派发匹配不了非 ASCII 命令名）；
    // 中文动作词放在参数里：Trait 列表 / Trait 加 / Trait 删 / Trait 帮助
    AsaApi::GetCommands().AddConsoleCommand(FString(L"Trait"), &CmdDispatch);
    AsaApi::GetCommands().AddRconCommand(FString(L"Trait"), &RconDispatch);

    const std::wstring add_names[]  = { CMD_ADD };
    const std::wstring del_names[]  = { CMD_DEL };
    const std::wstring list_names[] = { CMD_LIST };
    const std::wstring help_names[] = { CMD_HELP };

    auto reg = [](const std::wstring* names, size_t n,
                  void (*con)(APlayerController*, FString*, bool),
                  void (*rcon)(RCONClientConnection*, RCONPacket*, UWorld*))
    {
        for (size_t i = 0; i < n; ++i)
        {
            AsaApi::GetCommands().AddConsoleCommand(FString(names[i].c_str()), con);
            AsaApi::GetCommands().AddRconCommand(FString(names[i].c_str()), rcon);
        }
    };

    reg(add_names,  _countof(add_names),  &CmdAdd,  &RconAdd);
    reg(del_names,  _countof(del_names),  &CmdDel,  &RconDel);
    reg(list_names, _countof(list_names), &CmdList, &RconList);
    reg(help_names, _countof(help_names), &CmdHelp, &RconHelp);

    // 玩家侧只读查询（聊天指令；可在 zerotrait.ini 关闭）
    AsaApi::GetCommands().AddChatCommand(FString(L"trait"), &ChatQuery);
    AsaApi::GetCommands().AddChatCommand(FString(L"词条"), &ChatQuery);

    Log::GetLog()->info("[ZeroARKTrait] plugin initialized (api version {})", AsaApi::Tools::GetApiVersion());
}

extern "C" __declspec(dllexport) void Plugin_Unload()
{
    API::Timer::Get().UnloadTimer(FString(L"zeroark_trait_tick"));

    const std::wstring all_names[] = {
        L"Trait",
        CMD_ADD, CMD_DEL, CMD_LIST, CMD_HELP
    };
    for (size_t i = 0; i < _countof(all_names); ++i)
    {
        AsaApi::GetCommands().RemoveConsoleCommand(FString(all_names[i].c_str()));
        AsaApi::GetCommands().RemoveRconCommand(FString(all_names[i].c_str()));
    }
    AsaApi::GetCommands().RemoveChatCommand(FString(L"trait"));
    AsaApi::GetCommands().RemoveChatCommand(FString(L"词条"));

    Log::GetLog()->info("[ZeroARKTrait] plugin unloaded");
}
