// ============================================================================
// MiniMysql.h —— 极简 MySQL 客户端（只读一个整数用）
//
// 为什么自己写（服主 2026-09-22 的架构约束）：
//   「插件除了 asaapi 的那个官方权限插件和 modui 以外，不要有其他依赖项，
//     能配置的都放到配置文件」
//   ⇒ 不能带 libmysql.dll / 不能依赖 ArkShop 插件
//   ⇒ 但【点数存在 MySQL 里】，插件要自己读
//
// 权威先例：ArkShop.dll 的依赖表里【只有 WS2_32.dll】，没有任何 mysql 客户端 dll
//   ⇒ 证明"用 Windows 套接字自己实现 MySQL 协议"这条路是通的
//
// 本文件只依赖：WS2_32（套接字）+ bcrypt（SHA1/SHA256/HMAC，都是 Windows 系统库）
//   ⇒ 不算第三方依赖
//
// 能力范围（够用就好，不做通用客户端）：
//   - 读 server greeting、握手响应
//   - 认证：mysql_native_password（完整）/ caching_sha2_password（快路径）
//   - 一条 COM_QUERY，读结果集第一行第一列
//   - 全程带超时，失败就返回 false（调用方降级）
// ============================================================================
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <string>
#include <vector>
#include <cstdint>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace minimysql
{
    // ────────────────────────────────── 小工具

    inline uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
    inline uint32_t Rd24(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16)); }
    inline uint32_t Rd32(const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); }

    // Windows 系统库算哈希（避免自己写 SHA，少一堆出错点）
    inline bool Hash(BCRYPT_ALG_HANDLE alg, const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
    {
        BCRYPT_HASH_HANDLE h = nullptr;
        if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) < 0) return false;
        if (BCryptHashData(h, (PUCHAR)in.data(), (ULONG)in.size(), 0) < 0) { BCryptDestroyHash(h); return false; }
        DWORD objSize = 0, cb = 0;
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objSize, sizeof(objSize), &cb, 0);
        // 直接按算法输出长度取
        DWORD hashLen = 0, cb2 = 0;
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashLen, sizeof(hashLen), &cb2, 0);
        out.assign(hashLen, 0);
        bool ok = (BCryptFinishHash(h, out.data(), hashLen, 0) >= 0);
        BCryptDestroyHash(h);
        return ok;
    }

    inline bool Sha1(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
    {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) return false;
        bool ok = Hash(a, in, out);
        BCryptCloseAlgorithmProvider(a, 0);
        return ok;
    }

    inline bool Sha256(const std::vector<uint8_t>& in, std::vector<uint8_t>& out)
    {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
        bool ok = Hash(a, in, out);
        BCryptCloseAlgorithmProvider(a, 0);
        return ok;
    }

    // ────────────────────────────────── 连接对象

    class Conn
    {
    public:
        ~Conn() { Close(); }

        // 连上去并完成认证。失败返回 false（调用方降级到 push 模式）
        bool Open(const std::string& host, int port,
                  const std::string& user, const std::string& pass,
                  const std::string& db, std::string& err)
        {
            WSADATA wsa;
            if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup failed"; return false; }

            addrinfo hints = {};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            addrinfo* res = nullptr;
            char portStr[16];
            sprintf_s(portStr, "%d", port);
            if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || res == nullptr)
            {
                err = "resolve failed: " + host;
                return false;
            }
            sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            if (sock_ == INVALID_SOCKET) { freeaddrinfo(res); err = "socket failed"; return false; }

            // 连接 + 收发超时（必须设，否则服务器卡住会拖死游戏线程）
            DWORD tv = 5000;
            setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

            if (connect(sock_, res->ai_addr, (int)res->ai_addrlen) != 0)
            {
                freeaddrinfo(res);
                err = "connect failed";
                Close();
                return false;
            }
            freeaddrinfo(res);

            if (!Handshake(user, pass, db, err)) { Close(); return false; }
            return true;
        }

        void Close()
        {
            if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
        }

        // 执行一条【不返回结果集】的语句（CREATE / INSERT / UPDATE）。
        // 只关心 OK 还是 ERR —— 用于"自建数据库"那三件事
        bool Exec(const std::string& sql, std::string& err)
        {
            if (sock_ == INVALID_SOCKET) { err = "not connected"; return false; }
            seq_ = 0;                                     // 新命令序号从 0 重开
            std::vector<uint8_t> q;
            q.push_back(0x03);                            // COM_QUERY
            q.insert(q.end(), sql.begin(), sql.end());
            if (!WritePacket(q, err)) return false;

            std::vector<uint8_t> pkt;
            if (!ReadPacket(pkt, err)) return false;
            if (pkt.empty()) { err = "empty response"; return false; }
            if (pkt[0] == 0xFF) { err = "server error: " + ErrText(pkt); return false; }
            if (pkt[0] == 0x00 || pkt[0] == 0xFE) return true;   // OK
            err = "unexpected response for non-select";
            return false;
        }

        // 执行一条查询，返回第一行第一列的字符串（无行返回 false）
        bool QueryOne(const std::string& sql, std::string& out, std::string& err)
        {
            if (sock_ == INVALID_SOCKET) { err = "not connected"; return false; }
            // MySQL 协议：每条【新命令】的包序号都必须从 0 重新开始
            //   （否则服务器回 "Got packets out of order" —— 2026-09-22 实测踩到）
            seq_ = 0;
            std::vector<uint8_t> q;
            q.push_back(0x03);                       // COM_QUERY
            q.insert(q.end(), sql.begin(), sql.end());
            if (!WritePacket(q, err)) return false;

            std::vector<uint8_t> pkt;
            if (!ReadPacket(pkt, err)) return false;
            if (pkt.empty()) { err = "empty response"; return false; }

            if (pkt[0] == 0xFF)                      // ERR
            {
                err = "server error: " + ErrText(pkt);
                return false;
            }
            if (pkt[0] == 0x00) { err = "OK packet (no rows)"; return false; }

            // 结果集：先读列数（length-encoded int）
            size_t pos = 0;
            uint64_t ncols = 0;
            if (!ReadLenEnc(pkt, pos, ncols)) { err = "bad column count"; return false; }

            // 跳过列定义（ncols 个包）
            for (uint64_t i = 0; i < ncols; ++i)
            {
                std::vector<uint8_t> tmp;
                if (!ReadPacket(tmp, err)) return false;
            }
            // 读过 EOF（CLIENT_DEPRECATE_EOF 未开，所以有 EOF 包）
            {
                std::vector<uint8_t> tmp;
                if (!ReadPacket(tmp, err)) return false;
            }
            // 读第一行
            std::vector<uint8_t> row;
            if (!ReadPacket(row, err)) return false;
            if (!row.empty() && row[0] == 0xFE) { err = "no rows"; return false; }   // EOF = 空结果
            if (!row.empty() && row[0] == 0xFF) { err = "server error: " + ErrText(row); return false; }

            size_t rp = 0;
            uint64_t len = 0;
            if (!ReadLenEnc(row, rp, len)) { err = "bad row"; return false; }
            if (len == 0xFFFFFFFFFFFFFFFFULL) { out.clear(); return true; }          // NULL
            if (rp + len > row.size()) { err = "row overflow"; return false; }
            out.assign((const char*)row.data() + rp, (size_t)len);
            return true;
        }

    private:
        SOCKET sock_ = INVALID_SOCKET;
        uint8_t seq_ = 0;

        // ── 包读写（MySQL 包 = 3 字节小端长度 + 1 字节序号 + 负载）
        bool RecvAll(uint8_t* buf, size_t n)
        {
            size_t got = 0;
            while (got < n)
            {
                int r = recv(sock_, (char*)buf + got, (int)(n - got), 0);
                if (r <= 0) return false;
                got += (size_t)r;
            }
            return true;
        }

        bool ReadPacket(std::vector<uint8_t>& out, std::string& err)
        {
            uint8_t hdr[4];
            if (!RecvAll(hdr, 4)) { err = "recv header failed/timeout"; return false; }
            uint32_t len = Rd24(hdr);
            seq_ = (uint8_t)(hdr[3] + 1);
            out.assign(len, 0);
            if (len > 0 && !RecvAll(out.data(), len)) { err = "recv payload failed"; return false; }
            return true;
        }

        bool WritePacket(const std::vector<uint8_t>& payload, std::string& err)
        {
            uint8_t hdr[4];
            uint32_t len = (uint32_t)payload.size();
            hdr[0] = (uint8_t)(len & 0xFF);
            hdr[1] = (uint8_t)((len >> 8) & 0xFF);
            hdr[2] = (uint8_t)((len >> 16) & 0xFF);
            hdr[3] = seq_;
            std::vector<uint8_t> buf;
            buf.reserve(4 + payload.size());
            buf.insert(buf.end(), hdr, hdr + 4);
            buf.insert(buf.end(), payload.begin(), payload.end());
            size_t sent = 0;
            while (sent < buf.size())
            {
                int r = send(sock_, (const char*)buf.data() + sent, (int)(buf.size() - sent), 0);
                if (r <= 0) { err = "send failed"; return false; }
                sent += (size_t)r;
            }
            seq_++;
            return true;
        }

        static std::string ErrText(const std::vector<uint8_t>& pkt)
        {
            if (pkt.size() < 3) return "(short err)";
            // ERR: 0xFF + errno(2) + '#' + sqlstate(5) + message
            size_t p = 3;
            if (pkt.size() > 3 && pkt[3] == '#') p += 6;
            if (p >= pkt.size()) return "(no message)";
            return std::string((const char*)pkt.data() + p, pkt.size() - p);
        }

        // length-encoded integer
        static bool ReadLenEnc(const std::vector<uint8_t>& p, size_t& pos, uint64_t& v)
        {
            if (pos >= p.size()) return false;
            uint8_t b = p[pos++];
            if (b < 0xFB) { v = b; return true; }
            if (b == 0xFB) { v = 0xFFFFFFFFFFFFFFFFULL; return true; }   // NULL
            if (b == 0xFC) { if (pos + 2 > p.size()) return false; v = Rd16(&p[pos]); pos += 2; return true; }
            if (b == 0xFD) { if (pos + 3 > p.size()) return false; v = Rd24(&p[pos]); pos += 3; return true; }
            if (b == 0xFE) { if (pos + 8 > p.size()) return false; v = 0; for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[pos + i]) << (8 * i); pos += 8; return true; }
            return false;
        }

        static void WriteLenEnc(std::vector<uint8_t>& out, uint64_t v)
        {
            if (v < 251) { out.push_back((uint8_t)v); }
            else if (v < 65536) { out.push_back(0xFC); out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)(v >> 8)); }
            else { out.push_back(0xFD); out.push_back((uint8_t)(v & 0xFF)); out.push_back((uint8_t)((v >> 8) & 0xFF)); out.push_back((uint8_t)((v >> 16) & 0xFF)); }
        }

        // ── 认证
        bool Handshake(const std::string& user, const std::string& pass,
                       const std::string& db, std::string& err)
        {
            std::vector<uint8_t> greet;
            if (!ReadPacket(greet, err)) return false;
            if (greet.empty() || greet[0] == 0xFF) { err = "server refused: " + ErrText(greet); return false; }
            if (greet.size() < 34) { err = "short greeting"; return false; }

            size_t p = 1;
            while (p < greet.size() && greet[p] != 0) ++p;      // server version
            ++p;
            p += 4;                                             // thread id
            std::vector<uint8_t> scramble(greet.begin() + p, greet.begin() + p + 8);
            p += 8 + 1;                                         // +filler
            uint16_t capLow = Rd16(&greet[p]); p += 2;           // capability (low)
            p += 1 + 2;                                          // charset + status
            uint16_t capHigh = Rd16(&greet[p]); p += 2;
            uint32_t caps = (uint32_t)capLow | ((uint32_t)capHigh << 16);
            uint8_t authLen = greet[p]; p += 1;
            p += 10;                                             // reserved
            // auth-plugin-data part 2（长度 = max(13, authLen-8)，末尾可能有 NUL）
            size_t part2 = (size_t)((authLen > 8) ? (authLen - 8) : 13);
            if (p + part2 > greet.size()) part2 = greet.size() - p;
            for (size_t i = 0; i < part2 && i < 12; ++i)
                if (greet[p + i] != 0) scramble.push_back(greet[p + i]);
            p += part2;
            std::string plugin;
            if (p < greet.size()) plugin.assign((const char*)greet.data() + p);
            while (!plugin.empty() && plugin.back() == 0) plugin.pop_back();

            const uint32_t CLIENT_LONG_PASSWORD     = 0x00000001;
            const uint32_t CLIENT_CONNECT_WITH_DB   = 0x00000008;
            const uint32_t CLIENT_PROTOCOL_41       = 0x00000200;
            const uint32_t CLIENT_TRANSACTIONS      = 0x00002000;
            const uint32_t CLIENT_SECURE_CONNECTION = 0x00008000;
            const uint32_t CLIENT_PLUGIN_AUTH       = 0x00080000;

            uint32_t want = (CLIENT_LONG_PASSWORD | CLIENT_PROTOCOL_41 |
                             CLIENT_SECURE_CONNECTION | CLIENT_PLUGIN_AUTH |
                             CLIENT_TRANSACTIONS) & caps;
            if (!db.empty()) want |= (CLIENT_CONNECT_WITH_DB & caps);

            // 认证串
            std::vector<uint8_t> authResp;
            std::string usePlugin = plugin.empty() ? "mysql_native_password" : plugin;
            if (!pass.empty())
            {
                if (usePlugin == "caching_sha2_password")
                {
                    // XOR(SHA256(pass), SHA256(SHA256(SHA256(pass)) + scramble))
                    std::vector<uint8_t> p1(pass.begin(), pass.end());
                    std::vector<uint8_t> h1, h2, h3;
                    if (!Sha256(p1, h1)) { err = "sha256 failed"; return false; }
                    if (!Sha256(h1, h2)) { err = "sha256 failed"; return false; }
                    std::vector<uint8_t> mix = h2;
                    mix.insert(mix.end(), scramble.begin(), scramble.end());
                    if (!Sha256(mix, h3)) { err = "sha256 failed"; return false; }
                    authResp.resize(h1.size());
                    for (size_t i = 0; i < h1.size(); ++i) authResp[i] = h1[i] ^ h3[i];
                }
                else
                {
                    // mysql_native_password：SHA1(pass) XOR SHA1(scramble + SHA1(SHA1(pass)))
                    usePlugin = "mysql_native_password";
                    std::vector<uint8_t> p1(pass.begin(), pass.end());
                    std::vector<uint8_t> h1, h2, h3;
                    if (!Sha1(p1, h1)) { err = "sha1 failed"; return false; }
                    if (!Sha1(h1, h2)) { err = "sha1 failed"; return false; }
                    std::vector<uint8_t> mix = scramble;
                    mix.insert(mix.end(), h2.begin(), h2.end());
                    if (!Sha1(mix, h3)) { err = "sha1 failed"; return false; }
                    authResp.resize(h1.size());
                    for (size_t i = 0; i < h1.size(); ++i) authResp[i] = h1[i] ^ h3[i];
                }
            }

            // 组装 HandshakeResponse41
            std::vector<uint8_t> r;
            auto push32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) r.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); };
            auto push16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) r.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); };
            push32(want);
            push32(16777215);                       // max packet
            r.push_back(0x21);                      // charset = utf8_general_ci
            for (int i = 0; i < 23; ++i) r.push_back(0);
            r.insert(r.end(), user.begin(), user.end());
            r.push_back(0);
            WriteLenEnc(r, authResp.size());
            r.insert(r.end(), authResp.begin(), authResp.end());
            if (want & CLIENT_CONNECT_WITH_DB)
            {
                r.insert(r.end(), db.begin(), db.end());
                r.push_back(0);
            }
            r.insert(r.end(), usePlugin.begin(), usePlugin.end());
            r.push_back(0);

            if (!WritePacket(r, err)) return false;

            std::vector<uint8_t> auth;
            if (!ReadPacket(auth, err)) return false;
            if (auth.empty()) { err = "empty auth reply"; return false; }
            if (auth[0] == 0x00) return true;                        // OK
            if (auth[0] == 0xFF) { err = "auth failed: " + ErrText(auth); return false; }
            if (auth[0] == 0xFE)
            {
                // AuthSwitchRequest：换插件重来（带上新的 scramble）
                if (auth.size() < 2) { err = "bad auth switch"; return false; }
                size_t q = 1;
                std::string newPlugin;
                while (q < auth.size() && auth[q] != 0) { newPlugin.push_back((char)auth[q]); ++q; }
                ++q;
                std::vector<uint8_t> newScr(auth.begin() + q, auth.end());
                while (!newScr.empty() && newScr.back() == 0) newScr.pop_back();

                std::vector<uint8_t> resp;
                if (!pass.empty())
                {
                    std::vector<uint8_t> p1(pass.begin(), pass.end());
                    if (newPlugin == "caching_sha2_password")
                    {
                        std::vector<uint8_t> h1, h2, h3;
                        Sha256(p1, h1); Sha256(h1, h2);
                        std::vector<uint8_t> mix = h2; mix.insert(mix.end(), newScr.begin(), newScr.end());
                        Sha256(mix, h3);
                        resp.resize(h1.size());
                        for (size_t i = 0; i < h1.size(); ++i) resp[i] = h1[i] ^ h3[i];
                    }
                    else
                    {
                        std::vector<uint8_t> h1, h2, h3;
                        Sha1(p1, h1); Sha1(h1, h2);
                        std::vector<uint8_t> mix = newScr; mix.insert(mix.end(), h2.begin(), h2.end());
                        Sha1(mix, h3);
                        resp.resize(h1.size());
                        for (size_t i = 0; i < h1.size(); ++i) resp[i] = h1[i] ^ h3[i];
                    }
                }
                if (!WritePacket(resp, err)) return false;
                std::vector<uint8_t> a2;
                if (!ReadPacket(a2, err)) return false;
                if (!a2.empty() && a2[0] == 0x00) return true;
                if (!a2.empty() && a2[0] == 0xFF) { err = "auth failed: " + ErrText(a2); return false; }
                err = "auth incomplete (plugin=" + newPlugin + ")";
                return false;
            }
            if (auth[0] == 0x01)
            {
                // AuthMoreData：caching_sha2 的 0x03=快认证成功 / 0x04=需要完整认证（要 RSA 或 TLS）
                if (auth.size() >= 2 && auth[1] == 0x03)
                {
                    std::vector<uint8_t> ok;
                    if (!ReadPacket(ok, err)) return false;
                    if (!ok.empty() && ok[0] == 0x00) return true;
                    err = "auth final step failed";
                    return false;
                }
                err = "caching_sha2 full auth required (需 RSA/TLS) ⇒ 建议把该库账号改成 mysql_native_password";
                return false;
            }
            err = "unexpected auth packet";
            return false;
        }
    };
}
