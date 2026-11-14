// FTP サーバー (CTRPF 下画面メニューの「FTP Server」ボタンから起動)
//  - CTRPF のメニューを開いている間だけ動き、B で停止する
//  - SD カード全体を公開する。ユーザー名・パスワードは何でも通す (同じ Wi-Fi の端末から接続する想定)
//  - 制御ポート 5000、パッシブ (PASV / EPSV) とアクティブ (PORT) の両方に対応
//  - CTRPF 側の改造は ctrpf-patch/patch-home.sh (Mapper ボタン → FTP Server)

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "CTRPluginFrameworkImpl/Graphics/Renderer.hpp"
#include "CTRPluginFrameworkImpl/Graphics/Window.hpp"
#include "CTRPluginFrameworkImpl/Preferences.hpp"
#include "CTRPluginFrameworkImpl/System/FSPath.hpp"
#include "csvc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace CTRPluginFramework
{
    namespace
    {
        const u16  kCtrlPort      = 5000;
        const u16  kPasvPortFirst = 5100;
        const u16  kPasvPortCount = 100;
        const u32  kSocBufSize    = 0x40000;
        const u32  kXferBufSize   = 0x8000;
        const int  kTimeoutMs     = 15000;   // データ接続の待ち時間
        const int  kLogLines      = 9;

        // socInit に渡すメモリはプロセスのヒープ領域 (0x08000000～0x10000000) にないと
        // svcCreateMemoryBlock が E0A01BF5 (アドレス不正) になる。プラグインのメモリは 0x07000000 台なので、
        // ヒープ領域の空いている所に svcControlMemoryUnsafe で確保する。
        const u32   kSocBufAddrs[] = { 0x0F000000, 0x0E800000, 0x0E000000 };
        u32        *g_socBuf   = nullptr;
        u8         *g_xferBuf  = nullptr;
        FS_Archive  g_sdmc     = 0;
        u32         g_ip       = 0;          // ネットワークバイトオーダー

        int         g_listen   = -1;         // 制御ポートの待ち受け
        int         g_ctrl     = -1;         // 接続中のクライアント
        int         g_pasv     = -1;         // パッシブの待ち受け
        u16         g_pasvPort = kPasvPortFirst;
        bool        g_portSet  = false;      // PORT で受け取った接続先
        sockaddr_in g_portAddr;

        std::string g_rxBuf;
        std::string g_cwd = "/";
        std::string g_renameFrom;
        u64         g_restOffset = 0;
        std::string g_clientIp;

        std::vector<std::string> g_log;
        std::string g_status;

        // ---------------------------------------------------------------
        //  画面
        // ---------------------------------------------------------------
        void Log(const std::string &s)
        {
            g_log.push_back(s);
            if ((int)g_log.size() > kLogLines)
                g_log.erase(g_log.begin());
        }

        std::string IpToString(u32 ip)
        {
            char buf[20];
            const u8 *b = (const u8 *)&ip;
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            return buf;
        }

        void Draw(void)
        {
            const Color &txt = FwkSettings::Get().MainTextColor;
            const Color  sub(0xA0, 0xA0, 0xA0);
            const Color  ok(0x01, 0xE3, 0x0E);

            Renderer::SetTarget(TOP);
            Window::TopWindow.Draw("FTP Server");
            int y = 45;
            const std::string addr = g_ip ? "ftp://" + IpToString(g_ip) + ":" + std::to_string(kCtrlPort) : "-";
            Renderer::DrawSysString(addr.c_str(), 40, y, 360, ok);
            y += 2;
            Renderer::DrawSysString("ユーザー名・パスワード：何でも可", 40, y, 360, txt);
            y += 2;
            Renderer::DrawSysString(g_status.c_str(), 40, y, 360, txt);
            y += 6;
            for (const std::string &l : g_log)
            {
                if (y > 200) break;
                Renderer::DrawSysString(l.c_str(), 40, y, 360, sub);
            }

            Renderer::SetTarget(BOTTOM);
            Window::BottomWindow.Draw();
            y = 40;
            Renderer::DrawSysString("FTP サーバー動作中", 40, y, 280, txt);
            y += 10;
            Renderer::DrawSysString("PC やスマホの FTP クライアントから", 40, y, 280, txt);
            Renderer::DrawSysString("上画面のアドレスに接続してください", 40, y, 280, txt);
            Renderer::DrawSysString("SD カード全体を読み書きできます", 40, y, 280, txt);
            y += 6;
            // CTRPF が読んでいる AR コードのファイル (プラグインのフォルダの cheats.txt が優先)
            {
                std::string f = Preferences::CheatsFile;
                if (!f.empty() && f[0] != '/') f = FSPath::CurrentWorkingDirectory + f;
                Renderer::DrawSysString("AR コードのファイル:", 40, y, 290, txt);
                Renderer::DrawSysString(f.c_str(), 40, y, 300, ok);
            }
            y += 4;
            Renderer::DrawSysString(" で停止", 40, y, 280, ok);
            Renderer::EndFrame();
        }

        // ---------------------------------------------------------------
        //  ソケット
        // ---------------------------------------------------------------
        void CloseSock(int &fd)
        {
            if (fd >= 0) close(fd);
            fd = -1;
        }

        void SetNonBlock(int fd)
        {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        }

        bool WaitFd(int fd, short events, int ms)
        {
            pollfd p;
            p.fd = fd; p.events = events; p.revents = 0;
            return poll(&p, 1, ms) > 0 && (p.revents & (events | POLLERR | POLLHUP));
        }

        bool SendAll(int fd, const void *data, size_t len)
        {
            const u8 *p = (const u8 *)data;
            while (len > 0)
            {
                if (!WaitFd(fd, POLLOUT, kTimeoutMs)) return false;
                ssize_t n = send(fd, p, len, 0);
                if (n < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    return false;
                }
                p += n; len -= n;
            }
            return true;
        }

        void Reply(const std::string &s)
        {
            if (g_ctrl < 0) return;
            const std::string line = s + "\r\n";
            if (!SendAll(g_ctrl, line.data(), line.size()))
                CloseSock(g_ctrl);
        }

        int Listen(u16 port)
        {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return -1;
            sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = INADDR_ANY;
            if (bind(fd, (sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 2) < 0)
            {
                close(fd);
                return -1;
            }
            SetNonBlock(fd);
            return fd;
        }

        // データ接続を開く (PASV なら accept、PORT なら connect)
        int OpenData(void)
        {
            int fd = -1;
            if (g_pasv >= 0)
            {
                if (WaitFd(g_pasv, POLLIN, kTimeoutMs))
                    fd = accept(g_pasv, nullptr, nullptr);
                CloseSock(g_pasv);
            }
            else if (g_portSet)
            {
                fd = socket(AF_INET, SOCK_STREAM, 0);
                if (fd >= 0 && connect(fd, (sockaddr *)&g_portAddr, sizeof(g_portAddr)) < 0)
                    CloseSock(fd);
                g_portSet = false;
            }
            if (fd >= 0) SetNonBlock(fd);
            return fd;
        }

        // データ接続を閉じる。3DS の soc はいきなり close すると未送信のデータを捨てるので、
        // 送信側を shutdown して、相手が閉じる (recv が 0 を返す) まで待ってから close する。
        void CloseData(int fd)
        {
            if (fd < 0) return;
            shutdown(fd, SHUT_WR);
            for (int i = 0; i < 50; i++)
            {
                if (!WaitFd(fd, POLLIN, 100)) continue;
                char tmp[64];
                ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) break;
            }
            close(fd);
        }

        // ---------------------------------------------------------------
        //  SD カード
        // ---------------------------------------------------------------
        std::vector<u16> ToUtf16(const std::string &s)
        {
            std::vector<u16> out(s.size() + 1, 0);
            ssize_t n = utf8_to_utf16(out.data(), (const u8 *)s.c_str(), s.size());
            out.resize(n > 0 ? n + 1 : 1);
            out.back() = 0;
            return out;
        }

        std::string ToUtf8(const u16 *s)
        {
            size_t len = 0;
            while (s[len] && len < 0x106) len++;
            std::string out(len * 3 + 1, '\0');
            ssize_t n = utf16_to_utf8((u8 *)&out[0], s, len);
            out.resize(n > 0 ? n : 0);
            return out;
        }

        // 相対パスを cwd 基準の絶対パスにして、. と .. を解決する
        std::string Resolve(const std::string &arg)
        {
            std::string p = arg.empty() ? g_cwd : (arg[0] == '/' ? arg : g_cwd + "/" + arg);
            std::vector<std::string> parts;
            size_t i = 0;
            while (i <= p.size())
            {
                size_t j = p.find('/', i);
                if (j == std::string::npos) j = p.size();
                std::string part = p.substr(i, j - i);
                if (part == "..") { if (!parts.empty()) parts.pop_back(); }
                else if (!part.empty() && part != ".") parts.push_back(part);
                i = j + 1;
            }
            std::string out;
            for (const std::string &s : parts) out += "/" + s;
            return out.empty() ? "/" : out;
        }

        Result OpenFileR(Handle &h, const std::string &path)
        {
            std::vector<u16> w = ToUtf16(path);
            return FSUSER_OpenFile(&h, g_sdmc, fsMakePath(PATH_UTF16, w.data()), FS_OPEN_READ, 0);
        }

        Result OpenFileW(Handle &h, const std::string &path)
        {
            std::vector<u16> w = ToUtf16(path);
            return FSUSER_OpenFile(&h, g_sdmc, fsMakePath(PATH_UTF16, w.data()), FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
        }

        bool IsDir(const std::string &path)
        {
            if (path == "/") return true;
            std::vector<u16> w = ToUtf16(path);
            Handle d;
            if (R_FAILED(FSUSER_OpenDirectory(&d, g_sdmc, fsMakePath(PATH_UTF16, w.data()))))
                return false;
            FSDIR_Close(d);
            return true;
        }

        // ---------------------------------------------------------------
        //  転送
        // ---------------------------------------------------------------
        void SendList(const std::string &arg, bool namesOnly)
        {
            // "-la" などのオプションは無視する
            std::string a = arg;
            while (!a.empty() && a[0] == '-')
            {
                size_t sp = a.find(' ');
                a = (sp == std::string::npos) ? "" : a.substr(sp + 1);
            }
            const std::string path = Resolve(a);
            std::vector<u16> w = ToUtf16(path);
            Handle d;
            if (R_FAILED(FSUSER_OpenDirectory(&d, g_sdmc, fsMakePath(PATH_UTF16, w.data()))))
            {
                Reply("550 Directory not found.");
                return;
            }
            Reply("150 Opening data connection.");
            int fd = OpenData();
            if (fd < 0)
            {
                FSDIR_Close(d);
                Reply("425 Can't open data connection.");
                return;
            }

            bool ok = true;
            FS_DirectoryEntry ent[8];
            for (;;)
            {
                u32 n = 0;
                if (R_FAILED(FSDIR_Read(d, &n, 8, ent)) || n == 0) break;
                std::string out;
                for (u32 i = 0; i < n; i++)
                {
                    const std::string name = ToUtf8(ent[i].name);
                    const bool dir = ent[i].attributes & FS_ATTRIBUTE_DIRECTORY;
                    if (namesOnly)
                        out += name + "\r\n";
                    else
                    {
                        char line[96];
                        snprintf(line, sizeof(line), "%s 1 3ds 3ds %llu Jan  1  2000 ",
                                 dir ? "drwxrwxrwx" : "-rw-rw-rw-",
                                 dir ? 0ULL : (unsigned long long)ent[i].fileSize);
                        out += line + name + "\r\n";
                    }
                }
                // 送信元はヒープ領域のバッファにする (プラグインのメモリから大きく送ると失敗することがある)
                size_t len = out.size() < kXferBufSize ? out.size() : kXferBufSize;
                memcpy(g_xferBuf, out.data(), len);
                if (!SendAll(fd, g_xferBuf, len)) { ok = false; break; }
            }
            FSDIR_Close(d);
            CloseData(fd);
            Reply(ok ? "226 Transfer complete." : "426 Transfer aborted.");
        }

        void SendFile(const std::string &arg)
        {
            const std::string path = Resolve(arg);
            Handle h;
            if (R_FAILED(OpenFileR(h, path)))
            {
                Reply("550 File not found.");
                g_restOffset = 0;
                return;
            }
            u64 size = 0;
            FSFILE_GetSize(h, &size);
            u64 off = g_restOffset < size ? g_restOffset : size;
            g_restOffset = 0;

            Reply("150 Opening data connection.");
            int fd = OpenData();
            if (fd < 0)
            {
                FSFILE_Close(h);
                Reply("425 Can't open data connection.");
                return;
            }
            Log("送信: " + path);
            g_status = "送信中: " + path;
            Draw();

            bool ok = true;
            while (off < size)
            {
                u32 rd = 0;
                if (R_FAILED(FSFILE_Read(h, &rd, off, g_xferBuf, kXferBufSize)) || rd == 0) { ok = false; break; }
                if (!SendAll(fd, g_xferBuf, rd)) { ok = false; break; }
                off += rd;
            }
            FSFILE_Close(h);
            CloseData(fd);
            Reply(ok ? "226 Transfer complete." : "426 Transfer aborted.");
        }

        void RecvFile(const std::string &arg)
        {
            const std::string path = Resolve(arg);
            Handle h;
            if (R_FAILED(OpenFileW(h, path)))
            {
                Reply("550 Can't create file.");
                g_restOffset = 0;
                return;
            }
            u64 off = g_restOffset;
            g_restOffset = 0;
            FSFILE_SetSize(h, off);

            Reply("150 Opening data connection.");
            int fd = OpenData();
            if (fd < 0)
            {
                FSFILE_Close(h);
                Reply("425 Can't open data connection.");
                return;
            }
            Log("受信: " + path);
            g_status = "受信中: " + path;
            Draw();

            bool ok = true;
            const u64 start = off;
            u32 sum = 0;
            for (;;)
            {
                if (!WaitFd(fd, POLLIN, kTimeoutMs)) { ok = false; break; }
                ssize_t n = recv(fd, g_xferBuf, kXferBufSize, 0);
                if (n == 0) break;   // 送信完了
                if (n < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    ok = false;
                    break;
                }
                u32 wr = 0;
                for (ssize_t i = 0; i < n; i++) sum += g_xferBuf[i];
                Result wres = FSFILE_Write(h, &wr, off, g_xferBuf, (u32)n, 0);
                if (R_FAILED(wres) || wr != (u32)n)
                {
                    char e[48];
                    snprintf(e, sizeof(e), "書き込み失敗 %08lX", (unsigned long)wres);
                    Log(e);
                    ok = false;
                    break;
                }
                off += n;
            }
            FSFILE_Flush(h);   // SD に確実に書き出してから閉じる
            FSFILE_Close(h);
            CloseData(fd);

            // 書けたか確認: 閉じたあと開き直して、大きさと中身の合計を比べる
            if (ok)
            {
                u64 size = 0; u32 sum2 = 0;
                Handle r;
                if (R_SUCCEEDED(OpenFileR(r, path)))
                {
                    FSFILE_GetSize(r, &size);
                    for (u64 pos = start; pos < size; )
                    {
                        u32 rd = 0;
                        if (R_FAILED(FSFILE_Read(r, &rd, pos, g_xferBuf, kXferBufSize)) || rd == 0) break;
                        for (u32 i = 0; i < rd; i++) sum2 += g_xferBuf[i];
                        pos += rd;
                    }
                    FSFILE_Close(r);
                }
                char m[96];
                snprintf(m, sizeof(m), "受信 %llu バイト / 保存 %llu バイト%s",
                         (unsigned long long)(off - start), (unsigned long long)size,
                         (size == off && sum == sum2) ? "（一致）" : "（不一致）");
                Log(m);
                if (size != off || sum != sum2) ok = false;
            }
            Reply(ok ? "226 Transfer complete." : "451 Write verification failed.");
        }

        // ---------------------------------------------------------------
        //  コマンド
        // ---------------------------------------------------------------
        void Command(const std::string &line)
        {
            size_t sp = line.find(' ');
            std::string cmd = line.substr(0, sp);
            std::string arg = (sp == std::string::npos) ? "" : line.substr(sp + 1);
            for (char &c : cmd) if (c >= 'a' && c <= 'z') c -= 0x20;

            if (cmd == "USER")        Reply("331 Any password is OK.");
            else if (cmd == "PASS")   Reply("230 Logged in.");
            else if (cmd == "SYST")   Reply("215 UNIX Type: L8");
            else if (cmd == "FEAT")   { Reply("211-Features:"); Reply(" UTF8"); Reply(" SIZE"); Reply(" PASV"); Reply(" EPSV"); Reply(" REST STREAM"); Reply("211 End"); }
            else if (cmd == "OPTS")   Reply("200 OK.");
            else if (cmd == "NOOP")   Reply("200 OK.");
            else if (cmd == "TYPE" || cmd == "MODE" || cmd == "STRU") Reply("200 OK.");
            else if (cmd == "PWD" || cmd == "XPWD") Reply("257 \"" + g_cwd + "\" is the current directory.");
            else if (cmd == "CWD" || cmd == "XCWD" || cmd == "CDUP" || cmd == "XCUP")
            {
                const std::string p = Resolve((cmd == "CDUP" || cmd == "XCUP") ? ".." : arg);
                if (IsDir(p)) { g_cwd = p; Reply("250 OK."); }
                else Reply("550 Directory not found.");
            }
            else if (cmd == "PASV" || cmd == "EPSV")
            {
                CloseSock(g_pasv);
                g_portSet = false;
                for (int i = 0; i < kPasvPortCount && g_pasv < 0; i++)
                {
                    g_pasvPort = kPasvPortFirst + (g_pasvPort - kPasvPortFirst + 1) % kPasvPortCount;
                    g_pasv = Listen(g_pasvPort);
                }
                if (g_pasv < 0) { Reply("425 Can't open passive port."); return; }
                char buf[80];
                if (cmd == "EPSV")
                    snprintf(buf, sizeof(buf), "229 Entering Extended Passive Mode (|||%u|)", g_pasvPort);
                else
                {
                    const u8 *b = (const u8 *)&g_ip;
                    snprintf(buf, sizeof(buf), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
                             b[0], b[1], b[2], b[3], g_pasvPort >> 8, g_pasvPort & 0xFF);
                }
                Reply(buf);
            }
            else if (cmd == "PORT")
            {
                unsigned h1, h2, h3, h4, p1, p2;
                if (sscanf(arg.c_str(), "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
                { Reply("501 Bad PORT."); return; }
                CloseSock(g_pasv);
                memset(&g_portAddr, 0, sizeof(g_portAddr));
                g_portAddr.sin_family = AF_INET;
                g_portAddr.sin_port = htons((p1 << 8) | p2);
                g_portAddr.sin_addr.s_addr = htonl((h1 << 24) | (h2 << 16) | (h3 << 8) | h4);
                g_portSet = true;
                Reply("200 OK.");
            }
            else if (cmd == "LIST")   SendList(arg, false);
            else if (cmd == "NLST")   SendList(arg, true);
            else if (cmd == "RETR")   SendFile(arg);
            else if (cmd == "STOR")   RecvFile(arg);
            else if (cmd == "APPE")
            {
                Handle h; u64 size = 0;
                if (R_SUCCEEDED(OpenFileR(h, Resolve(arg)))) { FSFILE_GetSize(h, &size); FSFILE_Close(h); }
                g_restOffset = size;
                RecvFile(arg);
            }
            else if (cmd == "REST")   { g_restOffset = strtoull(arg.c_str(), nullptr, 10); Reply("350 OK."); }
            else if (cmd == "SIZE")
            {
                Handle h;
                u64 size = 0;
                if (R_SUCCEEDED(OpenFileR(h, Resolve(arg)))) { FSFILE_GetSize(h, &size); FSFILE_Close(h); Reply("213 " + std::to_string(size)); }
                else Reply("550 File not found.");
            }
            else if (cmd == "DELE")
            {
                const std::string p = Resolve(arg);
                std::vector<u16> w = ToUtf16(p);
                if (R_SUCCEEDED(FSUSER_DeleteFile(g_sdmc, fsMakePath(PATH_UTF16, w.data())))) { Log("削除: " + p); Reply("250 Deleted."); }
                else Reply("550 Can't delete.");
            }
            else if (cmd == "MKD" || cmd == "XMKD")
            {
                const std::string p = Resolve(arg);
                std::vector<u16> w = ToUtf16(p);
                if (R_SUCCEEDED(FSUSER_CreateDirectory(g_sdmc, fsMakePath(PATH_UTF16, w.data()), 0))) Reply("257 \"" + p + "\" created.");
                else Reply("550 Can't create directory.");
            }
            else if (cmd == "RMD" || cmd == "XRMD")
            {
                const std::string p = Resolve(arg);
                std::vector<u16> w = ToUtf16(p);
                if (R_SUCCEEDED(FSUSER_DeleteDirectory(g_sdmc, fsMakePath(PATH_UTF16, w.data())))) { Log("削除: " + p); Reply("250 Deleted."); }
                else Reply("550 Can't remove directory.");
            }
            else if (cmd == "RNFR")   { g_renameFrom = Resolve(arg); Reply("350 Ready for RNTO."); }
            else if (cmd == "RNTO")
            {
                if (g_renameFrom.empty()) { Reply("503 RNFR first."); return; }
                const std::string to = Resolve(arg);
                std::vector<u16> a = ToUtf16(g_renameFrom), b = ToUtf16(to);
                const FS_Path pa = fsMakePath(PATH_UTF16, a.data()), pb = fsMakePath(PATH_UTF16, b.data());
                Result r;
                if (IsDir(g_renameFrom))
                    r = FSUSER_RenameDirectory(g_sdmc, pa, g_sdmc, pb);
                else
                {
                    // 一時ファイルに送ってから名前を変えるクライアント向け: 行き先があれば消してから変える
                    Handle t;
                    if (to != g_renameFrom && R_SUCCEEDED(OpenFileR(t, to)))
                    {
                        FSFILE_Close(t);
                        FSUSER_DeleteFile(g_sdmc, pb);
                    }
                    r = FSUSER_RenameFile(g_sdmc, pa, g_sdmc, pb);
                }
                Log(std::string(R_SUCCEEDED(r) ? "名前変更: " : "名前変更失敗: ") + to);
                g_renameFrom.clear();
                Reply(R_SUCCEEDED(r) ? "250 Renamed." : "550 Can't rename.");
            }
            else if (cmd == "ABOR")   Reply("226 OK.");
            else if (cmd == "QUIT")   { Reply("221 Bye."); CloseSock(g_ctrl); }
            else                      { Log("未対応のコマンド: " + cmd); Reply("502 Command not implemented."); }
        }

        void Poll(void)
        {
            // 新しい接続 (一度に1台)
            if (g_ctrl < 0)
            {
                sockaddr_in a;
                socklen_t len = sizeof(a);
                int fd = accept(g_listen, (sockaddr *)&a, &len);
                if (fd >= 0)
                {
                    SetNonBlock(fd);
                    g_ctrl = fd;
                    g_rxBuf.clear();
                    g_cwd = "/";
                    g_renameFrom.clear();
                    g_restOffset = 0;
                    g_clientIp = IpToString(a.sin_addr.s_addr);
                    Log("接続: " + g_clientIp);
                    Reply("220 MK7_Cheat FTP Server ready.");
                }
                g_status = g_ctrl >= 0 ? "接続中: " + g_clientIp : "接続を待っています";
                return;
            }

            char buf[512];
            for (;;)
            {
                ssize_t n = recv(g_ctrl, buf, sizeof(buf), 0);
                if (n > 0) { g_rxBuf.append(buf, n); continue; }
                if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
                {
                    CloseSock(g_ctrl);
                    CloseSock(g_pasv);
                    Log("切断: " + g_clientIp);
                    g_status = "接続を待っています";
                    return;
                }
                break;
            }

            size_t nl;
            while (g_ctrl >= 0 && (nl = g_rxBuf.find('\n')) != std::string::npos)
            {
                std::string line = g_rxBuf.substr(0, nl);
                g_rxBuf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) Command(line);
            }
            g_status = g_ctrl >= 0 ? "接続中: " + g_clientIp : "接続を待っています";
        }

        u32 *AllocSocBuf(void)
        {
            for (u32 addr : kSocBufAddrs)
            {
                u32 out = 0;
                if (R_SUCCEEDED(svcControlMemoryUnsafe(&out, addr, kSocBufSize + kXferBufSize,
                        (MemOp)(MEMOP_ALLOC | MEMOP_REGION_SYSTEM), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE))))
                    return (u32 *)addr;
            }
            return nullptr;
        }

        void FreeSocBuf(void)
        {
            if (!g_socBuf) return;
            u32 out = 0;
            svcControlMemoryUnsafe(&out, (u32)g_socBuf, kSocBufSize + kXferBufSize, MEMOP_FREE, (MemPerm)0);
            g_socBuf = nullptr;
            g_xferBuf = nullptr;
        }

        void Shutdown(void)
        {
            CloseSock(g_pasv);
            CloseSock(g_ctrl);
            CloseSock(g_listen);
            if (g_socBuf) { socExit(); FreeSocBuf(); }
            if (g_sdmc) { FSUSER_CloseArchive(g_sdmc); g_sdmc = 0; }
        }

        bool Startup(std::string &err)
        {
            char buf[64];
            g_log.clear();
            // 転送用バッファも soc に渡すので、同じくヒープ領域 (soc 用の直後) に置く
            g_socBuf  = AllocSocBuf();
            g_xferBuf = g_socBuf ? (u8 *)g_socBuf + kSocBufSize : nullptr;
            if (!g_socBuf)
            {
                FreeSocBuf();
                err = "メモリが足りません";
                return false;
            }
            Result r = socInit(g_socBuf, kSocBufSize);
            if (R_FAILED(r))
            {
                FreeSocBuf();
                snprintf(buf, sizeof(buf), "通信の初期化に失敗しました (%08lX)", (unsigned long)r);
                err = buf;
                return false;
            }
            g_ip = (u32)gethostid();
            if (g_ip == 0 || g_ip == 0x0100007F)
            {
                err = "Wi-Fi に接続されていません";
                return false;
            }
            r = FSUSER_OpenArchive(&g_sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
            if (R_FAILED(r))
            {
                g_sdmc = 0;
                snprintf(buf, sizeof(buf), "SD カードを開けません (%08lX)", (unsigned long)r);
                err = buf;
                return false;
            }
            g_listen = Listen(kCtrlPort);
            if (g_listen < 0)
            {
                snprintf(buf, sizeof(buf), "ポート %u を開けません (errno %d)", kCtrlPort, errno);
                err = buf;
                return false;
            }
            g_status = "接続を待っています";
            Log("開始");
            return true;
        }
    }

    // CTRPF の下画面メニュー「FTP Server」ボタンから呼ばれる (ctrpf-patch/patch-home.sh)
    void FtpServerMenu(void)
    {
        std::string err;
        if (!Startup(err))
        {
            Shutdown();
            MessageBox("FTP Server", err)();
            return;
        }

        for (;;)
        {
            Controller::Update();
            if (Controller::IsKeyPressed(Key::B))
                break;
            Poll();
            Draw();
        }

        if (g_ctrl >= 0) Reply("421 Server shutting down.");
        Shutdown();
    }
}
