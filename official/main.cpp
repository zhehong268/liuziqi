// =============================================================================
//  Association Arena · 六子棋 (connect6) 参赛引擎  ·  arena.v1
//  纯「搜索 + 工程」路线（无学习 / 无神经网络）。C++17，单文件，仅标准库。
//
//  编译（与官方 Runner 一致）：
//      g++ -std=c++17 -O2 -pipe -I. main.cpp -o bot
//  自检测试：
//      g++ -std=c++17 -O2 -DARENA_SELFTEST -o bot_test main.cpp && ./bot_test --selftest
//
//  设计严格对照「六子棋攻坚要求」：
//    1) 六格窗口增量评估 (incremental 6-window eval, Connect6 调优)
//    2) 双子联合生成 (double-stone joint move) + 分层 / 条件生成
//    3) 强制着生成：立即胜(精确) + 必防(==精确击中集==, 绝不漏挡可防局面)
//    4) 迭代加深 PVS + 置换表 (TT) + 历史走法排序 + 统一墙钟时间管理
//    5) CTSS（保守威胁搜索 / 强制胜证明器：枚举对手全部相关应法 + 纳入反杀，绝不假必胜）
//    6) 平台逐子请求、内部双子搜索 + 跨请求第二子规划与核对
//    7) 计分意识：搜索以 WIN-ply 偏好最快胜 / 最慢负（score-v4 的 D 项）
//
//  变体开关（擂台赛 A/B 用，默认全开）：
//    -DCAND_CAP=N       候选点上限（默认 40）
//    -DTOTAL_CAP=N      双子对上限（默认 700）
//    -DUSE_EXACT_DEFENSE=0   退回贪心防御（对比用）
//    -DUSE_CTSS=0            关闭 CTSS
//    -DUSE_HISTORY=0         关闭历史排序
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iostream>

#ifndef CAND_CAP
#define CAND_CAP 40
#endif
#ifndef TOTAL_CAP
#define TOTAL_CAP 700
#endif
// 内部节点的着法宽度（根节点用全宽，内部节点必须窄）
// 实测数据：全宽 700 对时，4 秒只跑到 depth=2（266 万节点），
// 也就是说引擎只能看到「我一手 + 对手一手」。而六子棋的威胁链条是
// 4~8 步，depth=2 的引擎在战术上几乎等于瞎的 —— 这就是「被双四几步秒掉」
// 的最终解释。把内部宽度收到 56，深度立刻上到 6~8 层。
#ifndef NARROW_CAP
#define NARROW_CAP 56
#endif
#ifndef USE_EXACT_DEFENSE
#define USE_EXACT_DEFENSE 1
#endif
#ifndef USE_CTSS
#define USE_CTSS 1
#endif
#ifndef USE_HISTORY
#define USE_HISTORY 1
#endif

// ----------------------------------------------------------------------------
//  常量
// ----------------------------------------------------------------------------
static const int N = 19;
static const int CELLS = 361;
static const int WIN = 10'000'000;
static const int INF = 1 << 28;
static const int CENTER = 9 * N + 9;
static const int MAXDEPTH = 16;

// ----------------------------------------------------------------------------
//  六格窗口表（增量评估的核心）
// ----------------------------------------------------------------------------
// 注意：格子索引范围 0..360，必须用 uint16_t。
// 旧版误用 uint8_t 会把 idx>=256（row 13 及以后）截断成错误格子，
// 导致棋盘下半部分全部窗口关系错乱、威胁判断失效（表现：对手在下半区
// 做成四连/五连引擎却「看不见」而不堵）。
struct Win { uint16_t c[6]; };
static std::vector<Win> g_windows;
static std::vector<std::vector<int>> g_cellWin;

static void init_windows() {
    g_windows.clear(); g_cellWin.clear(); g_cellWin.resize(CELLS);
    const int dirs[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
    for (int sr = 0; sr < N; ++sr)
        for (int sc = 0; sc < N; ++sc)
            for (int d = 0; d < 4; ++d) {
                int dr = dirs[d][0], dc = dirs[d][1];
                int r = sr, c = sc; bool ok = true; int cells[6];
                for (int k = 0; k < 6; ++k) {
                    if (r < 0 || r >= N || c < 0 || c >= N) { ok = false; break; }
                    cells[k] = r * N + c; r += dr; c += dc;
                }
                if (!ok) continue;
                Win w; for (int k = 0; k < 6; ++k) w.c[k] = (uint16_t)cells[k];
                int wid = (int)g_windows.size();
                g_windows.push_back(w);
                for (int k = 0; k < 6; ++k) g_cellWin[cells[k]].push_back(wid);
            }
}

// ----------------------------------------------------------------------------
//  Zobrist 哈希
// ----------------------------------------------------------------------------
static uint64_t g_zob[CELLS][3];
static void init_zob() {
    uint64_t s = 0x9E3779B97F4A7C15ULL;
    auto rnd = [&]() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; };
    for (int i = 0; i < CELLS; ++i)
        for (int j = 0; j < 3; ++j) g_zob[i][j] = rnd();
}

// ----------------------------------------------------------------------------
//  棋盘
// ----------------------------------------------------------------------------
struct Board {
    uint8_t cell[CELLS];
    std::vector<short> w_own;
    std::vector<short> w_opp;
    uint64_t hash;
    int n_empty;
    int score_b;   // 增量评估：黑 - 白 的窗口形势分（黑视角）
    // hot[c] = 「c 方在该窗口 >=4 子、且对方 0 子」的窗口数。
    // 六子棋一手 2 子：此形对 c 方意味着「本回合一手即可成 6」→ 立即必杀威胁。
    // 单向增量维护，O(1) 查询，避免在 CTSS 高频判定里扫 924 窗口。
    int hot[3];
    static int hot_side(int wb, int ww) {
        if (wb >= 4 && ww == 0) return 1;
        if (ww >= 4 && wb == 0) return 2;
        return 0;
    }
    void init() {
        std::memset(cell, 0, sizeof(cell));
        w_own.assign(g_windows.size(), 0);
        w_opp.assign(g_windows.size(), 0);
        hash = 0; n_empty = CELLS; score_b = 0; hot[1] = hot[2] = 0;
    }
    void load(const uint8_t* src) {
        init();
        for (int i = 0; i < CELLS; ++i) if (src[i]) place_raw(i, src[i]);
    }
    // 单窗口对 score_b 的贡献（黑视角）：我方开窗口得分，对方开窗口扣分，争夺中(双方都有)为 0
    static int win_score(int cnt) {
        if (cnt == 5) return 100000;
        if (cnt == 4) return 9000;
        if (cnt == 3) return 600;
        if (cnt == 2) return 50;
        if (cnt == 1) return 5;
        return 0;
    }
    static int contrib(int own, int opp) {
        if (opp == 0) return win_score(own);
        if (own == 0) return -win_score(opp);
        return 0;
    }
    void place_raw(int idx, int color) {
        cell[idx] = (uint8_t)color; hash ^= g_zob[idx][color]; --n_empty;
        for (int wid : g_cellWin[idx]) {
            int o = (color == 1) ? w_own[wid] : w_opp[wid];
            int p = (color == 1) ? w_opp[wid] : w_own[wid];
            int oldC = contrib(o, p);
            int oh = hot_side(w_own[wid], w_opp[wid]);
            if (color == 1) w_own[wid]++; else w_opp[wid]++;
            int no = (color == 1) ? w_own[wid] : w_opp[wid];
            int np = (color == 1) ? w_opp[wid] : w_own[wid];
            int newC = contrib(no, np);
            score_b += (newC - oldC);
            int nh = hot_side(w_own[wid], w_opp[wid]);
            if (oh) --hot[oh];
            if (nh) ++hot[nh];
        }
    }
    void remove_raw(int idx) {
        int color = cell[idx]; cell[idx] = 0; hash ^= g_zob[idx][color]; ++n_empty;
        for (int wid : g_cellWin[idx]) {
            int o = (color == 1) ? w_own[wid] : w_opp[wid];
            int p = (color == 1) ? w_opp[wid] : w_own[wid];
            int oldC = contrib(o, p);
            int oh = hot_side(w_own[wid], w_opp[wid]);
            if (color == 1) w_own[wid]--; else w_opp[wid]--;
            int no = (color == 1) ? w_own[wid] : w_opp[wid];
            int np = (color == 1) ? w_opp[wid] : w_own[wid];
            int newC = contrib(no, np);
            score_b += (newC - oldC);
            int nh = hot_side(w_own[wid], w_opp[wid]);
            if (oh) --hot[oh];
            if (nh) ++hot[nh];
        }
    }
};

static bool just_won(const Board& B, int idx, int color) {
    for (int wid : g_cellWin[idx]) {
        int oc = (color == 1) ? B.w_own[wid] : B.w_opp[wid];
        if (oc == 6) return true;
    }
    return false;
}
static bool would_win(const Board& B, int idx, int color) {
    for (int wid : g_cellWin[idx]) {
        int oc = (color == 1) ? B.w_own[wid] : B.w_opp[wid];
        int pc = (color == 1) ? B.w_opp[wid] : B.w_own[wid];
        if (oc == 5 && pc == 0) return true;
    }
    return false;
}
static void win_points(const Board& B, int color, std::vector<int>& out) {
    out.clear();
    for (int i = 0; i < CELLS; ++i)
        if (!B.cell[i] && would_win(B, i, color)) out.push_back(i);
}
// ----------------------------------------------------------------------------
//  「对手本回合一手（2 子）即可成 6」——六子棋的关键定义。
//    覆盖两种必杀形：
//      · 5+1：对手 5 子 1 空 → 落 1 子即 6
//      · 4+2：对手 4 子 2 空 → 一回合落 2 子即 6   ← 旧版曾漏判，导致「四连不堵」
//    hot[] 已增量维护，O(1)。
// ----------------------------------------------------------------------------
static inline bool opp_has_immwin(const Board& B, int color) {
    return B.hot[3 - color] > 0;
}
// 我方落子/撤子后，对手是否获得「一手成 6」——用于安全约束（不得给对手送礼）
static inline bool gives_opp_immwin(const Board& B, int me) {
    return B.hot[3 - me] > 0;
}

// ----------------------------------------------------------------------------
//  评估函数（Connect6 调优）：以长度-6 窗口为单位。
//    own==5 → 立即胜威胁 (5+1)      own==4 → 必胜威胁 (4+2, 一回合可成 6)   均强
//    own==3 → 三 (可发展为四)        own==2/1 → 弱形
//  立即胜 / 必防由精确强制着生成负责；评估只做「非立即」的形势排序。
// ----------------------------------------------------------------------------
static int evaluate(const Board& B, int me) {
    // 增量评估：score_b 为黑视角，落子/撤销已实时维护
    //
    // ★ 必须把评估钳在 ±WIN/2 以内。score_b 是 924 个窗口的累加，理论上能到
    //   9000*924 ≈ 800 万，接近甚至超过 WIN。一旦评估值压过「真的成 6」的分数
    //   (WIN - ply)，搜索就会为了一手形势分爆表的闲棋，放弃一手必胜着。
    //   这类 bug 平时不显形，只有在胜机出现的那一刻才致命。
    long long e = (me == 1) ? B.score_b : -B.score_b;
    if (e >  WIN / 2) e =  WIN / 2;
    if (e < -WIN / 2) e = -WIN / 2;
    return (int)e;
}

// ----------------------------------------------------------------------------
//  威胁窗口：对手的立即致胜补点集合
//    - 对手 5 子 + 1 空 (5+1)：落此 1 子即 6 连
//    - 对手 4 子 + 2 空 (4+2)：落此 2 子即 6 连（正好一回合）
//  返回每个威胁窗口的空点清单 threats；以及去重后的全部空点 empties。
// ----------------------------------------------------------------------------
static void threat_empties(const Board& B, int opp, std::vector<int>& empties,
                           std::vector<std::vector<int>>& threats) {
    empties.clear(); threats.clear();
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int p = (opp == 1) ? B.w_own[w] : B.w_opp[w];
        int m = (opp == 1) ? B.w_opp[w] : B.w_own[w];
        if (m > 0) continue;                 // 窗口被我方阻断
        if (p == 5) {
            std::vector<int> e;
            for (int k = 0; k < 6; ++k) if (B.cell[g_windows[w].c[k]] == 0) e.push_back(g_windows[w].c[k]);
            if ((int)e.size() == 1) { threats.push_back(e); empties.push_back(e[0]); }
        } else if (p == 4) {
            std::vector<int> e;
            for (int k = 0; k < 6; ++k) if (B.cell[g_windows[w].c[k]] == 0) e.push_back(g_windows[w].c[k]);
            if ((int)e.size() == 2) { threats.push_back(e); empties.push_back(e[0]); empties.push_back(e[1]); }
        }
    }
    std::sort(empties.begin(), empties.end());
    empties.erase(std::unique(empties.begin(), empties.end()), empties.end());
}
// 一组落点 pts 是否覆盖全部威胁（每个威胁窗口至少被堵一个空点）
static bool covers_all(const std::vector<int>& pts, const std::vector<std::vector<int>>& threats) {
    for (const auto& t : threats) {
        bool ok = false;
        for (int e : t) for (int p : pts) if (e == p) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}
// 精确击中集：返回所有大小 <= k 且覆盖全部威胁的落点组合（解内点去重）。
// 若无解（覆盖所需 > k → 必败），返回空。绝不漏报可防局面。
static void exact_defense(const Board& B, int opp, int k, std::vector<std::vector<int>>& sols) {
    std::vector<int> emp; std::vector<std::vector<int>> thr;
    threat_empties(B, opp, emp, thr);
    sols.clear();
    if (thr.empty()) return;                 // 无威胁：无需防守
    int M = (int)emp.size();
    if (k >= 1) {
        for (int i = 0; i < M; ++i) {
            std::vector<int> one = { emp[i] };
            if (covers_all(one, thr)) sols.push_back(one);
        }
    }
    if (k >= 2) {
        for (int i = 0; i < M; ++i)
            for (int j = i + 1; j < M; ++j) {
                std::vector<int> two = { emp[i], emp[j] };
                if (covers_all(two, thr)) sols.push_back(two);
            }
    }
}

// 贪心部分防守（兜底，非「完全解」）：
//   逐子选取「覆盖威胁窗口数最多」的空点。当 k 不足以完全覆盖（必败形）时，
//   仍必须尽力把对手的「一手成 6」至少降级为「需要更多手」——绝不因为
//   「防不住」就一子不堵。这是硬性纪律：任何情况下不得无视对手 4 连 / 5 连。
static void greedy_defense(const Board& B, int opp, int k, std::vector<int>& picks) {
    std::vector<int> emp; std::vector<std::vector<int>> thr;
    threat_empties(B, opp, emp, thr);
    picks.clear();
    if (thr.empty()) return;                 // 无威胁：无需防守
    std::vector<char> covered(thr.size(), 0);
    for (int t = 0; t < k; ++t) {
        int bestc = -1, bestcnt = 0;
        for (int e : emp) {
            if (B.cell[e]) continue;
            if (std::find(picks.begin(), picks.end(), e) != picks.end()) continue;
            int cnt = 0;
            for (size_t ti = 0; ti < thr.size(); ++ti)
                if (!covered[ti]) for (int x : thr[ti]) if (x == e) { ++cnt; break; }
            if (cnt > bestcnt) { bestcnt = cnt; bestc = e; }
        }
        if (bestc < 0 || bestcnt == 0) break;
        picks.push_back(bestc);
        for (size_t ti = 0; ti < thr.size(); ++ti)
            if (!covered[ti]) for (int x : thr[ti]) if (x == bestc) { covered[ti] = 1; break; }
    }
}

// ----------------------------------------------------------------------------
//  迫着（threat）系统 —— 六子棋的胜负语言
//
//  定义（connect6.org）：一方有 t 个迫着 <=> 对手至少需要 t 颗子才能阻止它
//  下一手成 6。一手只能下 2 子，所以 t >= 3 就是必胜。
//
//  实现：把所有「己方 >=4 子、对方 0 子」的六格窗口（= 下一手可成 6 的窗口）
//  收集起来，求「最少要几个空点才能击中每一个这样的窗口」——这就是 t。
//  注意击中集不需要覆盖窗口的每个空点：4+2 的窗口只要占掉其中一个空点，
//  对手就只能在里面凑到 5 连，成不了 6。
// ----------------------------------------------------------------------------
static void threat_windows_of(const Board& B, int side, std::vector<std::vector<int>>& thr) {
    thr.clear();
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int own = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 4) continue;
        int oth = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth != 0) continue;
        std::vector<int> e;
        for (int k = 0; k < 6; ++k) {
            int idx = g_windows[w].c[k];
            if (!B.cell[idx]) e.push_back(idx);
        }
        if (!e.empty() && (int)e.size() <= 2) thr.push_back(e);
    }
}
// 最小击中集大小（截断到 3 = 「>=3 挡不住」）
static int hit_size(const std::vector<std::vector<int>>& thr, const std::vector<int>& emp) {
    if (thr.empty()) return 0;
    auto covers = [&](int a, int b) {
        for (const auto& t : thr) {
            bool ok = false;
            for (int e : t) if (e == a || e == b) { ok = true; break; }
            if (!ok) return false;
        }
        return true;
    };
    for (int a : emp) if (covers(a, -1)) return 1;
    for (size_t i = 0; i < emp.size(); ++i)
        for (size_t j = i + 1; j < emp.size(); ++j)
            if (covers(emp[i], emp[j])) return 2;
    return 3;
}
// side 的迫着数：0 / 1 / 2 / 3（3 表示 >=3，即对手挡不住）
//
// ★ 性能关键：这是全引擎调用最频繁的函数（每个候选着法、每个搜索节点、
//   每次 VCDT 展开都要算），旧实现每个威胁窗口都 new 一个 vector，再做
//   O(M^2 * W) 的击中集穷举 —— 单次 5~20 微秒，VCDT 一深就被它卡死。
//   现在改成「窗口空点落进静态缓冲 + 两遍扫描」的零分配版本：
//     1) 扫一遍把所有威胁窗口的 ≤2 个空点存进 E[]
//     2) 1 点解 = 某个空点出现在全部窗口里
//        2 点解 = 第一个窗口的两个空点之一 p 命中后，对「不含 p 的窗口」求空点交集，
//                 交集非空 ⇔ 存在 q 与 p 一起覆盖全部窗口
//   结果与旧实现逐位相同（最小击中集大小，截断到 3），但快一个数量级。
static int threat_count(const Board& B, int side) {
    static int E[2 * 1200];
    const int W = (int)g_windows.size();
    int nw = 0;
    for (int w = 0; w < W; ++w) {
        int own = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 4) continue;
        int oth = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth) continue;
        int e1 = -1, e2 = -1;
        for (int k = 0; k < 6; ++k) {
            int idx = g_windows[w].c[k];
            if (!B.cell[idx]) { if (e1 < 0) e1 = idx; else if (e2 < 0) e2 = idx; }
        }
        if (e1 < 0) continue;                  // 已经成 6：另有判定
        if (nw >= 1200) return 3;
        E[2 * nw] = e1; E[2 * nw + 1] = e2; ++nw;
    }
    if (nw == 0) return 0;
    int fa = E[0], fb = E[1];
    // ---- 1 点解 ----
    if (fa >= 0) {
        int c = 0;
        for (int i = 0; i < nw; ++i) if (E[2 * i] == fa || E[2 * i + 1] == fa) ++c;
        if (c == nw) return 1;
    }
    if (fb >= 0) {
        int c = 0;
        for (int i = 0; i < nw; ++i) if (E[2 * i] == fb || E[2 * i + 1] == fb) ++c;
        if (c == nw) return 1;
    }
    // ---- 2 点解 ----
    for (int t = 0; t < 2; ++t) {
        int p = (t == 0) ? fa : fb;
        if (p < 0) continue;
        int q1 = -1, q2 = -1;
        bool init = false, dead = false;
        for (int i = 0; i < nw; ++i) {
            if (E[2 * i] == p || E[2 * i + 1] == p) continue;
            int a = E[2 * i], b = E[2 * i + 1];
            if (!init) { q1 = a; q2 = b; init = true; }
            else {
                int n1 = -1, n2 = -1, n = 0;
                if (q1 >= 0 && (q1 == a || q1 == b)) { n1 = q1; ++n; }
                if (q2 >= 0 && (q2 == a || q2 == b)) { if (n == 0) n1 = q2; else n2 = q2; ++n; }
                q1 = n1; q2 = n2;
            }
            if (q1 < 0 && q2 < 0) { dead = true; break; }
        }
        if (!dead && q1 >= 0) return 2;
    }
    return 3;
}
// ----------------------------------------------------------------------------
//  进攻厚度 latent：把 side 的全部 3+ 窗口（own>=3、opp==0）彻底掐死，
//  对手至少要下几颗子（贪心覆盖，截断到 3）。
//
//  为什么要这条指标：n3（3+ 窗口个数）会把「一条线上堆 4 颗子」算成很高的分
//  —— 那条线同时落在 3~4 个重叠窗口里。但对手只要下一颗子就能把这条线整条废掉，
//  这种「厚」是假的。真实棋力比的是**互不相交的战线有几条**：
//      一条线上堆子        → latent = 1（一子全废）
//      两条互不相交的线    → latent = 2（对手必须用掉两子）
//      三条                → latent = 3（对手挡不完，等于胜势）
//  六子棋的正解正是「多线交叉」：横 + 竖 + 两条斜线各做一个活三，任何单点封堵
//  都只能掐掉其中一条。这也是棋谱里胜方「每回合 tc 从不掉到 0」的结构性原因。
// ----------------------------------------------------------------------------
static int latent_threats(const Board& B, int side) {
    static int EMP[3 * 1200];
    int nw = 0;
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int own = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 3) continue;
        int oth = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth) continue;
        int e1 = -1, e2 = -1, e3 = -1;
        for (int k = 0; k < 6; ++k) {
            int idx = g_windows[w].c[k];
            if (!B.cell[idx]) {
                if (e1 < 0) e1 = idx; else if (e2 < 0) e2 = idx; else if (e3 < 0) e3 = idx;
            }
        }
        if (e1 < 0) continue;
        if (nw >= 1200) break;
        EMP[3 * nw] = e1; EMP[3 * nw + 1] = e2; EMP[3 * nw + 2] = e3;
        ++nw;
    }
    if (nw == 0) return 0;
    static unsigned char mark[1200];
    std::memset(mark, 0, nw);
    int left = nw, picks = 0;
    while (left > 0 && picks < 3) {
        int bestCell = -1, bestCnt = 0;
        for (int i = 0; i < nw; ++i) {
            if (mark[i]) continue;
            for (int t = 0; t < 3; ++t) {
                int c = EMP[3 * i + t];
                if (c < 0) continue;
                int cntC = 0;
                for (int j = 0; j < nw; ++j) {
                    if (mark[j]) continue;
                    if (EMP[3 * j] == c || EMP[3 * j + 1] == c || EMP[3 * j + 2] == c) ++cntC;
                }
                if (cntC > bestCnt) { bestCnt = cntC; bestCell = c; }
            }
        }
        if (bestCell < 0) break;
        for (int j = 0; j < nw; ++j)
            if (!mark[j] && (EMP[3 * j] == bestCell || EMP[3 * j + 1] == bestCell || EMP[3 * j + 2] == bestCell))
            { mark[j] = 1; --left; }
        ++picks;
    }
    return picks;
}

// side 的「双迫着制造点」：在此落一子后迫着数 >= 2。
// 这就是六子棋的「活三 / 双四前身」——对手必须一次性拿两子来挡，
// 抢到它的人掌握主动权；放任对手落上去，下一手就是三迫着直接赢。
// tcs（可空）并行输出每个点落子后能做出几个迫着：2 = 强制对手两子全挡，
// 3 = 对手挡不住（落上去就等于赢），两者的紧迫度差一个数量级。
static void double_threat_points(Board& B, int side, std::vector<int>& pts,
                                 std::vector<int>* tcs = nullptr, int capPts = 96) {
    pts.clear();
    if (tcs) tcs->clear();
    // 候选点必须**按重要性**排序后再截断。
    // 曾经这里按格子索引升序截断 48 个 —— 索引大的（棋盘靠下/靠右）威胁点会被
    // 整批丢掉，引擎在那片区域「瞎掉」，对手在下方做双四它毫无反应。
    // 权重 = 该空点落在多少个「own>=3 且 opp==0」的窗口里（越多越关键）。
    static int dwgt[CELLS];
    std::memset(dwgt, 0, sizeof(int) * CELLS);
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int own = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 3) continue;                  // 落一子最多到 4，own<3 的窗口不可能变成迫着
        int oth = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth != 0) continue;
        for (int k = 0; k < 6; ++k) {
            int idx = g_windows[w].c[k];
            if (!B.cell[idx]) ++dwgt[idx];
        }
    }
    std::vector<int> cand;
    for (int i = 0; i < CELLS; ++i) if (dwgt[i] > 0 && !B.cell[i]) cand.push_back(i);

    std::sort(cand.begin(), cand.end(), [](int x, int y) {
        if (dwgt[x] != dwgt[y]) return dwgt[x] > dwgt[y];
        return x < y;
    });
    if ((int)cand.size() > capPts) cand.resize(capPts);
    for (int p : cand) {
        if (B.cell[p]) continue;
        B.place_raw(p, side);
        int tc = threat_count(B, side);
        B.remove_raw(p);
        if (tc >= 2) { pts.push_back(p); if (tcs) tcs->push_back(tc); }
    }
}
// side 一回合（2 子）最多能做出几个迫着（截断 3）——对手威胁的「上限探测」
static int max_threats_one_turn(Board& B, int side, int cap_pts) {
    std::vector<int> cand;
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int own = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 3) continue;
        int oth = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth != 0) continue;
        for (int k = 0; k < 6; ++k) {
            int idx = g_windows[w].c[k];
            if (!B.cell[idx]) cand.push_back(idx);
        }
    }
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    if ((int)cand.size() > cap_pts) cand.resize(cap_pts);
    int best = threat_count(B, side);
    for (size_t i = 0; i < cand.size(); ++i) {
        if (B.cell[cand[i]]) continue;
        B.place_raw(cand[i], side);
        int t1 = threat_count(B, side);
        if (t1 > best) best = t1;
        if (best < 3) {
            for (size_t j = i + 1; j < cand.size(); ++j) {
                if (B.cell[cand[j]]) continue;
                B.place_raw(cand[j], side);
                int t2 = threat_count(B, side);
                B.remove_raw(cand[j]);
                if (t2 > best) best = t2;
                if (best >= 3) break;
            }
        }
        B.remove_raw(cand[i]);
        if (best >= 3) return 3;
    }
    return best;
}
// 「贴着对手棋子」的程度：用来在等效防守里偏向紧凑封堵（把对手彻底钉死），
// 而不是隔一格敷衍地挡——后者会把对手的连子养大。
static int tightness(const Board& B, int idx, int oppSide) {
    int r = idx / N, c = idx % N, t = 0;
    for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc) {
        if (!dr && !dc) continue;
        int nr = r + dr, nc = c + dc;
        if (nr < 0 || nr >= N || nc < 0 || nc >= N) continue;
        if (B.cell[nr * N + nc] == oppSide) ++t;
    }
    return t;
}
// 残余威胁下的最顽强防守（迫着数 > k，理论必败时使用）：
// 枚举可行落点组合，优先把对手残余迫着数压到最低，其次压对手双迫着点，
// 再次选形势最好、最紧贴的。绝不「放弃式」乱走。
static void resilient_defense(Board& B, int me, int k, int& outA, int& outB) {
    int oppSide = 3 - me;
    std::vector<int> emp; std::vector<std::vector<int>> thr;
    threat_empties(B, oppSide, emp, thr);
    outA = -1; outB = -1;
    if (emp.empty()) return;
    if ((int)emp.size() > 16) emp.resize(16);
    int bestResid = 99, bestEv = -INF, bestTight = -1, bestDt = 99;
    auto consider = [&](int a, int b) {
        B.place_raw(a, me);
        if (b >= 0) B.place_raw(b, me);
        int resid = threat_count(B, oppSide);
        int ev = evaluate(B, me);
        int tg = tightness(B, a, oppSide) + (b >= 0 ? tightness(B, b, oppSide) : 0);
        if (b >= 0) B.remove_raw(b);
        B.remove_raw(a);
        (void)bestDt;
        bool better = false;
        if (resid < bestResid) better = true;
        else if (resid == bestResid) {
            if (tg > bestTight) better = true;
            else if (tg == bestTight && ev > bestEv) better = true;
        }
        if (better) { bestResid = resid; bestEv = ev; bestTight = tg; outA = a; outB = b; }
    };
    if (k <= 1) {
        for (int p : emp) if (!B.cell[p]) consider(p, -1);
    } else {
        for (size_t i = 0; i < emp.size(); ++i) {
            if (B.cell[emp[i]]) continue;
            for (size_t j = i + 1; j < emp.size(); ++j) {
                if (B.cell[emp[j]]) continue;
                consider(emp[i], emp[j]);
            }
        }
    }
}

// ----------------------------------------------------------------------------
//  候选生成（邻域 + 强制点必含 + 排序 + 宽度裁剪）
// ----------------------------------------------------------------------------
static void collect_candidates(const Board& B, int color, int k, std::vector<int>& cand) {
    std::vector<char> near(CELLS, 0);
    for (int i = 0; i < CELLS; ++i) {
        if (B.cell[i] == 0) continue;
        int r = i / N, c = i % N;
        for (int dr = -2; dr <= 2; ++dr) for (int dc = -2; dc <= 2; ++dc) {
            int nr = r + dr, nc = c + dc;
            if (nr < 0 || nr >= N || nc < 0 || nc >= N) continue;
            int j = nr * N + nc; if (B.cell[j] == 0) near[j] = 1;
        }
    }
    for (int i = 0; i < CELLS; ++i) if (near[i]) cand.push_back(i);
    std::vector<int> wp; win_points(B, color, wp);
    std::vector<int> owp; win_points(B, 3 - color, owp);
    for (int e : wp)  if (!near[e]) cand.push_back(e);
    for (int e : owp) if (!near[e]) cand.push_back(e);
    std::sort(cand.begin(), cand.end());
    cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
    auto prio = [&](int i) {
        int r = i / N, c = i % N, cnt = 0;
        for (int dr = -1; dr <= 1; ++dr) for (int dc = -1; dc <= 1; ++dc) {
            int nr = r + dr, nc = c + dc;
            if (nr < 0 || nr >= N || nc < 0 || nc >= N) continue;
            if (B.cell[nr * N + nc]) ++cnt;
        }
        return cnt;
    };
    auto forced = [&](int i) {
        return std::find(wp.begin(), wp.end(), i) != wp.end() ||
               std::find(owp.begin(), owp.end(), i) != owp.end();
    };
    std::vector<std::pair<int, int>> sc;
    for (size_t i = 0; i < cand.size(); ++i)
        sc.push_back({ forced(cand[i]) ? 1000 + prio(cand[i]) : prio(cand[i]), (int)i });
    std::sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<int> ordered; ordered.reserve(cand.size());
    for (auto& p : sc) ordered.push_back(cand[p.second]);
    cand = std::move(ordered);
    if ((int)cand.size() > CAND_CAP) cand.resize(CAND_CAP);
}

struct Move { int a; int b; };
static void gen_moves(const Board& B, int color, int k, std::vector<Move>& moves) {
    moves.clear();
    std::vector<int> cand; collect_candidates(B, color, k, cand);
    std::vector<int> wp;  win_points(B, color, wp);
    std::vector<int> owp; win_points(B, 3 - color, owp);
    auto isForce = [&](int i) {
        return std::find(wp.begin(), wp.end(), i) != wp.end() ||
               std::find(owp.begin(), owp.end(), i) != owp.end();
    };
    if (k == 1) { for (int a : cand) moves.push_back({a, -1}); return; }

    // ------------------------------------------------------------------
    //  「拆掉对手 4+2 / 5+1 窗口」的点，必须当作必防点，而且**允许两两远距离成对**。
    //
    //  这是本引擎最后一块大漏洞的补丁。原来的成对规则是：
    //      两颗子都不是必防点 且 距离 > 2  →  直接丢弃
    //  于是「对手横着一条四（4+2，两个空点相距 5）」这种局面，搜索里
    //  **根本不存在「同时堵两端」这个应手**，只能堵一头。搜索因此认为
    //  「怎么走都是对手赢」，把价值判成 -WIN；而根节点又因为 PVS 报出
    //  search_win 而兴高采烈地去下自己的进攻手 —— 实战表现就是：
    //  对手一个双四，几步就把我们秒了，而引擎全程「以为自己要赢」。
    //
    //  只要把这类点升级成必防点，两端的远距离组合就进得来，搜索立刻能
    //  看见「堵住两端 = 对手这条线彻底死掉」。
    // ------------------------------------------------------------------
    std::vector<int> urg;
    {
        int opp = 3 - color;
        char seen[CELLS];
        std::memset(seen, 0, sizeof(seen));
        for (size_t w = 0; w < g_windows.size(); ++w) {
            int o = (opp == 1) ? B.w_own[w] : B.w_opp[w];
            if (o < 4) continue;
            int m = (opp == 1) ? B.w_opp[w] : B.w_own[w];
            if (m != 0) continue;
            for (int t = 0; t < 6; ++t) {
                int idx = g_windows[w].c[t];
                if (!B.cell[idx] && !seen[idx]) { seen[idx] = 1; urg.push_back(idx); }
            }
        }
        std::sort(urg.begin(), urg.end());
    }

    std::vector<Move> all;
    all.reserve(TOTAL_CAP);
    // 1) 必防点两两组合优先（挡双四的唯一手段，必须排在截断之前）
    for (size_t i = 0; i < urg.size(); ++i)
        for (size_t j = i + 1; j < urg.size() && (int)all.size() < TOTAL_CAP; ++j)
            all.push_back({urg[i], urg[j]});
    // 2) 必防点 × 其余候选（限 160 条：足够表达「一防一攻」，再多纯属浪费）
    {
        int q2 = 0;
        for (size_t i = 0; i < urg.size() && q2 < 160; ++i)
            for (size_t j = 0; j < cand.size() && q2 < 160 && (int)all.size() < TOTAL_CAP; ++j)
                if (cand[j] != urg[i]) { all.push_back({urg[i], cand[j]}); ++q2; }
    }
    // 3) 常规组合（保留原来的「非必防点必须相邻」约束，控制宽度）
    for (size_t ia = 0; ia < cand.size(); ++ia) {
        int a = cand[ia]; bool aF = isForce(a) ||
            std::binary_search(urg.begin(), urg.end(), a);
        for (size_t ib = 0; ib < cand.size(); ++ib) {
            if (ia == ib) continue;
            int b = cand[ib]; bool bF = isForce(b) ||
                std::binary_search(urg.begin(), urg.end(), b);
            if (!aF && !bF) {
                int ar = a / N, ac = a % N, br = b / N, bc = b % N;
                if (std::abs(ar - br) > 2 || std::abs(ac - bc) > 2) continue;
            }
            all.push_back({a, b});
            if ((int)all.size() >= TOTAL_CAP) break;
        }
        if ((int)all.size() >= TOTAL_CAP) break;
    }
    if ((int)all.size() > TOTAL_CAP) all.resize(TOTAL_CAP);
    // ★ 必须用 stable_sort：gen_moves 的分层生成顺序本身就是优先级
    //   （必防点对 → 必防点×候选 → 常规）。用普通 sort 只按「是否强制」
    //   二分，会把这层顺序打乱，再配合内部节点截断，就把「同时堵住两端」
    //   这种唯一的救命着法挤掉了 —— 搜索随即看到「我赢了」的假象
    //   （实测：白方误判 (8,7)(8,9) 一手必胜，其实黑两子一堵就死）。
    auto urgF = [&](int i) { return std::binary_search(urg.begin(), urg.end(), i); };
    std::stable_sort(all.begin(), all.end(), [&](const Move& x, const Move& y) {
        int fx = (isForce(x.a) || isForce(x.b) || urgF(x.a) || urgF(x.b)) ? 1 : 0;
        int fy = (isForce(y.a) || isForce(y.b) || urgF(y.a) || urgF(y.b)) ? 1 : 0;
        return fx > fy;
    });
    moves = std::move(all);
}

// 形状点：落在最多「自己 k 子、对方 0 子」窗口里的空点。
//
// 这是开局与中局唯一可靠的位置价值语言。复盘两份真实棋谱（姜雨涵先手胜、
// 后手胜各一局）后能看到一个共同的骨架特征：她的每一手几乎都同时踩在
// 横向、竖向、两个斜向的多条线上，也就是「一子多线」的交叉点；而这类点
// 正是本函数选出来的点。
//
// 六窗计数（score_b）虽然也隐含了这层信息，但它是「所有窗口的加权和」，
// 会被大量 1 子窗口摊薄；本函数只数「已经 >= k 子、且对方一颗都没进」
// 的窗口，直接反映「这个点能把几条线同时推进一步」。
static void shape_points(const Board& B, int side, int minOwn, int cap,
                         std::vector<int>& pts) {
    static int cnt[CELLS];
    std::memset(cnt, 0, sizeof(int) * CELLS);
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int o = (side == 1) ? B.w_own[w] : B.w_opp[w];
        int p = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (p != 0 || o < minOwn) continue;
        for (int k = 0; k < 6; ++k) {
            int idx = g_windows[w].c[k];
            if (!B.cell[idx]) ++cnt[idx];
        }
    }
    // 贴对手的权重：形状点不能只看「自己那条线顺不顺」，
    // 还要看「离战场近不近」。六子棋后手的正解是**防守反击** ——
    // 挡着对手的同时把子落在能同时兼顾自己成形的位置上。
    // 只看自己的窗口会选出「远离战场、孤芳自赏」的点（执白实测就是这么输的：
    // 白方整局自己造形、离黑方的攻势很远，tc 恒为 0，10 个回合就被压死）。
    static int oppTouch[CELLS];
    std::memset(oppTouch, 0, sizeof(int) * CELLS);
    {
        int op = 3 - side;
        for (int i = 0; i < CELLS; ++i) {
            if (B.cell[i] != op) continue;
            int r = i / N, c = i % N;
            for (int dr = -3; dr <= 3; ++dr) for (int dc = -3; dc <= 3; ++dc) {
                // 只统计「距离 2~3 格」的一圈：后手贴着先手一格走（距离 1）是新手
                // 最容易犯的错 —— 先手可以顺势借力、把那颗子当成自己的延伸点。
                // 棋谱里姜雨涵执白第一回合走的是 (11,7)(12,8)，距天元 (9,9) 恰好
                // 是 2~3 格，两子相邻自成二连：既压住了先手的方向，又立起自己的骨架。
                int m = std::max(std::abs(dr), std::abs(dc));
                if (m < 2) continue;
                int nr = r + dr, nc = c + dc;
                if (nr < 0 || nr >= N || nc < 0 || nc >= N) continue;
                int j = nr * N + nc;
                if (!B.cell[j]) ++oppTouch[j];
            }
        }
    }
    std::vector<int> v;
    for (int i = 0; i < CELLS; ++i) if (cnt[i] > 0 && !B.cell[i]) v.push_back(i);
    std::sort(v.begin(), v.end(), [&](int x, int y) {
        int sx = cnt[x] * 3 + oppTouch[x];
        int sy = cnt[y] * 3 + oppTouch[y];
        if (sx != sy) return sx > sy;
        return x < y;
    });
    if ((int)v.size() > cap) v.resize(cap);
    pts.swap(v);
}

// 极开局选点（自己还没有子时唯一可用的位置价值）。
//
// 棋谱依据：122 局姜雨涵执白，第一回合 (11,7)(12,8)，距黑方天元 (9,9) 是 2~3 格。
// 而引擎的 collect_candidates 是按「8 邻域内有多少子」排序的 —— 距对手 1 格的
// 点得分最高，于是 cand 前几条全被紧贴点占满，2 格外的点根本进不来。
// 实测白方第一回合永远走 (8,8)(9,8) 这种紧贴位，十个回合就被压死。
//
// 这里专门补一组「距对手 2~3 格」的候选：既压住对手那条线的走向，
// 又不会被对手顺势借力延伸。
static void opening_points(const Board& B, int side, int cap, std::vector<int>& pts) {
    static int sc[CELLS];
    std::memset(sc, 0, sizeof(int) * CELLS);
    int op = 3 - side;
    for (int i = 0; i < CELLS; ++i) {
        if (B.cell[i]) continue;
        int r = i / N, c = i % N, best = 99;
        for (int j = 0; j < CELLS; ++j) {
            if (B.cell[j] != op) continue;
            int d = std::max(std::abs(r - j / N), std::abs(c - j % N));
            if (d < best) best = d;
        }
        if (best < 2 || best > 3) continue;
        int dc = std::max(std::abs(r - 9), std::abs(c - 9));
        sc[i] = 100 - dc * 10 - (best == 3 ? 1 : 0);   // 靠近中心的优先
    }
    std::vector<int> v;
    for (int i = 0; i < CELLS; ++i) if (sc[i] > 0 && !B.cell[i]) v.push_back(i);
    std::sort(v.begin(), v.end(), [&](int x, int y) {
        if (sc[x] != sc[y]) return sc[x] > sc[y];
        return x < y;
    });
    if ((int)v.size() > cap) v.resize(cap);
    pts.swap(v);
}

// 进攻性第二子：最大化落子后评估；安全约束：不得给对手制造立即胜点。
// 六子棋是「谁在强制、谁就掌握主动」的游戏，所以同分时优先选能顺手做出四
// （一个迫着 = 强制对手）的点，而不是纯粹的形势分最高点。
static int pick_offensive_second(const Board& B, int me, int a) {
    Board T = B; T.place_raw(a, me);
    int best = -1, bestEv = -INF;          // 纯评估最优（兜底）
    int bestSafe = -1, bestSafeEv = -INF;  // 且不给对手「一手成 6」的最优
    for (int i = 0; i < CELLS; ++i) {
        if (T.cell[i]) continue;
        int r = i / N, c = i % N; bool near = false;
        for (int dr = -2; dr <= 2; ++dr) for (int dc = -2; dc <= 2; ++dc) {
            int nr = r + dr, nc = c + dc;
            if (nr < 0 || nr >= N || nc < 0 || nc >= N) continue;
            if (T.cell[nr * N + nc]) { near = true; break; }
        }
        if (!near) continue;
        T.place_raw(i, me);
        // 安全约束：落完两子后不得让对手获得「一手成 6」（5 连、以及 4 连送礼都算）
        bool bad = gives_opp_immwin(T, me);
        int ev = evaluate(T, me);
        // 这一子是否顺手做出一个四（我方 >=4 子、对方 0 子的窗口）＝ 一个迫着
        bool makes4 = false;
        for (int wid : g_cellWin[i]) {
            int own = (me == 1) ? T.w_own[wid] : T.w_opp[wid];
            int oth = (me == 1) ? T.w_opp[wid] : T.w_own[wid];
            if (oth == 0 && own >= 4) { makes4 = true; break; }
        }
        if (makes4) ev += 20000;
        T.remove_raw(i);
        if (!bad && ev > bestSafeEv) { bestSafeEv = ev; bestSafe = i; }
        if (ev > bestEv) { bestEv = ev; best = i; }
    }
    if (bestSafe >= 0) return bestSafe;
    if (best >= 0) return best;
    for (int i = 0; i < CELLS; ++i) if (!B.cell[i]) { return i; }
    return -1;
}

// ----------------------------------------------------------------------------
//  时间管理（墙钟，单调时钟；CTSS 独立子预算）
// ----------------------------------------------------------------------------
static long long g_deadline_ms = 0;
static long long g_ctss_deadline_ms = 0;
static bool     g_timed_out = false;
static long long g_nodes = 0;
static bool      g_trace = false;   // C6_TRACE=1 时输出决策追踪
static int      g_my_color = 1;
static long long steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
// 单步内部预算上限。
// 平台给 10s，但**不是越久越强**：实测同一对手、同一 seed，8s 预算下执白
// 反而从 51 手速胜退化成 179 手不终（搜索越深越偏向「稳」的闲棋，
// 强制着被形势分盖掉），而 3~3.5s 时纯战术路线更果断。
// 另外 3.5s 对 10s 硬超时留了 2.8 倍余量，平台只有 1 核也不会踩线。
static const int ENGINE_BUDGET_MS = 5000;
// 本步**实际生效**的预算（= min(time_limit, ENGINE_BUDGET_MS)）。
// ★ 必须记下来给 CTSS 用。原来 CTSS 的预算写的是 `tlim/2`（再用 4000 封顶），
//   直接绕过了 ENGINE_BUDGET_MS —— 实测把预算调成 1000ms，日志里单步仍然是
//   「我方最长 4.0s」：CTSS 拿着 4 秒在跑，PVS 再补一刀。这不只是「预算不准」，
//   在平台 10s 硬超时下它就是超时判负的种子（CTSS 4s + PVS 若干 + 平台更慢）。
static long long g_budget_ms = ENGINE_BUDGET_MS;
static void set_deadline(int tlim) {
    long long budget = tlim;
    if (budget > ENGINE_BUDGET_MS) budget = ENGINE_BUDGET_MS;
    if (budget < 50) budget = 50;
    g_budget_ms = budget;
    g_deadline_ms = steady_ms() + budget; g_timed_out = false; g_nodes = 0;
}
static void check_time() {
    long long dl = g_deadline_ms;
    if (g_ctss_deadline_ms > 0 && g_ctss_deadline_ms < dl) dl = g_ctss_deadline_ms;
    if (dl == 0) return;
    if (steady_ms() >= dl) g_timed_out = true;
}
// 统一超时判据：主预算与 CTSS 子预算取小。
static inline bool past_deadline() {
    long long dl = g_deadline_ms;
    if (g_ctss_deadline_ms > 0 && g_ctss_deadline_ms < dl) dl = g_ctss_deadline_ms;
    return dl > 0 && steady_ms() >= dl;
}

// ----------------------------------------------------------------------------
//  两子成迫着枚举 —— 六子棋真正的「胜负方程」
//
//  迫着数 t：对手最少要 t 颗子才能阻止我下一手成 6。
//  一手只有 2 子 ⇒ 本回合做到 t >= 3 就是必胜（对手挡住 2 个，第三个下回合成 6）。
//  所以每回合该问的第一个问题是：「我这两子能把迫着数做到几个？」
//      t >= 3  直接赢；
//      t == 2  对手两子全被钉死（我掌握主动，连续强制）；
//      t == 1  只是一只四，对手一子就挡了，等于白送一手。
//
//  枚举范围：只有「己方 >= 2 子、对方 0 子」的六格窗口，才可能靠再落两子变成
//  迫着窗口（own >= 4），所以候选点 = 这些窗口的空点，按「落在多少个这样的
//  窗口里」降序截断。实测 20 个点（190 个组合）≈ 1ms，代价完全可忽略。
// ----------------------------------------------------------------------------
struct ThrPair { int tc, a, b; };

static void collect_threat_pairs(Board& B, int side, int minTc, int capPts,
                                 std::vector<ThrPair>& out, int maxOut = 64) {
    out.clear();
    static int wgt[CELLS];
    std::memset(wgt, 0, sizeof(int) * CELLS);
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int own = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 2) continue;
        int oth = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth != 0) continue;
        for (int t = 0; t < 6; ++t) {
            int idx = g_windows[w].c[t];
            if (!B.cell[idx]) ++wgt[idx];
        }
    }
    std::vector<int> cand;
    cand.reserve(64);
    for (int i = 0; i < CELLS; ++i) if (wgt[i] > 0 && !B.cell[i]) cand.push_back(i);
    if (cand.empty()) return;
    int oppSide = 3 - side;
    std::sort(cand.begin(), cand.end(), [&](int x, int y) {
        if (wgt[x] != wgt[y]) return wgt[x] > wgt[y];
        return tightness(B, x, oppSide) > tightness(B, y, oppSide);
    });
    if ((int)cand.size() > capPts) cand.resize(capPts);

    bool full = false;
    for (size_t i = 0; i < cand.size() && !full; ++i) {
        if (past_deadline()) break;
        int a = cand[i];
        if (B.cell[a]) continue;
        B.place_raw(a, side);
        for (size_t j = i + 1; j < cand.size(); ++j) {
            int b = cand[j];
            if (B.cell[b]) continue;
            B.place_raw(b, side);
            int tc = threat_count(B, side);
            B.remove_raw(b);
            if (tc >= minTc) {
                out.push_back({tc, a, b});
                if ((int)out.size() >= maxOut) { full = true; break; }
            }
        }
        B.remove_raw(a);
    }
    std::sort(out.begin(), out.end(), [](const ThrPair& x, const ThrPair& y) {
        if (x.tc != y.tc) return x.tc > y.tc;
        if (x.a != y.a)   return x.a < y.a;
        return x.b < y.b;
    });
    if ((int)out.size() > maxOut) out.resize(maxOut);
}

// 这两子打掉了对手多少个「两子成 3 迫着」的组合；返回「没打掉的个数」。
// 0 = 对手下一手再也做不到 3 迫着 = 我们不会被秒。
static int pairs_unhit(const std::vector<ThrPair>& ps, int a, int b) {
    int u = 0;
    for (const auto& p : ps)
        if (p.a != a && p.a != b && p.b != a && p.b != b) ++u;
    return u;
}

// ============================================================================
//  「做局」：杀点对（kill pair）与覆盖数（cover number）
//
//  定义：**杀点对** = 一对空点 (c,d)，把这两颗子落上去之后 tc >= 3。
//        tc>=3 的唯一含义是「对手一手 2 子压不住全部威胁」→ 下回合直接成 6。
//        所以每一个杀点对，就是一条**已经办妥、只差把两子放上去**的必杀线。
//
//  对手要活下去，必须把全部杀点对逐个打掉（占掉 c 或 d）。一手只有 2 子，
//  于是「覆盖数」= 最少几颗子能打掉全部杀点对（= 杀点对无向图的**最小点覆盖**）：
//        0 → 还没有杀点对（局面还很生）
//        1 → 对手一子就化解，等于白送人家一手
//        2 → 对手必须**两子全用来化解**，整回合被钉死 → 我们白赚一手（绞杀）
//        3 → 对手挡不住 → 下一手必成 6（真杀）
//
//  这就是「做局」唯一可靠的量化语言：不看我有几条活三，只看**对手还剩几条活路**。
//  一堆活三可以一子全废（同一条线），而两条互不相交的杀点对，对手就得掏两颗子。
//
//  ★ 优先级纪律（绝不能违反）：做局只是「双方都没有立即杀招」时的选址依据。
//    成 6 / 4+2 / 三迫着 / VCF / VCDT 这些**真杀**永远先走，见 decide() 的 1)~2)。
//    做局的分数（最高 250 万）被刻意压在「三迫着」(10 亿) 和「搜索证胜」之下。
// ============================================================================
struct KillPair { int c, d; };

// 杀点对的候选点：落在 own>=2 且对手 0 子的窗口里的空点。
// 为什么门槛是 2：一条窗口补两子就从 2 变 4（成四）；补一子则需要 own>=3。
static void kill_graph_points(const Board& B, int me, std::vector<int>& pts, int capPts) {
    static int wgt[CELLS];
    std::memset(wgt, 0, sizeof(int) * CELLS);
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int own = (me == 1) ? B.w_own[w] : B.w_opp[w];
        if (own < 2) continue;
        int oth = (me == 1) ? B.w_opp[w] : B.w_own[w];
        if (oth) continue;
        for (int t = 0; t < 6; ++t) {
            int idx = g_windows[w].c[t];
            if (!B.cell[idx]) ++wgt[idx];
        }
    }
    pts.clear();
    for (int i = 0; i < CELLS; ++i) if (wgt[i] > 0 && !B.cell[i]) pts.push_back(i);
    // 必须按权重排序后再截断：按格子索引截断会让棋盘下半区整片「瞎掉」
    // （这个坑在 double_threat_points 里已经踩过一次）。
    std::sort(pts.begin(), pts.end(), [&](int x, int y) {
        if (wgt[x] != wgt[y]) return wgt[x] > wgt[y];
        return x < y;
    });
    if ((int)pts.size() > capPts) pts.resize(capPts);
}

static int collect_kill_pairs(Board& B, int me, std::vector<KillPair>& K,
                              const std::vector<int>& pts, int maxOut, int stopAt) {
    K.clear();
    for (size_t i = 0; i < pts.size(); ++i) {
        int a = pts[i];
        if (a < 0 || B.cell[a]) continue;
        B.place_raw(a, me);
        for (size_t j = i + 1; j < pts.size(); ++j) {
            int b = pts[j];
            if (b < 0 || B.cell[b]) continue;
            B.place_raw(b, me);
            int tc = threat_count(B, me);
            B.remove_raw(b);
            if (tc >= 3) {
                K.push_back({a, b});
                if ((stopAt > 0 && (int)K.size() >= stopAt) ||
                    ((int)K.size() >= maxOut)) { B.remove_raw(a); return (int)K.size(); }
            }
        }
        B.remove_raw(a);
        if (past_deadline()) break;
    }
    return (int)K.size();
}

// 最小点覆盖的规模（截断到 3）。covers2 非空时顺便输出**全部** 2 点解，
// 用来判断「唯一解」——唯一解才有做局价值：对手必须精确落在那一对上。
static int kill_cover_number(const std::vector<KillPair>& K,
                             std::vector<std::pair<int,int>>* covers2,
                             int* cover1) {
    if (cover1) *cover1 = -1;
    if (covers2) covers2->clear();
    if (K.empty()) return 0;
    std::vector<int> vs;
    vs.reserve(K.size() * 2);
    for (const auto& e : K) { vs.push_back(e.c); vs.push_back(e.d); }
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
    // ---- 1 点解 ----
    for (int p : vs) {
        bool all = true;
        for (const auto& e : K) if (e.c != p && e.d != p) { all = false; break; }
        if (all) { if (cover1) *cover1 = p; return 1; }
    }
    // ---- 2 点解：固定 p 之后，剩下没被打掉的杀点对必须共享同一个端点 ----
    int found = 0;
    for (int p : vs) {
        int q1 = -1, q2 = -1; bool init = false, dead = false;
        for (const auto& e : K) {
            if (e.c == p || e.d == p) continue;
            if (!init) { q1 = e.c; q2 = e.d; init = true; continue; }
            int n1 = -1, n2 = -1, n = 0;
            if (q1 >= 0 && (q1 == e.c || q1 == e.d)) { n1 = q1; ++n; }
            if (q2 >= 0 && (q2 == e.c || q2 == e.d)) { if (n == 0) n1 = q2; else n2 = q2; ++n; }
            q1 = n1; q2 = n2;
            if (q1 < 0 && q2 < 0) { dead = true; break; }
        }
        if (dead || !init) continue;
        if (q1 >= 0) {
            if (!covers2) return 2;
            covers2->push_back({p, q1}); ++found;
        }
        if (q2 >= 0) {
            if (!covers2) return 2;
            covers2->push_back({p, q2}); ++found;
        }
    }
    if (found > 0) {
        if (covers2) {
            std::sort(covers2->begin(), covers2->end());
            covers2->erase(std::unique(covers2->begin(), covers2->end()), covers2->end());
        }
        return 2;
    }
    return 3;
}

struct TrapInfo { int kp = 0, cover = 0, n2 = 0, uniq = 0, tcMe = 0, rate = -1; };

// 在「我方已经落下 a,b」的局面上评估做局强度
static TrapInfo trap_eval(Board& B, int me) {
    TrapInfo T;
    std::vector<int> pts;
    kill_graph_points(B, me, pts, 36);
    if ((int)pts.size() < 2) return T;
    std::vector<KillPair> K;
    collect_kill_pairs(B, me, K, pts, 48, 0);
    T.kp = (int)K.size();
    if (T.kp == 0) return T;
    std::vector<std::pair<int,int>> c2;
    T.cover = kill_cover_number(K, &c2, nullptr);
    T.n2 = (int)c2.size();
    T.uniq = (T.cover == 2 && (int)c2.size() == 1) ? 1 : 0;
    T.tcMe = threat_count(B, me);
    return T;
}

// 「这一手做局之后，对手有多少比例的应手挡不住我们」×1000（0~1000）
//
// 为什么必须有这一条：光看覆盖数（cover）是不够的 —— 覆盖数只说明「最少几颗子
// 能拆掉全部杀点对」，但没考虑**对手的落子会隔空废掉我方的四连窗口**。
// 杀点对 (c,d) 能成立，靠的是「我方已有 4 子 + 2 空点」组成窗口；对手在自己那一手
// 落下的子，可能正好落进那个窗口，把整条威胁废掉。所以覆盖数 >= 3 只是必要条件。
//
// 实测（执黑 seed1200 第 44 手，盘上 43 子）：cover=3 的做局手，
//   按真判据穷举 49770 种对手应手，只有 629 种能拆（okRate 98.7%）——
//   「很强」但**不是必胜**。以前只查「格子空不空」会把它误判成 locked=true，
//   那就是标准的假必胜。
//
// 这里用有上限的抽样（maxReplies）把它变成可负担的选址指标。
static int trap_ok_rate(Board& B, int me, int a, int b, long long maxReplies) {
    if (a < 0 || b < 0 || a == b || B.cell[a] || B.cell[b]) return -1;
    B.place_raw(a, me);
    B.place_raw(b, me);
    std::vector<int> pts;
    kill_graph_points(B, me, pts, 28);
    std::vector<KillPair> K;
    collect_kill_pairs(B, me, K, pts, 64, 0);
    if (K.empty()) { B.remove_raw(b); B.remove_raw(a); return -1; }
    long long steps = 0, replies = 0, ok = 0;
    bool stop = false;
    // 等距抽样：不能直接取「前 maxReplies 个」—— 那一片全在棋盘左上角，
    // 逃脱率会被局部区域的行棋习惯带偏（同一个坑：按索引截断让下半区瞎掉）。
    int nfree = B.n_empty;
    long long total = (long long)nfree * (nfree - 1) / 2;
    long long stride = total / (maxReplies > 0 ? maxReplies : 1);
    if (stride < 1) stride = 1;
    long long seq = 0;
    for (int x = 0; x < CELLS && !stop; ++x) {
        if (B.cell[x]) continue;
        for (int y = x + 1; y < CELLS; ++y) {
            if (B.cell[y]) continue;
            if ((seq++ % stride) != 0) continue;
            if (++steps > maxReplies) { stop = true; break; }
            ++replies;
            B.place_raw(x, 3 - me);
            B.place_raw(y, 3 - me);
            // ★ 对手这一手的三种「逃掉 / 反杀」方式，一个都不能漏：
            //   ① 做出 4+2/5+1（hot）→ 他下一手就成 6，比我们快一步；
            //   ② 自己做出 3 迫着（tc>=3）→ 我们下回合挡不住，他比我们先成 6；
            //   ③ 他的落子隔空废掉我方四连窗口（下面的 tc>=3 重算会体现）。
            //   只查「我方杀点对没被占」是典型的假必胜。
            bool alive = (B.hot[3 - me] == 0) && (threat_count(B, 3 - me) < 3);
            if (alive) for (const auto& e : K) {
                if (e.c == x || e.d == x || e.c == y || e.d == y) continue;
                B.place_raw(e.c, me);
                B.place_raw(e.d, me);
                if (threat_count(B, me) >= 3) alive = true;
                B.remove_raw(e.d);
                B.remove_raw(e.c);
                if (alive) break;
            }
            B.remove_raw(y);
            B.remove_raw(x);
            if (alive) ++ok;
        }
    }
    B.remove_raw(b);
    B.remove_raw(a);
    if (!replies) return -1;
    return (int)(ok * 1000 / replies);
}

// 找「做局手」：枚举我方两子的组合，按覆盖数初筛，再对最优的几个做真实逃脱率穷举。
// 只花小预算（<0.6s），主预算必须留给杀棋。
static bool trap_best(Board& B, int me, int k, Move& out, TrapInfo& best,
                      long long budget_ms) {
    best = TrapInfo();
    if (k != 2) return false;
    long long dl = steady_ms() + budget_ms;
    std::vector<int> pts;
    kill_graph_points(B, me, pts, 13);
    int n = (int)pts.size();
    if (n < 2) return false;
    struct TC { long long s; int a, b; TrapInfo T; };
    std::vector<TC> cands;
    cands.reserve(128);
    for (int i = 0; i < n; ++i) {
        if (B.cell[pts[i]]) continue;
        for (int j = i + 1; j < n; ++j) {
            if (B.cell[pts[j]]) continue;
            if (steady_ms() > dl) break;
            B.place_raw(pts[i], me);
            B.place_raw(pts[j], me);
            bool gift = (B.hot[3 - me] > 0);      // 这一手等于送礼，直接废掉
            TrapInfo T;
            if (!gift) T = trap_eval(B, me);
            B.remove_raw(pts[j]);
            B.remove_raw(pts[i]);
            if (gift) continue;
            long long s = (long long)T.cover * 1000000LL
                        + (long long)T.uniq  * 300000LL
                        + (long long)T.kp    * 2000LL
                        + (long long)T.tcMe  * 30000LL;
            cands.push_back({s, pts[i], pts[j], T});
        }
    }
    if (cands.empty()) return false;
    std::sort(cands.begin(), cands.end(), [](const TC& x, const TC& y) { return x.s > y.s; });

    // 前几名做真实逃脱率穷举（每名最多 6000 种应手，约 20ms）
    int top = (int)cands.size() < 3 ? (int)cands.size() : 3;
    long long bestKey = -1;
    int pickIdx = 0;
    for (int t = 0; t < top; ++t) {
        if (steady_ms() > dl && t > 0) break;
        int r = trap_ok_rate(B, me, cands[t].a, cands[t].b, 6000);
        cands[t].T.rate = r;
        // 先比逃脱率（越高越好），相同时比覆盖数
        long long key = (long long)(r < 0 ? 0 : r) * 100LL
                      + (long long)cands[t].T.cover * 1000000LL / 1000LL
                      + cands[t].s / 1000000LL;
        if (key > bestKey) { bestKey = key; pickIdx = t; }
        if (r == 1000) break;                  // 100% 锁死，已经最好
    }
    out = Move{cands[pickIdx].a, cands[pickIdx].b};
    best = cands[pickIdx].T;
    return true;
}

static bool apply_move(Board& B, const Move& m, int color, int k) {
    B.place_raw(m.a, color);
    if (just_won(B, m.a, color)) return true;
    if (k == 2) { B.place_raw(m.b, color); if (just_won(B, m.b, color)) return true; }
    return false;
}
static void undo_move(Board& B, const Move& m, int color, int k) {
    if (k == 2) B.remove_raw(m.b);
    B.remove_raw(m.a);
}

// ----------------------------------------------------------------------------
//  置换表
// ----------------------------------------------------------------------------
struct TTEntry { uint64_t key; int depth; int flag; int value; uint32_t best; };
static const int TT_BITS = 21;
static const int TT_SIZE = 1 << TT_BITS;
static TTEntry g_tt[TT_SIZE];

// 历史走法排序（单点）
#if USE_HISTORY
static int g_hist[CELLS];
#endif

// ----------------------------------------------------------------------------
//  搜索：Negamax + PVS（WIN-ply 偏好最快胜 / 最慢负）
// ----------------------------------------------------------------------------
static int search(Board& B, int color, int k, int depth, int alpha, int beta, int ply) {
    if (g_timed_out) return 0;
    if (B.n_empty <= 0) return 0;
    // ★ 叶子级战术终结（quiescence）：轮到走的一方若已握有 hot 窗口
    //   （6 窗内自己 >=4 子、对手 0 子），本回合两子就能连成 6 —— 直接判胜。
    //   原来这个判据只在根节点做，搜索内部完全不知道「谁已经有四了」：
    //   中局一旦让对手把四做出来，搜索只会按窗口分判断「形势还不差」，
    //   于是继续做自己的闲棋，几步之后被对手 4+2 直接连成 6 —— 这正是
    //   实战里「三十一手就被秒」的那条路径。放在每层入口做，代价 O(1)。
    if (B.hot[color] > 0) return WIN - ply;
    if (depth <= 0) {
        // ★ 战术延伸（threat quiescence）：盘面上只要还存在「一手成 6」的窗口
        //   （任何一方握着 hot），就不能静态评估 —— 必须再往下看一层。
        //   理由：静态分完全无法表达「对手下一手直接连成 6」。搜索一旦停在
        //   这种局面上，就会给出一句「形势还不错」的谎言，然后照着自己的
        //   计划走棋，而对手已经在终点线上了。这是所有六子棋引擎最典型
        //   的地平线效应，也是「对手一个双四、几步把我秒了」的最后一环。
        if ((B.hot[1] > 0 || B.hot[2] > 0) && ply < 32) depth = 1;
        else {
            int e = evaluate(B, g_my_color);
            return (color == g_my_color) ? e : -e;
        }
    }
    uint64_t key = B.hash ^ ((uint64_t)color << 1);
    int ti = (int)(key & (TT_SIZE - 1));
    TTEntry e0 = g_tt[ti];
    bool hit = (e0.key == key);
    int alphaOrig = alpha;
    if (hit && e0.depth >= depth) {
        if (e0.flag == 0) return e0.value;
        else if (e0.flag == 1 && e0.value > alpha) alpha = e0.value;
        else if (e0.flag == 2 && e0.value < beta)  beta  = e0.value;
        if (alpha >= beta) return e0.value;
    }
    std::vector<Move> moves; gen_moves(B, color, k, moves);
    if (moves.empty()) return 0;
    // 内部节点窄宽度（gen_moves 已把强制点/必防点排在最前，截断不丢关键着法）。
    // ★ 分层收窄（根 56 / 中 30 / 深 16）实测**有害**：执白从 27 手退回 91 手。
    //   gen_moves 只保证「必防点」在队首，第 17 位之后的常规着法里藏着关键的
    //   攻防转换手；深层一砍，搜索看到的是一条残疾的棋路，评估再准也只能磨棋。
    //   宽度保持常数，深度靠时间预算去换。
    if ((int)moves.size() > NARROW_CAP) moves.resize(NARROW_CAP);
    if (hit && e0.best) {
        int ba = (int)(e0.best >> 16);
        int bb = (int)(e0.best & 0xFFFF); if (bb == 0xFFFF) bb = -1;
        for (size_t it = 0; it < moves.size(); ++it)
            if (moves[it].a == ba && moves[it].b == bb) { std::swap(moves[0], moves[it]); break; }
    }
#if USE_HISTORY
    // 历史得分排序（强制点已在前面）：让 PVS 更聚焦
    std::sort(moves.begin() + 1, moves.end(), [](const Move& x, const Move& y) {
        int hx = g_hist[x.a] + g_hist[(x.b < 0 ? 0 : x.b)];
        int hy = g_hist[y.a] + g_hist[(y.b < 0 ? 0 : y.b)];
        return hx > hy;
    });
#endif
    int best = -INF, bestEnc = 0;
    for (size_t i = 0; i < moves.size(); ++i) {
        if (g_timed_out) return 0;
        if ((++g_nodes & 1023) == 0) { check_time(); if (g_timed_out) return 0; }
        bool first = (i == 0);
        bool won = apply_move(B, moves[i], color, k);
        int val;
        if (won) val = WIN - ply;
        else {
            if (first) val = -search(B, 3 - color, 2, depth - 1, -beta, -alpha, ply + 1);
            else {
                val = -search(B, 3 - color, 2, depth - 1, -alpha - 1, -alpha, ply + 1);
                if (val > alpha && val < beta)
                    val = -search(B, 3 - color, 2, depth - 1, -beta, -alpha, ply + 1);
            }
        }
        undo_move(B, moves[i], color, k);
        if (g_timed_out) return 0;
        if (val > best) {
            best = val; bestEnc = ((uint32_t)moves[i].a << 16) | (uint32_t)(moves[i].b < 0 ? 0xFFFF : moves[i].b);
#if USE_HISTORY
            if (val > alpha) { g_hist[moves[i].a] += depth * depth; if (moves[i].b >= 0) g_hist[moves[i].b] += depth * depth; }
#endif
        }
        if (val > alpha) alpha = val;
        if (alpha >= beta) break;
    }
    int flag = (best <= alphaOrig) ? 2 : (best >= beta) ? 1 : 0;
    g_tt[ti] = {key, depth, flag, best, (uint32_t)bestEnc};
    return best;
}

// 受限根候选：非空时 root_search 只在这个集合里搜索（用于「必防解内搜索」）
static const std::vector<Move>* g_root_restrict = nullptr;

static int root_search(Board& B, int color, int k, int& bestIdx) {
    bestIdx = -1;
    std::vector<Move> moves;
    if (g_root_restrict && !g_root_restrict->empty()) moves = *g_root_restrict;
    else gen_moves(B, color, k, moves);
    if (moves.empty()) return 0;
    int alpha = -INF, beta = INF, best = -INF, bestI = -1;
    int lastDepth = 0;
    std::vector<int> prevVal(moves.size(), -INF);
    for (int depth = 1; depth <= MAXDEPTH; ++depth) {
        // ★ 迭代加深必须「按上一轮的分数重排根着法」。
        //   不重排的话，每一层都从固定顺序开始搜，alpha 阈值几乎不起作用，
        //   同样的节点数只能多看一层 —— 实测重排后同样 4 秒能多跑 2~3 层。
        if (depth > 1) {
            std::vector<int> idx(moves.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
            std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) { return prevVal[x] > prevVal[y]; });
            std::vector<Move> reordered(moves.size());
            for (size_t i = 0; i < idx.size(); ++i) reordered[i] = moves[idx[i]];
            moves.swap(reordered);
            std::vector<int> pv2 = prevVal;
            for (size_t i = 0; i < idx.size(); ++i) prevVal[i] = pv2[idx[i]];
        }
        bool completed = true; int localBest = -INF, localI = -1;
        for (size_t i = 0; i < moves.size(); ++i) {
            if (g_timed_out) { completed = false; break; }
            if ((++g_nodes & 1023) == 0) { check_time(); if (g_timed_out) { completed = false; break; } }
            bool won = apply_move(B, moves[i], color, k);
            int val;
            if (won) val = WIN;
            else {
                if (i == 0) val = -search(B, 3 - color, 2, depth - 1, -beta, -alpha, 1);
                else {
                    val = -search(B, 3 - color, 2, depth - 1, -alpha - 1, -alpha, 1);
                    if (val > alpha && val < beta)
                        val = -search(B, 3 - color, 2, depth - 1, -beta, -alpha, 1);
                }
            }
            undo_move(B, moves[i], color, k);
            if (g_timed_out) { completed = false; break; }
            prevVal[i] = val;
            if (val > localBest) { localBest = val; localI = (int)i; }
            if (val > alpha) alpha = val;
            if (alpha >= beta) break;
        }
        if (!completed) break;
        best = localBest; bestI = localI; lastDepth = depth;
        if (best >= WIN - 1) break;
    }
    bestIdx = bestI;
    if (g_trace) std::fprintf(stderr, "  [search] depth=%d nodes=%lld val=%d\n", lastDepth, g_nodes, best);
    return best;
}

// ----------------------------------------------------------------------------
//  CTSS（保守威胁搜索 / 强制胜证明器）
// ----------------------------------------------------------------------------
#if USE_CTSS
static long long g_ctss_nodes = 0;
static bool     g_ctss_stop  = false;
static int      g_ctss_outA  = -1;
static int      g_ctss_outB  = -1;
static const long long CTSS_NODE_CAP = 200000LL;
// 威胁点上限：超过就不敢判胜（保守方向，只漏杀不假杀）。
// 实测双四局面（一条线上 4 子连续）的威胁窗口空点并集约 6~12 个，
// 20 在深层局面（双方都有四三）经常不够 → 明明能杀却整层放弃。
static const int           CTSS_DEF_CAP  = 26;
static const int           CTSS_DEPTH_TOP= 8;    // 连续双迫着的层数（6 层只够 3 个回合）
// VCDT 开关：开启后攻击方除「连续四(VCF)」外，还可用「双迫着（对手必须两子全挡）」
// 参与强攻搜索 —— 这是六子棋真正的取胜主线（连续四只是它的一个特例）。
static bool g_ctss_allow_dt = false;

static void four_empties(const Board& B, int color, std::vector<int>& out) {
    out.clear();
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int o = (color == 1) ? B.w_own[w] : B.w_opp[w];
        int p = (color == 1) ? B.w_opp[w] : B.w_own[w];
        if (o == 4 && p == 0)
            for (int k = 0; k < 6; ++k) { int idx = g_windows[w].c[k]; if (!B.cell[idx]) out.push_back(idx); }
    }
    std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
}
static inline bool ctss_timeup() {
    if (g_ctss_deadline_ms && steady_ms() >= g_ctss_deadline_ms) { g_ctss_stop = true; g_timed_out = true; return true; }
    return false;
}
static bool ctss_attacker(Board& B, int color, int depth);
// ----------------------------------------------------------------------------
//  防守方（3-color）的**精确**应手枚举：最小击中集
//
//  攻击方落完两子后留下若干「威胁窗口」（own>=4、opp==0、空点<=2）——
//  防守方必须把每一个窗口至少命中一个空点，否则攻击方下一手直接成 6。
//  一手 2 子，所以只需枚举 1 点解与 2 点解，这就是**全部**合法应手里的
//  「能活下来的那些」；不在解里的应手等价于当场认输（下一手被连 6）。
//
//  旧实现把所有「胜点 + 四空点 + 送对手一手成 6 的点」堆成 blocks，再对所有
//  两两组合逐一递归 —— 组合数最多 231 个，且大量是根本挡不住威胁的废组合，
//  既慢又浪费节点预算（真正要证明的只有「每一种有效防守都输」）。
// ----------------------------------------------------------------------------
static inline bool cov2(const std::vector<std::vector<int>>& thr, int a, int b) {
    for (const auto& t : thr) {
        bool ok = false;
        for (int e : t) if (e == a || e == b) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}
// ----------------------------------------------------------------------------
//  防守方的「自由子」候选 —— 相关区（relevance zone）
//
//  ★ 这是本轮补上的**假必胜根源**，也是论文里 AND-OR 证明搜索的硬要求
//    （`EvaluateAllDefensiveMoves` 必须完整）。原实现只枚举「命中全部威胁窗口」
//    的 1 点解 / 2 点解：
//       · 2 点解：两颗子都用掉，应手是完整的；
//       · **1 点解：只用掉一颗子，另一颗子凭空消失了**。
//    而那颗自由子完全可以落在我方后续威胁的补点上，把整条强制线掐死。
//    忽略它不会漏杀，但会**假报必胜** —— 方向正好错在最危险的一侧：
//    引擎会按「我已经必胜」去走一条其实不成立的线，然后被反杀。
//
//  成本控制：不在整个棋盘上枚举（19×19 里两两组合 6.5 万，不可能），
//  只取「我方 own>=2 且对手 0 子」窗口的空点 —— 这正是我方后续威胁只能长出来的
//  区域（relevance zone），按落在几个窗口里排序，截断到 10 个。
// ----------------------------------------------------------------------------
static void spare_points(const Board& B, int attacker, std::vector<int>& out, int cap) {
    out.clear();
    static int w[CELLS];
    std::memset(w, 0, sizeof(int) * CELLS);
    for (size_t i = 0; i < g_windows.size(); ++i) {
        int o = (attacker == 1) ? B.w_own[i] : B.w_opp[i];
        if (o < 2) continue;
        int p = (attacker == 1) ? B.w_opp[i] : B.w_own[i];
        if (p) continue;
        for (int t = 0; t < 6; ++t) {
            int idx = g_windows[i].c[t];
            if (!B.cell[idx]) ++w[idx];
        }
    }
    for (int i = 0; i < CELLS; ++i) if (w[i] > 0 && !B.cell[i]) out.push_back(i);
    std::sort(out.begin(), out.end(), [&](int x, int y) { return w[x] > w[y]; });
    if ((int)out.size() > cap) out.resize(cap);
}

static bool ctss_defender(Board& B, int color, int depth) {
    if (ctss_timeup()) return false;
    // 防守方自己已经握有「一手成 6」→ 攻击方的强制线当然不成立
    if (opp_has_immwin(B, color)) return true;
    std::vector<std::vector<int>> thr;
    threat_windows_of(B, color, thr);
    if (thr.empty()) return true;              // 攻不出威胁 = 守住了
    std::vector<int> emp;
    for (const auto& t : thr) for (int e : t) emp.push_back(e);
    std::sort(emp.begin(), emp.end());
    emp.erase(std::unique(emp.begin(), emp.end()), emp.end());
    // 威胁点太多 → 枚举不完，不敢判「必胜」（方向安全：只是漏报）
    if ((int)emp.size() > CTSS_DEF_CAP) return true;
    std::vector<std::pair<int, int>> reps;
    for (int a : emp) if (cov2(thr, a, -1)) reps.push_back({a, -1});
    for (size_t i = 0; i < emp.size(); ++i)
        for (size_t j = i + 1; j < emp.size(); ++j)
            if (cov2(thr, emp[i], emp[j])) reps.push_back({emp[i], emp[j]});
    if (reps.empty()) return false;            // 一个挡法都没有 → 攻击方下一手成 6
    // 有效防守太多 → 不判胜（保守）。旧值 48：双四局面的有效两点解常常 50~80 组
    // （每条威胁线的两个空点自由组合），48 会在「明明所有防守都输」时整层放弃，
    // 这正是「有双四却磨到 70 手」的一个直接来源。
    // 提高上限的代价由 deadline + CTSS_NODE_CAP 兜住：只要第一种防守能撑住就
    // 立刻 return true，枚举全部只发生在真正必败的节点上 —— 而那些节点正是
    // 我们要找的胜线，值得花时间。
    if ((int)reps.size() > 96) return true;
    // 单点解（r.second < 0）天然排在前：用 1 子就挡住，比 2 子挡更省，
    // 也几乎总是「最有可能撑住」的那一种，先试它能让多数节点秒返回。
    int spareTried = 0;                        // 本节点最多给几个单点解补自由子
    for (const auto& r : reps) {
        B.place_raw(r.first, 3 - color);
        if (r.second >= 0) B.place_raw(r.second, 3 - color);
        bool defWin = just_won(B, r.first, 3 - color) ||
                      (r.second >= 0 && just_won(B, r.second, 3 - color));
        bool child = false;
        if (!defWin) child = ctss_attacker(B, color, depth - 1);
        bool survive = defWin || !child;
        // ★ 单点解 → 防守方还剩一颗自由子，必须把它算进来（见 spare_points 注释）
        if (!survive && r.second < 0 && depth >= 2 && spareTried < 6 && !g_ctss_stop) {
            ++spareTried;
            std::vector<int> sp;
            spare_points(B, color, sp, 10);
            for (int s : sp) {
                if (B.cell[s]) continue;
                B.place_raw(s, 3 - color);
                bool dw = just_won(B, s, 3 - color);
                bool ch = false;
                if (!dw) ch = ctss_attacker(B, color, depth - 1);
                B.remove_raw(s);
                if (g_ctss_stop) break;
                if (dw || !ch) { survive = true; break; }   // 自由子救活了防守
            }
        }
        if (r.second >= 0) B.remove_raw(r.second);
        B.remove_raw(r.first);
        if (g_ctss_stop) return false;
        if (survive) return true;              // 存在一种能撑住的防守
    }
    return false;                              // 所有防守都输 → 攻击方必胜
}
static bool ctss_attacker(Board& B, int color, int depth) {
    if (ctss_timeup()) return false;
    if ((++g_ctss_nodes & 4095) == 0) {
        if (g_ctss_nodes > CTSS_NODE_CAP) { g_ctss_stop = true; return false; }
        if (ctss_timeup()) return false;
    }
    if (depth <= 0) return false;
    std::vector<int> wp; win_points(B, color, wp);
    if (!wp.empty()) { g_ctss_outA = wp[0]; g_ctss_outB = -1; return true; }
    // 我方 4+2：一回合两子直接成 6（与 5+1 同为立即胜，必须先判）
    if (B.hot[color] > 0) {
        for (size_t w = 0; w < g_windows.size(); ++w) {
            int own = (color == 1) ? B.w_own[w] : B.w_opp[w];
            int opn = (color == 1) ? B.w_opp[w] : B.w_own[w];
            if (own == 4 && opn == 0) {
                std::vector<int> e;
                for (int t = 0; t < 6; ++t) { int idx = g_windows[w].c[t]; if (!B.cell[idx]) e.push_back(idx); }
                if ((int)e.size() == 2) { g_ctss_outA = e[0]; g_ctss_outB = e[1]; return true; }
            }
        }
    }
    std::vector<int> cand; collect_candidates(B, color, 2, cand);
    struct FC { int a; bool makes5; };
    std::vector<FC> fclist;
    for (int a : cand) {
        B.place_raw(a, color);
        if (opp_has_immwin(B, color)) { B.remove_raw(a); continue; }
        std::vector<int> mywp; win_points(B, color, mywp);
        bool makes5 = !mywp.empty();
        bool ok = makes5;
        if (!ok) { std::vector<int> fe; four_empties(B, color, fe); ok = !fe.empty(); }
        B.remove_raw(a);
        if (ok) fclist.push_back({a, makes5});
    }
    // 优先尝试「落子即形成五连（制造立即胜点）」的着法：这是最强威胁，
    // 其获胜伙伴常可由 just_won 直接判定，无需昂贵的防御搜索，从而避免在
    // 较弱候选上耗尽节点/时间预算而漏判真正的必杀。
    std::sort(fclist.begin(), fclist.end(),
              [](const FC& x, const FC& y) { return x.makes5 > y.makes5; });
    std::vector<int> forcing; forcing.reserve(fclist.size());
    for (auto& f : fclist) forcing.push_back(f.a);
    for (int a : forcing) {
        if (ctss_timeup()) return false;
        B.place_raw(a, color);
        if (just_won(B, a, color)) { g_ctss_outA = a; g_ctss_outB = -1; B.remove_raw(a); return true; }
        std::vector<int> cand2; collect_candidates(B, color, 2, cand2);
        for (int b : cand2) {
            if (b == a) continue;
            B.place_raw(b, color);
            if (just_won(B, b, color)) { g_ctss_outA = a; g_ctss_outB = b; B.remove_raw(b); B.remove_raw(a); return true; }
            if (opp_has_immwin(B, color)) { B.remove_raw(b); continue; }
            // ★★ 只有「双迫着（tc>=2）」才算强制着 ★★
            //   一手 2 子，对手也必须用 2 子来挡 —— 这时他的应手是**精确**的
            //   最小击中集，枚举完整，判定可靠。
            //   一旦 tc<=1（单迫着），对手还剩一颗**自由子**；自由子落在 19×19
            //   任意一格都可能隔空废掉我方后续的四连窗口，落点无法穷举
            //   （relevance zone 截断、单点解只试前 6 个），判据必然不精确 →
            //   会给出**假必胜**。
            //   实测（平台等效参数，我方 5s、基准 2.5s）：执黑 4/8 负，全部是
            //   `vcdt` 在某手宣称必胜，几步之后同一个局面又找不到胜、搜索直接
            //   给出 -9999997（已输）。棋谱也支持这一点 —— 六子棋的强制取胜线
            //   本来就必须靠**连续双迫着**（NCTU6 的 VCST 就是这个结构），
            //   单迫着链条在对手手里多一颗子的情况下不成立。
            if (threat_count(B, color) < 2) { B.remove_raw(b); continue; }
            if (ctss_defender(B, color, depth)) { g_ctss_outA = a; g_ctss_outB = b; B.remove_raw(b); B.remove_raw(a); return true; }
            B.remove_raw(b);
        }
        B.remove_raw(a);
    }
    // ---- VCDT 层：攻击着 = 两子做出「恰好 2 个迫着」的组合 ----
    // 为什么必须是 2：对手一手 2 子，被迫全部用来封堵（一子覆盖一个威胁），
    // 因此他的应手是**完全强制**的（精确最小击中集），搜索才是可靠的；
    // 做到 3 个迫着则对手挡不住 —— 那是直接胜。
    if (g_ctss_allow_dt) {
        std::vector<ThrPair> dt;
        collect_threat_pairs(B, color, 2, 40, dt, 64);
        for (const auto& p : dt) {
            if (ctss_timeup()) return false;
            if (B.cell[p.a] || B.cell[p.b]) continue;
            B.place_raw(p.a, color);
            bool won = just_won(B, p.a, color);
            B.place_raw(p.b, color);
            if (!won) won = just_won(B, p.b, color);
            // ★ 关键：我方「3 个迫着」是**下一手**才兑现，所以必须先确认对手
            //   自己**没用**一手成 6 的形（4+2 / 5+1）。否则对手下一步直接成 6，
            //   我们的三个迫着一个都来不及用 —— 这一条漏掉就是「假必胜」，
            //   引擎会一路按「我要赢了」乱走，几步之内被反杀。
            bool oppWin = opp_has_immwin(B, color);
            bool ok = false;
            if (won) ok = true;
            else if (!oppWin) {
                int tc = threat_count(B, color);
                if (tc >= 3) ok = true;                       // 三迫着 → 对手一手挡不住
                else if (tc == 2) ok = ctss_defender(B, color, depth);
            }
            B.remove_raw(p.b); B.remove_raw(p.a);
            if (g_ctss_stop) return false;
            if (ok) { g_ctss_outA = p.a; g_ctss_outB = p.b; return true; }
        }
    }
    return false;
}
static bool ctss_find(Board& B, int color, int& outA, int& outB, int budget_ms,
                      bool allow_dt = false) {
    g_ctss_nodes = 0; g_ctss_stop = false; g_ctss_outA = -1; g_ctss_outB = -1;
    g_timed_out = false;
    g_ctss_allow_dt = allow_dt;
    g_ctss_deadline_ms = steady_ms() + budget_ms;
    bool r = ctss_attacker(B, color, CTSS_DEPTH_TOP);
    g_ctss_deadline_ms = 0;
    outA = g_ctss_outA; outB = g_ctss_outB;
    return r;
}
#endif

// ----------------------------------------------------------------------------
//  迫着层终审打分：给「搜索选出来的着法」和「战术候选」一个同一把尺子。
//    · 我方做成 3 迫着  -> 对手一手挡不住 -> 必胜
//    · 我方做成 2 迫着  -> 对手必须两子全挡（强制、抢到先手）
//    · 我方只做成 1 迫着（一个四）-> 只是普通强制
//    · 对手留下 3 迫着    -> 我们下一手挡不住 -> 必败
//    · 抢掉对手一个「双迫着制造点」-> 直接掐死它的双四/三迫着路线
// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------
//  方向覆盖度：我方在几个**不同方向**上存在「own>=3 且对手 0 子」的窗口。
//
//  为什么这条指标决定胜负：六子棋的「三迫着（tc>=3）」在几何上要求 3 条
//  **互不相交**的战线，而线只可能来自 4 个方向（横/竖/正斜/反斜）。
//  一个方向的子堆得再厚（棋谱里常见：一条横线上摆 4~5 颗），对手两颗子
//  （一条线只需 1 颗子占住就废）就能整条掐死 —— 所以「堆单线」永远做不出
//  第 2 个迫着，更做不出第 3 个。
//
//  实测执黑对基准跑了 89 个回合：tc=2 只出现 7 次、tc=3 一次都没有，
//  而对手 83 个回合 tc=0（纯防守）。根因就是引擎一直在同一条线/同一片
//  区域加深，从没把棋子摊到第二个方向去。这条指标把它显式地拉回来。
// ----------------------------------------------------------------------------
static inline int dir_of_window(const Win& w) {
    int r0 = w.c[0] / N, c0 = w.c[0] % N;
    int r1 = w.c[1] / N, c1 = w.c[1] % N;
    if (r0 == r1) return 0;          // 横
    if (c0 == c1) return 1;          // 竖
    return (c1 > c0) ? 2 : 3;        // 正斜 / 反斜
}
// 返回值 = 有「活三及以上」的方向数 * 10 + 有「活二及以上」的方向数
static int line_dirs(const Board& B, int side) {
    int m3 = 0, m2 = 0;
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int o = (side == 1) ? B.w_own[w] : B.w_opp[w];
        if (o < 2) continue;
        int p = (side == 1) ? B.w_opp[w] : B.w_own[w];
        if (p) continue;
        int d = dir_of_window(g_windows[w]);
        if (o >= 3) m3 |= (1 << d);
        m2 |= (1 << d);
    }
    int n3 = 0, n2 = 0;
    for (int i = 0; i < 4; ++i) { if (m3 & (1 << i)) ++n3; if (m2 & (1 << i)) ++n2; }
    return n3 * 10 + n2;
}

// ----------------------------------------------------------------------------
//  「活三」与「双三」（fork three）—— 棋理里最核心的进攻构件
//
//  六子棋的胜负结构（NCTU6 一路下来的共识）：
//    · 单个「四」其实常常是坏手：对手用 1 颗子贴死，另一颗子随便发展，
//      等于我们白送对手半手 tempo；
//    · 真正值钱的是「双三」：一颗子同时在两条不同方向各做出一条活三。
//      对手必须用 2 颗子分别去掐，而且掐了之后我们还有第三条方向在长。
//    这就是「三三制胜」「双三结构」的几何来源，也是从「有材料」走到
//    「有 tc=2 / tc=3」的唯一自然路径。
//
//  旧版只统计 n3（我方 own>=3 的窗口数），那是**按窗口**数的：
//  一条 5 连会同时落进 3~4 个重叠窗口，于是「在一条线上堆子」在 n3 上
//  看起来比「两条方向各三连」还高 —— 正好把引擎推向对手最容易封死的形状。
//  fork_three() 换成**按方向**数，且只认「恰好三连、两端都空」（四连以上
//  已经由 tc 那条精确通道处理），方向重复计数彻底消失。
// ----------------------------------------------------------------------------
static bool live_three_through(const Board& B, int p, int me, int dr, int dc) {
    int r0 = p / N, c0 = p % N;
    int lo = 0, hi = 0;
    for (;;) {
        int r = r0 + dr * (lo - 1), c = c0 + dc * (lo - 1);
        if (r < 0 || r >= N || c < 0 || c >= N) break;
        if (B.cell[r * N + c] != me) break;
        --lo;
    }
    for (;;) {
        int r = r0 + dr * (hi + 1), c = c0 + dc * (hi + 1);
        if (r < 0 || r >= N || c < 0 || c >= N) break;
        if (B.cell[r * N + c] != me) break;
        ++hi;
    }
    if (hi - lo + 1 != 3) return false;        // 只认恰好三连
    int rl = r0 + dr * (lo - 1), cl = c0 + dc * (lo - 1);
    int rh = r0 + dr * (hi + 1), ch = c0 + dc * (hi + 1);
    if (rl < 0 || rl >= N || cl < 0 || cl >= N) return false;
    if (rh < 0 || rh >= N || ch < 0 || ch >= N) return false;
    return B.cell[rl * N + cl] == 0 && B.cell[rh * N + ch] == 0;
}
// p 落下后，4 个方向各有几条活三（0~4）。调用时 p 必须为空点。
static int fork_three(Board& B, int me, int p) {
    if (p < 0 || p >= CELLS || B.cell[p]) return 0;
    B.place_raw(p, me);
    static const int D[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
    int n = 0;
    for (int d = 0; d < 4; ++d)
        if (live_three_through(B, p, me, D[d][0], D[d][1])) ++n;
    B.remove_raw(p);
    return n;
}

static long long threat_aware_score(Board& B, int me, int a, int b,
                                    const std::vector<int>& oppDt,
                                    const std::vector<int>& oppDtTc,
                                    const std::vector<ThrPair>* oppThree = nullptr) {
    int oppSide = 3 - me;
    B.place_raw(a, me);
    if (b >= 0) B.place_raw(b, me);
    int tcm = threat_count(B, me);
    int tco = threat_count(B, oppSide);
    int ev  = evaluate(B, me);
    // 形状分：落完之后我方的「活三窗口数 / 活二窗口数」。
    // 为什么要单独算：ev 是 924 个窗口的加权和，被大量 1 子窗口摊薄，
    // 数值上能到几十万，会把「这一手把三条线同时推进」这种结构性优势淹没。
    // 而六子棋的胜负恰恰由结构决定 —— 棋谱里胜方每一手都在增加自己的活三数。
    int n3 = 0, n2 = 0;
    for (size_t w = 0; w < g_windows.size(); ++w) {
        int o = (me == 1) ? B.w_own[w] : B.w_opp[w];
        int pp = (me == 1) ? B.w_opp[w] : B.w_own[w];
        if (pp != 0) continue;
        if (o >= 3) ++n3; else if (o == 2) ++n2;
    }
    int latM = latent_threats(B, me);        // 我方战线厚度（互不相交的活三数）
    int latO = latent_threats(B, oppSide);   // 对手战线厚度
    int tg  = tightness(B, a, oppSide) + (b >= 0 ? tightness(B, b, oppSide) : 0);
    if (b >= 0) B.remove_raw(b);
    B.remove_raw(a);

    long long s = 0;
    bool weWin = (tcm >= 3);
    if (weWin) s += 1000000000LL;
    else if (tcm == 2) s += 1200000LL;
    else if (tcm == 1) s += 300000LL;
    if (tco >= 3) s -= 2000000000LL;          // 送出三迫着 = 送死
    else if (tco == 2) s -= 1500000LL;
    else if (tco == 1) s -= 400000LL;
    // 抢掉对手的双迫着制造点：对手落上去能成 3 迫着的点是「下一手就输」的死穴，
    // 优先级远高于只是 2 迫着的点。
    // 注意：2 迫着的点不能重复叠加奖励——否则会出现「一回合堵两个点」比
    // 「自己造双迫着」还划算的错觉，引擎就会一路堵下去、永远不进攻（实测被磨成和局）。
    int maxBlockTc = 0, nBlocked = 0;
    for (size_t i = 0; i < oppDt.size(); ++i) {
        int p = oppDt[i];
        if (p != a && p != b) continue;
        ++nBlocked;
        int tc = (i < oppDtTc.size()) ? oppDtTc[i] : 2;
        if (tc > maxBlockTc) maxBlockTc = tc;
    }
    if (nBlocked > 0) {
        if (maxBlockTc >= 3) s += 6000000LL;
        else s += 900000LL + 100000LL * (nBlocked - 1);
    }
    // ev 降权到 1/8：它只做「同为战术档时挑哪个位置更厚」的微调。
    // 原来 ev 全额计入（可达数十万），直接压过 +30 万档的「造出一个迫着」，
    // 于是引擎会放着活三不做、去下形势分略高的闲棋 —— 棋谱里输的那一方
    // 整局 tc=0，就是这么来的。
    s += ev / 8;
    s += (long long)tg * 40;
    // 上限钳制：n3 不封顶的话，一个四面开花的局面能攒出几十个活三窗口
    // （每个 6 万），总额会压过「真的造出一个迫着」（30 万），本末倒置。
    // 钳到 10 / 24 之后，形状分的上限（60 万 + 9.6 万）仍高于 ev/8，
    // 但永远低于「多一个真迫着」这一档。
    {
        int c3 = n3 > 10 ? 10 : n3;
        int c2 = n2 > 24 ? 24 : n2;
        s += (long long)c3 * 40000 + (long long)c2 * 4000;
    }
    // 战线厚度：2 条独立战线 ≈ 一次真迫着（对手必须拿两子来掐），
    // 3 条 = 对手挡不完 → 胜势。对手的厚度反向扣分，而且扣得更重
    // （让对手攒出双战线，基本等于把主动权交出去）。
    s += (long long)latM * 120000 - (long long)latO * 150000;
    // 双三：这一手最高的那条「多方向活三」有几条。见 fork_three 的注释。
    // 权重标定（对照上面的阶梯：单迫着 30 万、双迫着 120 万、做局上限 100 万）：
    //   3 条方向 → 90 万：只差一手就能化成 tc=2/3，比做局更硬，但低于双迫着；
    //   2 条方向 → 32 万：略高于「一个孤四」（孤四等于白送对手半手），
    //                     但低于「真迫着 30 万 + 形状分」的总和，不会顶掉强制着；
    //   1 条方向 → 不加分（它已经体现在 n3 与 ev 里，重复计会推高摊大饼倾向）。
    {
        int f1 = fork_three(B, me, a);
        int f2 = (b >= 0) ? fork_three(B, me, b) : 0;
        int best = (f1 > f2 ? f1 : f2);
        if (best >= 3)      s += 900000LL;
        else if (best == 2) s += 320000LL;
    }
    // [实验回退] 方向覆盖度评分：接入后 4 局全部退化成 179 手不终（含执白
    // 从 27 手退回），说明「按方向数加分」会把引擎推去做「摊大饼」的闲棋，
    // 反而放弃即时威胁。line_dirs() 保留在代码里备用，但不参与打分。
    // 对手下一手就能做到 3 迫着（= 我们当场挡不住，下一手被秒）：
    // 这时唯一安全的走法是把它的组合全部打掉（每个组合至少命中一个点）。
    // 没打掉几个就扣几个档，逼着搜索/终审只在「全打掉」的走法里挑。
    if (!weWin && oppThree && !oppThree->empty()) {
        int u = pairs_unhit(*oppThree, a, b);
        // 惩罚必须**不饱和**。原来写成 -1.5e9 * min(u,3)，于是「漏掉 21 个组合」
        // 与「漏掉 132 个组合」得分完全一样，两者此后只能靠 ev 分高低 ——
        // 实测就是这样选错的：该占 (3,7)（漏 21 个）却去占 (14,14)（漏 132 个）。
        // 现在改为「前 3 个每个 15 亿」+「之后每个 2 亿（上限 30 个）」，
        // 让「少漏一个」也始终比「多漏一个」严格更好。
        if (u > 0) {
            int u1 = (u > 3) ? 3 : u;
            int u2 = (u > 3) ? (u - 3) : 0;
            if (u2 > 30) u2 = 30;
            s -= 1500000000LL * u1 + 200000000LL * u2;
        }
    }
    return s;
}

// ----------------------------------------------------------------------------
//  决策：分层（立即胜 → CTSS 强制胜 → 迫着层 → PVS → 迫着层终审）。
//  返回第一子 a 与第二子 b（b=-1 表 k==1）。
// ----------------------------------------------------------------------------
struct Decision { int a; int b; };
// 决策路径追踪（C6_TRACE=1 时把「走了哪条分支」打到 stderr，排查用）
static const char* g_path = "-";
static int g_tcOppDbg = 0, g_tcMeDbg = 0, g_oppThreeDbg = 0, g_tierMaxDbg = -1000;
static int g_trapCoverDbg = 0, g_trapKpDbg = 0, g_trapUniqDbg = 0;
static Decision decide(Board& B, int me, int k, int tlim) {
    g_my_color = me;
    g_path = "?";
    set_deadline(tlim);
    if (B.n_empty == CELLS) return {CENTER, -1};
    // 1) 立即胜（最高优先级，精确）
    std::vector<int> wp; win_points(B, me, wp);
    if (!wp.empty()) {
        g_path = "win6";
        int a = wp[0];
        int b = (k == 2) ? pick_offensive_second(B, me, a) : -1;
        return {a, b};
    }
    // 1b) 我方 4+2：本回合两子直接补成 6 —— 同样是「立即胜」，旧版漏掉会白丢胜局
    if (k == 2 && B.hot[me] > 0) {
        for (size_t w = 0; w < g_windows.size(); ++w) {
            int own = (me == 1) ? B.w_own[w] : B.w_opp[w];
            int opn = (me == 1) ? B.w_opp[w] : B.w_own[w];
            if (own == 4 && opn == 0) {
                std::vector<int> e;
                for (int t = 0; t < 6; ++t) { int idx = g_windows[w].c[t]; if (!B.cell[idx]) e.push_back(idx); }
                if ((int)e.size() == 2) { g_path = "4+2"; return {e[0], e[1]}; }
            }
        }
    }
    // 1c) 三迫着必胜：本回合两子把迫着数做到 >=3 → 对手一手只能挡 2 个，
    //     剩下那个下回合直接成 6。这是六子棋最短的必胜路线，必须先查、
    //     而且要精确查（不能指望搜索偶然发现）。
    //     前提：对手手上没有「4+2」这种一手成 6 的形，否则他先赢。
    if (k == 2 && !opp_has_immwin(B, me)) {
        std::vector<ThrPair> win3;
        // capPts 20 -> 48：这条是「本回合两子直接做到 3 迫着」的**唯一**转换口径，
        // 截得太窄会把真实存在的必杀着法漏掉（实测做局成功之后下一手查不到杀点对，
        // 原因就是候选点被截到 20 个）。maxOut 6 -> 10 也只是多给几条候选。
        collect_threat_pairs(B, me, 3, 48, win3, 10);
        if (!win3.empty()) { g_path = "t3win"; return {win3[0].a, win3[0].b}; }
        g_timed_out = false;
    }
    // 2) CTSS 强制胜（优先于防守：直接胜 > 防守；仅完整双子回合、对手无立即胜点时）
    //    分两趟：先小预算扫「连续四(VCF)」——便宜、命中率高；再补齐预算扫
    //    「连续双迫着(VCDT)」——双迫着的应手是完全强制的，所以这一趟才是
    //    「几步做出三迫着直接赢」的主线。
#if USE_CTSS
    if (k == 2 && !opp_has_immwin(B, me)) {
        int ca = -1, cb = -1;
        // 只在自己「手上有进攻材料」时才花时间证明杀棋：
        // 一个双迫着点都造不出来的局面，不可能存在连续双迫着的必杀线，
        // 强行搜 VCDT 只是白烧时间、把 PVS 挤掉 —— 实测执白因此从和局退化成
        // 36 手崩盘（每手少了一两秒的 PVS，闲棋质量整体下滑，10 手内就被压死）。
        std::vector<int> myDt0;
        double_threat_points(B, me, myDt0, nullptr, 40);
        g_timed_out = false;
        bool hasAttack = !myDt0.empty();
        // ★ 判据修正（本轮关键 bug）：原来只用「**一颗**子落下去后 tc>=2」判断
        //   有没有进攻材料，但 VCDT 的着法单位是**一回合两子**，双子组合达到
        //   tc>=2 的门槛比单子低得多。实测执黑对基准的第 3 手局面：
        //   单子判据只有 2 个点，双子判据有 17 组；而整局里绝大多数回合
        //   单子判据为 0、双子却有 12~38 组 —— 等于每回合都把 VCDT 锁在门外，
        //   引擎只能走「评估 + alpha-beta」的闲棋，这就是执黑全程 tcMe=0 的根因。
        if (!hasAttack) {
            std::vector<ThrPair> probe;
            collect_threat_pairs(B, me, 2, 48, probe, 4);
            hasAttack = !probe.empty();
        }
        g_timed_out = false;
        // 预算必须从**本步实际预算**里切，不能用 tlim（那是平台的 10s 上限）。
        // CTSS 最多吃掉本步的一半，剩下留给 PVS 与终审。
        // ★ 预算分配的实测结论（同一对手同一批种子，只改这一处）：
        //      CTSS 4.0s / PVS 0.0s → 执黑 69 手、执白 27 手   ← 最快
        //      CTSS 4.0s / PVS 2.0s → 执黑 73 手、执白 31 手
        //      CTSS 2.7s / PVS 1.3s → 执黑 73 手、执白 31 手
        //      CTSS 1.5s / PVS 1.5s → 执黑 73 手、执白 31 手
        //   两个结论：① CTSS 必须吃满 4 秒才有明显的「一步到位」；
        //             ② **给 PVS 更多时间反而更慢** —— 搜索越深越偏向「稳」的闲棋，
        //                会把手上的强制着让位给形势分更高的腾挪。所以 PVS 只留 1s
        //                做兜底（平静局面仍需要它选造形点）。
        long long bud_all = g_budget_ms - 1000;   // 给 PVS / 终审留 1s 兜底
        if (bud_all > 4000) bud_all = 4000;
        if (bud_all < 200) bud_all = 200;
        // 没有进攻材料 → VCF 拿全部预算（与已验证的基线行为一致，执白靠它守住）；
        // 有进攻材料 → 分 1/3 给 VCF、2/3 给 VCDT（双迫着才是真正的取胜主线）。
        long long bud_vcf = hasAttack ? (bud_all / 3) : bud_all;
        if (bud_vcf < 250) bud_vcf = 250;
        if (ctss_find(B, me, ca, cb, (int)bud_vcf, false)) {
            g_timed_out = false;
            if (cb >= 0) { g_path = "vcf"; return {ca, cb}; }
            int b = (k == 2 && !would_win(B, me, ca)) ? pick_offensive_second(B, me, ca) : -1;
            g_path = "vcf1"; return {ca, b};
        }
        g_timed_out = false;
        if (hasAttack && ctss_find(B, me, ca, cb, (int)(bud_all - bud_vcf), true)) {
            g_timed_out = false;
            if (cb >= 0) { g_path = "vcdt"; return {ca, cb}; }
            int b = (k == 2 && !would_win(B, me, ca)) ? pick_offensive_second(B, me, ca) : -1;
            g_path = "vcdt1"; return {ca, b};
        }
        g_timed_out = false;
    }
#endif
    // ------------------------------------------------------------------
    // 3) 迫着层态势
    //    tcOpp = 对手迫着数 = 我至少要几颗子才能阻止它下一手成 6。
    //    一手 2 子 → 2 个迫着是「强制防守」（两子全要用去挡），
    //    3 个以上是理论必败；我方的迫着数则是进攻资本。
    // ------------------------------------------------------------------
    int oppSide = 3 - me;
    int tcOpp = threat_count(B, oppSide);
    if (g_trace) { g_tcOppDbg = tcOpp; g_tcMeDbg = threat_count(B, me); g_oppThreeDbg = 0; }

    // 理论必败形（对手迫着数超过本回合能封堵的上限）：仍必须最顽强地拖
    if (tcOpp > k) {
        int a = -1, b = -1;
        resilient_defense(B, me, k, a, b);
        if (a >= 0) {
            if (k == 2 && (b < 0 || b == a || B.cell[b])) b = pick_offensive_second(B, me, a);
            g_path = "resilient"; return {a, b};
        }
    }
    // 双迫着制造点（活三 / 双四前身）：抢到的人掌握主动权。
    // 对手落上去即成 3 迫着的点是死穴，必须占掉——这就是「防双四」的本质。
    std::vector<int> oppDt, oppDtTc, myDt, myDtTc;
    if (k == 2) {
        double_threat_points(B, oppSide, oppDt, &oppDtTc);
        double_threat_points(B, me, myDt, &myDtTc);
    }
    int nDtHard = 0;                 // 对手「一子成 3 迫着」的死穴个数
    for (int t : oppDtTc) if (t >= 3) ++nDtHard;

    // 对手「下一手两子就能做到 3 迫着」的全部组合 —— 这才是「几步就被秒」的根源。
    // 把每个组合都打掉一个点，对手就再也凑不出 3 迫着（一回合 2 子挡不住 3 个威胁）。
    std::vector<ThrPair> oppThree;
    std::vector<int> oppThreeKeys;
    if (k == 2 && !opp_has_immwin(B, me)) {
        // capPts 从 20 提到 64、maxOut 从 48 提到 160：
        // 这一层是「对手下一手能不能做出 3 迫着（= 我们当场挡不住）」的唯一判据，
        // 组合被截断掉一个，就会把「必死」误判成「安全」，是漏防的直接来源。
        // ★ 口径不能再放大：这套组合数是 threat_aware_score 里「残余几个没打掉」
        //   的惩罚基数，一旦从 160 放大到 240，各候选之间的惩罚差被整体拉大，
        //   防守项就压过了进攻项 —— 实测执黑从 73 手退化到 117 手（虽仍胜，
        //   但把必胜局磨成了长局）。检测的完整性由下面的「硬闸门」单独保证。
        collect_threat_pairs(B, oppSide, 3, 64, oppThree, 160);
        if (!oppThree.empty()) {
            for (auto& p : oppThree) { oppThreeKeys.push_back(p.a); oppThreeKeys.push_back(p.b); }
            std::sort(oppThreeKeys.begin(), oppThreeKeys.end());
            oppThreeKeys.erase(std::unique(oppThreeKeys.begin(), oppThreeKeys.end()), oppThreeKeys.end());
            if ((int)oppThreeKeys.size() > 36) oppThreeKeys.resize(36);
        }
        g_timed_out = false;
    }
    g_oppThreeDbg = (int)oppThree.size();

    // ------------------------------------------------------------------
    //  3b) 做局（trap）：双方都没有立即杀招时，比的不是「这一步最优」，
    //      而是「谁先把对手钉死在防守上」。
    //
    //  为什么不能每步都贪最优：对手（基准 AI）也是贪的 —— 它只对**看得见的
    //  威胁**（四、双迫着点）反应，看不见「三步之后会连成三迫着」这种结构。
    //  所以真正能赢它的不是「每步都好」，而是**摆出一个它不会去拆的局**：
    //  我这边先攒出若干个「两子落上去就 tc>=3」的杀点对，等它按自己的最优
    //  去发展，下回合我直接提子成杀 —— 它算得出来也已经来不及了。
    //
    //  这一层只在 tcOpp == 0（对手没有迫着、我们不必防守）时启用：
    //  有威胁时必须先挡，做局再美也没命用。
    //  ★ 这里给出的是**候选与权重**，不是「必胜」。所有真杀（成 6 / 4+2 /
    //    三迫着 / VCF / VCDT）在前面已经 return 掉了，绝不可能被做局顶替。
    // ------------------------------------------------------------------
    Move trapMv{-1, -1};
    TrapInfo trapBest;
    g_trapCoverDbg = g_trapKpDbg = g_trapUniqDbg = 0;
    long long trapBonus = 0;
    //  ★ 硬闸门（对称防守）：对手手上已经握有「两子成 3 迫着」的杀点对时，
    //    绝对不能去安静做局 —— 他只要落上去就是 tc>=3，我们下回合挡不住，
    //    他比我们先成 6。这时候唯一的正解是**把他的杀点对全部打掉**（或自己先杀）。
    //    「光顾着给对面做局、结果掉进对面的坑」，根子就是漏了这一条。
    if (k == 2 && tcOpp == 0 && oppThree.empty() && B.n_empty < CELLS - 8) {
        long long bud = g_deadline_ms - steady_ms();
        if (bud > 400) bud = 400;                 // 做局只花小钱，主预算留给杀棋
        if (bud >= 150) {
            if (trap_best(B, me, k, trapMv, trapBest, bud)) {
                // 权重标定（基准：1 个真迫着 = 30 万，双迫着 = 120 万）：
                //   覆盖数 3 → 100 万：对手两子拆不完，下一手我真能成 6；
                //             但仍**低于双迫着**（120 万）—— 因为双迫着是当场
                //             把对手两子钉死，而他连拆我杀点对的机会都没有。
                //   覆盖数 2 → 55 万（唯一解 +15 万）：对手必须两子全用来拆，
                //             这是「绞杀」的起点，值一手真迫着。
                //   覆盖数 1 → 10 万：对手一子就拆，只算位置微调。
                // 这样排序天然是：强制+做局 > 纯强制 > 纯做局。
                int rate = (trapBest.rate < 0) ? 500 : trapBest.rate;   // 0~1000
                if (trapBest.cover >= 3) {
                    trapBonus = 600000LL + (long long)rate * 400LL;      // 最高 100 万
                } else if (trapBest.cover == 2) {
                    trapBonus = 250000LL + (long long)rate * 350LL
                              + (trapBest.uniq ? 100000LL : 0);          // 最高 70 万
                } else if (trapBest.cover == 1) {
                    trapBonus = 100000LL;
                }
                // 覆盖数 0/1 的「局」不值钱（对手一子就拆），不单独推动选点
                if (trapBest.cover < 2) { trapMv = Move{-1, -1}; trapBonus = 0; }
                if (trapMv.a >= 0) {
                    g_trapCoverDbg = trapBest.cover;
                    g_trapKpDbg    = trapBest.kp;
                    g_trapUniqDbg  = trapBest.uniq;
                }
            }
            g_timed_out = false;
        }
    }

    // ------------------------------------------------------------------
    // 4) 构建候选集：**全部是合法防守着法**（保住「绝不漏防」这条底线）
    // ------------------------------------------------------------------
    std::vector<Move> cand;
    auto push = [&](int a, int b) {
        if (a < 0 || a >= CELLS || B.cell[a]) return;
        if (k == 1) b = -1;
        // k==2 时绝不能留下 b<0（会带着 -1 进搜索 → 越界/漏子）
        if (k == 2 && b < 0) b = pick_offensive_second(B, me, a);
        if (b >= 0 && (b == a || b >= CELLS || B.cell[b])) b = -1;
        if (k == 2 && b < 0) return;                 // 棋盘几乎满等极端情形
        for (auto& m : cand) if (m.a == a && m.b == b) return;
        cand.push_back({a, b});
    };
    // 空闲子候选：对手死穴优先（占掉），其次我方双迫着点，最后才交给形势评估
    std::vector<int> freePts;
    {
        std::vector<int> idx(oppDt.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return oppDtTc[x] > oppDtTc[y]; });
        for (size_t i = 0; i < idx.size() && i < 8; ++i) {
            int p = oppDt[idx[i]];
            if (!B.cell[p] && std::find(freePts.begin(), freePts.end(), p) == freePts.end()) freePts.push_back(p);
        }
        std::vector<int> idx2(myDt.size());
        for (size_t i = 0; i < idx2.size(); ++i) idx2[i] = (int)i;
        std::sort(idx2.begin(), idx2.end(), [&](int x, int y) { return myDtTc[x] > myDtTc[y]; });
        for (size_t i = 0; i < idx2.size() && i < 6; ++i) {
            int p = myDt[idx2[i]];
            if (!B.cell[p] && std::find(freePts.begin(), freePts.end(), p) == freePts.end()) freePts.push_back(p);
        }
    }

    // ------------------------------------------------------------------
    //  我方「两子成 2 迫着」的组合 —— 防守与进攻共用
    //
    //  六子棋最高级的一条战术就是**防守反击**：用一颗子挡住对手的威胁，
    //  另一颗子同时做出自己的双迫着。原来这段枚举只写在「自由手」分支里，
    //  于是只要对手握着 1 个迫着（tcOpp=1），我方就进入强制防守分支，
    //  第二子只能从 freePts（双迫着**点**）里挑，**永远造不出双迫着**。
    //  实战后果：白方整局被迫防守，tc 恒为 0，48 手被压死。
    // ------------------------------------------------------------------
    std::vector<ThrPair> myTwo;
    if (k == 2) {
        collect_threat_pairs(B, me, 2, 96, myTwo, 48);
        g_timed_out = false;
    }

    std::vector<std::vector<int>> dsol;
    if (tcOpp >= 1) exact_defense(B, oppSide, k, dsol);
    if (tcOpp >= 1 && tcOpp <= k) {
        // 强制防守：解集里的点必须先占满，剩下的子才自由。
        // 注意：**双点解不能被拆开**——它俩是一组最小击中集，少一个就漏防。
        //
        // 等效封堵怎么挑最优？以对手横四 (9,5..8) 为例，三种挡法都对：
        //     (9,3)+(9,9) / (9,4)+(9,9) / (9,4)+(9,10)
        // 但只有「两端紧贴」(9,4)+(9,9) 是把对手这四颗子**彻底钉死**：两侧延伸点
        // 全被占掉，对手再也不可能在这条线上凑出五连。隔一格挡只是勉强过关，
        // 会把对手的连子养大、白送对方空间，实战里就是「越堵越被动」。
        // 所以排序键 = 残余迫着 → 紧贴度 → 形势。
        struct DC { int a, b, resid, ev, tg; };
        std::vector<DC> ranked;
        for (auto& sol : dsol) {
            int a = sol[0];
            if (a < 0 || a >= CELLS || B.cell[a]) continue;
            int b2 = ((int)sol.size() >= 2) ? sol[1] : -1;
            if (b2 >= 0 && (b2 == a || b2 >= CELLS || B.cell[b2])) b2 = -1;
            B.place_raw(a, me);
            if (b2 >= 0) B.place_raw(b2, me);
            int resid = threat_count(B, oppSide);
            // ★ 这里**不能**掺 latent：实测给挡点加「顺手长自己的形」的权重后，
            //   引擎开始为了凑自己的形去选偏松的挡法，攻防节奏全乱 ——
            //   黑方从 45 手胜退化到 179 手不终、白方三局全不终。挡点只认
            //   「封得死、封得紧」，长形交给自由手去做。
            int ev = evaluate(B, me);
            if (b2 >= 0) B.remove_raw(b2);
            B.remove_raw(a);
            int tg = tightness(B, a, oppSide) + (b2 >= 0 ? tightness(B, b2, oppSide) : 0);
            ranked.push_back({a, b2, resid, ev, tg});
        }
        // 排序键：残余迫着 → **用子数（少的优先）** → 紧贴度 → 形势。
        //
        // 第二项是这一轮找到的致命 bug。原来只按「紧贴度总和」排，而双点解的
        // tightness 是两个点之和，天然比单点解高 —— 于是只要存在双点解，
        // 单点解就被整体挤掉。后果极其严重：对手明明只有 1 个威胁窗口
        // （一颗子就能封死），我方却老老实实用两颗子去堵，另一颗子白白浪费，
        // 永远没有余力反击。实测白方整局 tc=0、48 手被杀，就是这么来的。
        //
        // 六子棋的铁律：**能一子封死就绝不用两子**。省下的那颗子必须拿去
        // 做自己的双迫着，这才叫攻守兼备。
        std::sort(ranked.begin(), ranked.end(), [](const DC& x, const DC& y) {
            if (x.resid != y.resid) return x.resid < y.resid;
            int nx = (x.b >= 0) ? 2 : 1;
            int ny = (y.b >= 0) ? 2 : 1;
            if (nx != ny) return nx < ny;
            if (x.tg != y.tg) return x.tg > y.tg;
            return x.ev > y.ev;
        });
        // 只保留「残余最小 + 最紧贴」的那一组。
        // 同一批等效挡法在战术上等价，但紧贴的那种直接让对手这条线彻底死掉；
        // 留着宽松挡法只会给搜索一个「看起来形势分更高、其实白送对手空间」的选择。
        int keepResid = ranked.empty() ? 0 : ranked[0].resid;
        int keepTg    = ranked.empty() ? 0 : ranked[0].tg;
        int keepN     = ranked.empty() ? 1 : ((ranked[0].b >= 0) ? 2 : 1);
        for (auto& d : ranked) {
            int dn = (d.b >= 0) ? 2 : 1;
            if (d.resid != keepResid || dn != keepN || d.tg < keepTg) continue;
            // d.a 单独能不能封住对手的全部威胁？能，则第二子完全自由，
            // 该拿去做反击；不能，才必须沿用 d.a+d.b 的两点解。
            // 注意：不能拿「tcOpp<=1」直接当判据 —— exact_defense 出于 k=2 的
            // 语义总是返回两点解，光看 tcOpp 会一直走「两点齐下」，反击永远
            // 出不来（实测 cand 只剩 2 条候选，白方整局 tc=0）。
            bool soloOk = false;
            {
                B.place_raw(d.a, me);
                soloOk = (threat_count(B, oppSide) == 0);
                B.remove_raw(d.a);
            }
            if (d.b >= 0 && !soloOk) { push(d.a, d.b); continue; }
            // 单点解（一点即可覆盖全部威胁）：d.a 已经把对手的威胁全部封死，
            // 第二子因此完全自由 —— 这里必须主动去找「反击」的第二子。
            push(d.a, -1);
            // ★★ 防守反击（本引擎此前最大的战术缺口）
            //
            //   d.a 是「挡对手」的点，它天然不在我方的进攻组合里，
            //   所以「拿 d.a 去进攻表里查搭档」永远查不到 —— 之前 cand 只剩下
            //   一两条候选就是这么来的：一个挡点，别的什么都没有。
            //   正确做法是**直接枚举**：d.a 落下之后，哪一颗第二子能让
            //   我方做出双迫着（对手必须两子全挡）或至少一个迫着？
            //   这就是六子棋最值钱的一手：挡完当回合就反制，攻防节奏立刻翻转。
            //   实测白方整局 tc=0、48 手被杀，根子就在这一条上。
            {
                B.place_raw(d.a, me);
                // 第二子有三个层次：
                //   strong = 落下去直接做出 >=2 个迫着（对手两子全得用来挡，攻守瞬间翻转）
                //   weak   = 做出 1 个迫着（也逼对手分一颗子）
                //   其余   = 纯发展子
                // ★ 排序键不能只看 ev。ev 是 924 窗口的加权和，一颗远在天边的孤子
                //   也会因为「开了几条 1 子窗口」拿到不低的分 —— 实测白方就走过
                //   (7,17) 这种和主战场毫无关系的子，等于白送半手。
                //   六子棋里一颗子的价值 = 它同时推进了几条**互不相交**的战线，
                //   所以用 latent（战线厚度）主导，n3 次之，ev 只做微调；
                //   并且直接排除「离我方所有棋子都超过 2 格」的孤立点。
                std::vector<std::pair<long long, int>> strong, weak;   // (score, q)
                for (int q = 0; q < CELLS; ++q) {
                    if (B.cell[q]) continue;
                    int qr = q / N, qc = q % N;
                    bool near = false;
                    for (int i = 0; i < CELLS && !near; ++i) {
                        if (B.cell[i] != me) continue;
                        if (std::max(std::abs(qr - i / N), std::abs(qc - i % N)) <= 2) near = true;
                    }
                    B.place_raw(q, me);
                    int tc = threat_count(B, me);
                    long long sc = 0;
                    if (tc >= 2) sc = 2000000;
                    else if (tc >= 1) sc = 500000;
                    else if (near) {
                        int lat = latent_threats(B, me);
                        int n3 = 0;
                        for (size_t w = 0; w < g_windows.size(); ++w) {
                            int o = (me == 1) ? B.w_own[w] : B.w_opp[w];
                            int pp = (me == 1) ? B.w_opp[w] : B.w_own[w];
                            if (pp == 0 && o >= 3) ++n3;
                        }
                        sc = (long long)lat * 100000 + (long long)n3 * 1000;
                    }
                    if (near || tc >= 1) sc += evaluate(B, me) / 4;
                    B.remove_raw(q);
                    if (tc >= 2) strong.push_back({sc, q});
                    else if (sc > 0) weak.push_back({sc, q});
                }
                B.remove_raw(d.a);
                // ★★ 防守时也要给自己作势（这是「被动防守」和「守中带攻」的分水岭）
                //
                //  只有在**第一子已经把对手封死**（单点解）时才有这颗自由子。
                //  过去这颗子只按 latent / n3 / ev 挑，本质是「发展子」——
                //  对手只要不按我们的预想走，这颗子就白费了。
                //  正确做法：用**杀点对覆盖数**去挑 —— 落下去之后我能攒出几个
                //  「两子上去就 tc>=3」的杀点对、对手要几子才能全拆掉。
                //  这样即使对手完全不理我们的计划，我们的结构也在自己长；
                //  而一旦我们攒到覆盖数 >= 2，对手就必须整回合拿来拆，
                //  主动权就回到我们手里（绞杀的机制）。
                //
                //  ★ 只对**第二子**做这件事，第一子的封堵排序完全不动 ——
                //    上一轮给挡点掺 latent 导致攻防节奏全乱（黑方 45 手胜退化到
                //    179 手不终），根因就是「为了长自己的形而选了偏松的挡法」。
                //    封堵只认「封得死、封得紧」，长形交给这颗自由子。
                auto applyTrapBias = [&](std::vector<std::pair<long long, int>>& vec,
                                         int topK) {
                    if (vec.empty()) return;
                    std::sort(vec.begin(), vec.end(),
                              [](const std::pair<long long,int>& x,
                                 const std::pair<long long,int>& y) { return x.first > y.first; });
                    int lim = ((int)vec.size() < topK) ? (int)vec.size() : topK;
                    for (int t = 0; t < lim; ++t) {
                        int q = vec[t].second;
                        if (B.cell[q] || B.cell[d.a]) continue;
                        B.place_raw(d.a, me);
                        B.place_raw(q, me);
                        TrapInfo T = trap_eval(B, me);
                        B.remove_raw(q);
                        B.remove_raw(d.a);
                        if (T.cover >= 2)
                            vec[t].first += (long long)T.cover * 700000LL
                                          + (T.uniq ? 200000LL : 0)
                                          + (long long)T.kp * 2000LL;
                    }
                    std::sort(vec.begin(), vec.end(),
                              [](const std::pair<long long,int>& x,
                                 const std::pair<long long,int>& y) { return x.first > y.first; });
                };
                applyTrapBias(strong, 8);
                applyTrapBias(weak, 6);
                g_timed_out = false;
                for (auto& pr : strong) push(d.a, pr.second);
                if (strong.empty()) {
                    std::sort(weak.begin(), weak.end(),
                              [](const std::pair<long long,int>& x, const std::pair<long long,int>& y) {
                                  return x.first > y.first;
                              });
                    for (size_t t = 0; t < weak.size() && t < 10; ++t) push(d.a, weak[t].second);
                }
                g_timed_out = false;
            }
            for (int q : freePts) if (q != d.a && !B.cell[q]) push(d.a, q);
            if (nDtHard > 0) {
                for (size_t i = 0; i < oppDt.size(); ++i)
                    if (oppDtTc[i] >= 3 && oppDt[i] != d.a && !B.cell[oppDt[i]]) push(d.a, oppDt[i]);
            }
            // 第二子顺手把对手「两子成 3 迫着」的组合键点打掉（能全打掉最好）
            for (int q : oppThreeKeys)
                if (q != d.a && !B.cell[q] && pairs_unhit(oppThree, d.a, q) == 0) push(d.a, q);
        }
    } else {
        // 自由手（对手无立即威胁）：优先造自己的双迫着 / 占掉对手死穴
        for (size_t i = 0; i < myDt.size() && i < 10; ++i) {
            int p = myDt[i];
            if (B.cell[p]) continue;
            push(p, pick_offensive_second(B, me, p));
            for (int q : freePts) if (q != p) push(p, q);
        }
        // 我方「两子成 2 迫着」的强制着：六子棋里唯一能抢到主动权的手段
        // （对手必须两子全用来挡，攻防节奏立刻转到我方）。tc>=3 的已在 1c) 直接返回。
        if (k == 2) {
            // myTwo 已在上面统一枚举（capPts=96：中局这张表通常有 60~120 个点，
            // 截到 32 等于把大半的进攻着法丢掉，实测白方因此一个双迫着都造不出）
            for (auto& p : myTwo) push(p.a, p.b);
            g_timed_out = false;
            // 连双迫着都造不出来时，退一档找「造一个迫着（冲四 / 活三）」的组合。
            // 六子棋的主动权是按「谁在强制对手」计的：造不出 2 个迫着，能造 1 个
            // 也远好过什么都不造 —— 对手必须分一颗子来挡，节奏就慢一拍。
            // 原来这一档缺失，导致中局双方都无威胁时，候选只剩「形势分最高」的
            // 闲棋，实战表现就是长局反复拉锯（117 手、129 手那种）。
            if (myTwo.empty()) {
                std::vector<ThrPair> myOne;
                collect_threat_pairs(B, me, 1, 96, myOne, 40);
                for (auto& p : myOne) push(p.a, p.b);
                g_timed_out = false;
            }
        }
        // 形状点：把「一子多线」的交叉点显式放进候选。
        // 没有这一步，开局阶段（双方 0 迫着）的候选只来自 gen_moves 的前 8 条，
        // 而那 8 条是按「落在几个 own>=3 窗口」排的 —— 开局根本没有这种窗口，
        // 于是等于随机取前 8 个空点。这就是「先手开局走不出骨架、被对方牵着走」
        // 的直接原因。minOwn=2 才对应「活二的延伸点」，也就是真正的开局选点。
        {
            // 两档：先找「活二的延伸点」（own>=2 的窗口），
            // 开局第一两个回合自己的子还不够时这个集合是空的 ——
            // 此时退到 own>=1，也就是「贴着自己已有棋子延伸」的点。
            // 这一步是「先手开局」的关键：没有它，第 2 回合（盘上只有黑 1 子）
            // 候选集等于空，引擎只能从 gen_moves 里随便拿前 8 个空点，
            // 于是开局永远走不出骨架，全程被对手牵着走。
            std::vector<int> sp;
            shape_points(B, me, 2, 10, sp);
            if ((int)sp.size() < 4) shape_points(B, me, 1, 10, sp);
            // 自己一颗子都没有（后手第一回合）→ 用「距对手 2~3 格」的点
            if ((int)sp.size() < 4) opening_points(B, me, 10, sp);
            for (int p : sp) push(p, pick_offensive_second(B, me, p));
            // 形状点两两配对 —— 这是「一个回合就把两条线同时推进」的主要手段
            // （棋谱里姜雨涵的第 13、14 回合就是这样一次落下两子，直接把
            // 单线变成横竖交叉的双活三）。只取前 6 个点，15 对，成本可忽略。
            for (size_t i = 0; i < sp.size() && i < 6; ++i)
                for (size_t j = i + 1; j < sp.size() && j < 6; ++j)
                    push(sp[i], sp[j]);
        }
        // 做局手必须进候选集：它通常是一手「安静棋」（tc 不变、形状分也不爆表），
        // 上面那些「造迫着 / 抢死穴」的生成器一条都不会产出它。
        if (trapMv.a >= 0) push(trapMv.a, trapMv.b);
        // 对手下一手就能做出 3 迫着（= 我们被秒）→ 必须把它的每个组合都打掉。
        // 这一条是「几步就被双四秒掉」的根治病：不打掉，评分层面就是必败。
        if (!oppThree.empty()) {
            for (size_t i = 0; i < oppThreeKeys.size(); ++i) {
                if (B.cell[oppThreeKeys[i]]) continue;
                if (pairs_unhit(oppThree, oppThreeKeys[i], -1) == 0)
                    push(oppThreeKeys[i], pick_offensive_second(B, me, oppThreeKeys[i]));
                for (size_t j = i + 1; j < oppThreeKeys.size(); ++j) {
                    if (B.cell[oppThreeKeys[j]]) continue;
                    if (pairs_unhit(oppThree, oppThreeKeys[i], oppThreeKeys[j]) == 0)
                        push(oppThreeKeys[i], oppThreeKeys[j]);
                }
            }
            // [实验回退] 这里曾经加过「拆不干净时按残余最少挑 6 组」的兜底，
            // 本意是防对手做局，实测反而把引擎推成纯防守：执黑 73 手 → 117 手、
            // 1300 种子 101 手。原因是它给「残余最少」的着法开了后门，而这类
            // 着法多半是纯挡、不长自己的形。防做局的正解是下面那条**硬闸门**
            // （对手有杀点对时不做局），不是在这里补防守候选。
        }
        for (size_t i = 0; i < oppDt.size() && i < 10; ++i) {
            int p = oppDt[i];
            if (B.cell[p]) continue;
            push(p, pick_offensive_second(B, me, p));
        }
        // 对手死穴两子齐上
        for (size_t i = 0; i < oppDt.size(); ++i) {
            if (oppDtTc[i] < 3 || B.cell[oppDt[i]]) continue;
            for (size_t j = 0; j < oppDt.size(); ++j) {
                if (i == j || B.cell[oppDt[j]]) continue;
                push(oppDt[i], oppDt[j]);
            }
        }
        std::vector<Move> base; gen_moves(B, me, k, base);
        for (size_t i = 0; i < base.size() && i < 8; ++i) push(base[i].a, base[i].b);
    }
    if (cand.empty()) {                            // 兜底：绝不能返回空
        std::vector<Move> base; gen_moves(B, me, k, base);
        for (auto& m : base) push(m.a, m.b);
    }
    if (cand.empty()) {
        // 绝不能再「返回棋盘第一个空点」—— 那等于把子送到角上。
        // 退而求其次也要选离中心最近的空点。
        int best = -1, bd = 1 << 30;
        for (int i = 0; i < CELLS; ++i) {
            if (B.cell[i]) continue;
            int dr = i / N - 9, dc = i % N - 9;
            int d = dr * dr + dc * dc;
            if (d < bd) { bd = d; best = i; }
        }
        if (best >= 0) return {best, -1};
        return {CENTER, -1};
    }

    // ------------------------------------------------------------------
    // 5) 搜索 + 迫着层终审
    //    · 强制防守时：只能在「必防解」里搜（保证不漏防）。
    //    · 自由手时：搜索必须看**全量候选**（战术候选只是前置），否则大局观
    //      会被砍掉，实战表现为「一直堵、一直不进攻」，把胜局磨成和局。
    // ------------------------------------------------------------------
    std::vector<Move> searchSet;
    if (tcOpp >= 1) searchSet = cand;
    else {
        searchSet = cand;
        std::vector<Move> base; gen_moves(B, me, k, base);
        for (auto& m : base) {
            bool dup = false;
            for (auto& c2 : searchSet) if (c2.a == m.a && c2.b == m.b) { dup = true; break; }
            if (!dup) searchSet.push_back(m);
            if ((int)searchSet.size() >= TOTAL_CAP + 40) break;
        }
    }

    // ==================================================================
    //  战术分桶：自由手时只在「能造出最多迫着」的那一档里搜索
    //
    //  依据来自两份真实棋谱（姜雨涵 vs 官方基准，先手胜 / 后手胜各一局），
    //  逐回合统计双方迫着数后规律极其干净：
    //
    //      胜方  每回合结束时的 tc：1 1 1 2 2 2 3 3 ……（从未出现 0）
    //      败方  整局 tc = 0，只有被逼到最后一手才勉强 1
    //
    //  也就是说六子棋的「主动权」是可以量化的：**谁每回合握着迫着，谁在赢**。
    //  一手的价值首先取决于它把 tc 推到几，其次才是形势分。
    //
    //  为什么必须用「分桶」而不是「加权」：加权会被 ev 淹没（score_b 是 924 个
    //  窗口的累加，轻松上到几十万），而分桶是硬隔离——同一档内让搜索自由发挥，
    //  跨档则由棋理直接裁决。而且桶一收窄，搜索的时间集中到真正有意义的着法上，
    //  深度反而更高。
    // ==================================================================
    if (tcOpp < 1 && k == 2 && cand.size() >= 4) {
        std::vector<int> tier(cand.size(), 0);
        int maxTier = -1000;
        for (size_t i = 0; i < cand.size(); ++i) {
            if (B.cell[cand[i].a]) continue;
            if (cand[i].b >= 0 && B.cell[cand[i].b]) continue;
            B.place_raw(cand[i].a, me);
            if (cand[i].b >= 0) B.place_raw(cand[i].b, me);
            int t1 = threat_count(B, me);          // 我造出几个迫着
            int t2 = threat_count(B, oppSide);     // 顺手送对手几个
            // 前驱形状：自己 3 子 / 2 子且对方 0 子的窗口数（活三 / 活二的种子）。
            // 这是开局阶段唯一的「位置价值」语言 —— 开局谁都还没有迫着，比的就是
            // 谁先把棋子摆成多方向交叉的骨架。复盘姜雨涵的先手胜局，她的每一手
            // 都同时落在 2~4 条线的交叉点上（同一子参与横向、竖向、斜向多个窗口），
            // 而这类点位正是 n3/n2 高的点。
            int n3 = 0, n2 = 0;
            for (size_t w = 0; w < g_windows.size(); ++w) {
                int o = (me == 1) ? B.w_own[w] : B.w_opp[w];
                int p = (me == 1) ? B.w_opp[w] : B.w_own[w];
                if (p != 0) continue;
                if (o >= 3) ++n3;
                else if (o == 2) ++n2;
            }
            if (cand[i].b >= 0) B.remove_raw(cand[i].b);
            B.remove_raw(cand[i].a);
            // 档位设计（间距必须拉开，否则整数比较会失真）：
            //   1 个迫着 = +100 档；3 个迫着 = +1000 档（直接必胜）
            //   对手 1 个迫着 = -60（半强制，让一步还行）
            //   前驱形状最多 +63 +20 = +83 < 100 → 永远不会盖过「多一个真迫着」，
            //   但在同为 0 迫着时能明确选出「最像活三的那一手」。
            if (n3 > 9) n3 = 9;
            if (n2 > 20) n2 = 20;
            // 战线厚度：互不相交的活三数（见 latent_threats）。
            // n3 会把「一条线堆子」算成很高（多个重叠窗口），latent 只看
            // 「对手要几子才能全掐死」，正好纠正这个偏差 —— 这是执白从
            // 「一条横线被两头堵死、179 手杀不掉」走出来的关键。
            int latM = latent_threats(B, me);
            int latO = latent_threats(B, oppSide);
            tier[i] = (t1 >= 3 ? 1000 : t1 * 100) - t2 * 60
                    + latM * 40 - latO * 40 + n3 * 4 + n2;
            // 【极开局】后手不允许紧贴先手一格。
            //
            // 棋谱依据：122 局姜雨涵执白，第一回合落在 (11,7)(12,8)，距黑方首手
            // 天元 (9,9) 恰好是 2~3 格。这不是随手 —— 后手贴一格（距离 1）会
            // 被先手直接「借力」：先手顺势在那颗子旁边延伸，等于免费多一个支点；
            // 而离 2~3 格，先手用不上，我方却能同时压住那条线的走向。
            // 实测引擎执白开局总是走 (8,8)(9,8) 这种紧贴点，十个回合就被压死。
            if (B.n_empty >= CELLS - 6) {
                auto nearOpp = [&](int idx) {
                    int r = idx / N, c = idx % N, best = 99;
                    for (int j = 0; j < CELLS; ++j) {
                        if (B.cell[j] != 3 - me) continue;
                        int d = std::max(std::abs(r - j / N), std::abs(c - j % N));
                        if (d < best) best = d;
                    }
                    return best;
                };
                int da = (nearOpp(cand[i].a) == 1) ? 1 : 0;
                int db = (cand[i].b >= 0 && nearOpp(cand[i].b) == 1) ? 1 : 0;
                tier[i] -= (da + db) * 70;
            }
            if (tier[i] > maxTier) maxTier = tier[i];
        }
        g_tierMaxDbg = maxTier;
        std::vector<Move> keep;
        // 先收最高档
        for (size_t i = 0; i < cand.size(); ++i)
            if (tier[i] == maxTier) keep.push_back(cand[i]);
        // （旧版这里又把最高档重复 push 了一遍：root_search 会把同一着法搜两次，
        //   等于把根节点的有效宽度砍半 —— 已删除）
        for (int lim = 1; lim <= 3 && (int)keep.size() < 8; ++lim)
            for (size_t i = 0; i < cand.size(); ++i)
                if (tier[i] < maxTier && tier[i] >= maxTier - 60 * lim) keep.push_back(cand[i]);
        // 兜底：万一 keep 还是空，退回原集合
        if (!keep.empty() && keep.size() < searchSet.size()) searchSet.swap(keep);
        g_timed_out = false;
    }
    // ------------------------------------------------------------------
    //  根候选收窄：原来把 740 条（cand + 全量 gen_moves）全塞进迭代加深，
    //  等于每一层都把时间平摊成 740 份，实测 4 秒只搜到 depth=3。
    //  这里按「落地后的静态分」排序，只留前 64，并且**战术候选一条不砍**
    //  （它们代表强制/必防，是大局观的核心）。深度立刻翻倍。
    // ------------------------------------------------------------------
    if ((int)searchSet.size() > 64) {
        std::vector<Move> keep = cand;
        std::vector<std::pair<int, int>> sv;
        sv.reserve(searchSet.size());
        for (size_t i = 0; i < searchSet.size(); ++i) {
            Move& m = searchSet[i];
            bool dup = false;
            for (auto& k2 : keep) if (k2.a == m.a && k2.b == m.b) { dup = true; break; }
            if (dup) continue;
            B.place_raw(m.a, me);
            if (m.b >= 0) B.place_raw(m.b, me);
            int ev = evaluate(B, me);
            if (m.b >= 0) B.remove_raw(m.b);
            B.remove_raw(m.a);
            sv.push_back({ev, (int)i});
        }
        std::stable_sort(sv.begin(), sv.end(), [](const std::pair<int,int>& x, const std::pair<int,int>& y) {
            return x.first > y.first;
        });
        for (size_t i = 0; i < sv.size() && (int)keep.size() < 64; ++i)
            keep.push_back(searchSet[sv[i].second]);
        searchSet.swap(keep);
    }
    int bestIdx = -1;
    int rootVal = 0;
    if (searchSet.size() >= 2) {
        g_root_restrict = &searchSet;
        rootVal = root_search(B, me, k, bestIdx);
        g_root_restrict = nullptr;
        g_timed_out = false;
    }
    int sa = cand[0].a, sb = cand[0].b;
    if (bestIdx >= 0 && bestIdx < (int)searchSet.size()) { sa = searchSet[bestIdx].a; sb = searchSet[bestIdx].b; }

    // 搜索已算出必胜 → 直接采用（战术层不必再介入）
    if (rootVal >= WIN - 100) { g_path = "search_win"; return {sa, sb}; }

    // 终审集合 = 战术候选 + 搜索的选择
    std::vector<Move> arb = cand;
    bool hasSearch = false;
    for (auto& m : arb) if (m.a == sa && m.b == sb) { hasSearch = true; break; }
    if (!hasSearch) arb.push_back({sa, sb});

    long long bestS = -(1LL << 62);
    int ba = sa, bb = sb;
    long long searchS = -(1LL << 62);
    std::vector<long long> sc(arb.size(), -(1LL << 62));
    for (size_t i = 0; i < arb.size(); ++i) {
        Move& m = arb[i];
        if (B.cell[m.a]) continue;
        if (m.b >= 0 && (m.b == m.a || B.cell[m.b])) continue;
        long long s = threat_aware_score(B, me, m.a, m.b, oppDt, oppDtTc, &oppThree);
        // 做局加权：只加在那一手特定着法上，权重被刻意压在「三迫着」(10 亿)
        // 与「双迫着」(120 万) 之上、但永远低于任何真杀。
        if (trapBonus > 0 && m.a == trapMv.a && m.b == trapMv.b) s += trapBonus;
        sc[i] = s;
        if (m.a == sa && m.b == sb) searchS = s;
        if (s > bestS) { bestS = s; ba = m.a; bb = m.b; }
    }
    // ------------------------------------------------------------------
    //  双迫着点潜力 —— 六子棋真正的位置货币
    //
    //  dtMe  = 我落上去就能做出 >=2 个迫着的空点数（活三 / 双四的种子）
    //  dtOpp = 对手同类点数
    //
    //  谁的这类点多，谁掌握主动。实战复现过：白方一路只做「形势分最高」的闲棋，
    //  放任对手把 dtOpp 堆到 8 个，下一手对手随便落一个就是双四、三迫着，
    //  5 步之内被反杀 —— 而每一步单独看「形势分」都不亏。
    //  所以战术分接近时必须用「dtMe − dtOpp」把真有攻势的着法挑出来。
    //  只对前几个候选精算，成本可控（K×点数×threat_count，实测 < 5ms）。
    // ------------------------------------------------------------------
    auto dtPotential = [&](int a, int b, int& mine, int& his) {
        B.place_raw(a, me);
        if (b >= 0) B.place_raw(b, me);
        std::vector<int> t1, t2;
        double_threat_points(B, me, t1, nullptr, 26);
        double_threat_points(B, oppSide, t2, nullptr, 26);
        mine = (int)t1.size(); his = (int)t2.size();
        if (b >= 0) B.remove_raw(b);
        B.remove_raw(a);
    };
    {
        std::vector<int> idx(arb.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = (int)i;
        std::sort(idx.begin(), idx.end(), [&](int x, int y) { return sc[x] > sc[y]; });
        int K = 0;
        for (size_t t = 0; t < idx.size() && K < 4; ++t) {
            int i = idx[t];
            if (sc[i] <= -(1LL << 61)) continue;
            if (sc[i] + 900000LL < bestS) break;      // 战术上已明显更差，不必精算
            int mine = 0, his = 0;
            dtPotential(arb[i].a, arb[i].b, mine, his);
            long long s2 = sc[i] + 120000LL * mine - 150000LL * his;
            if (arb[i].a == sa && arb[i].b == sb) searchS = s2;
            if (s2 > bestS) { bestS = s2; ba = arb[i].a; bb = arb[i].b; }
            ++K;
        }
    }
    // 搜索的选择只要在战术上没明显更差（差距 < 阈值），就尊重搜索的形势判断。
    // ★ 阈值必须**小于「多一个真迫着」的价值（30 万）**。原来写 40 万，
    //   等于允许搜索用「形势分稍微高一点」的闲棋去盖掉一手真的迫着 ——
    //   实测加时到 8 秒后搜索更深、更爱走「稳」的闲棋，执白反而从 51 手
    //   拖到 171 手，根子就在这里。现在只允许在「战术上几乎等价」时让搜索微调。
    int fa = ba, fb = bb;
    if (searchS >= 0 && searchS >= bestS - 90000) { g_path = "search"; fa = sa; fb = sb; }
    else g_path = "tactics";
    // 做局手被采纳（且没有任何真杀在前面返回过）→ 单独标记，便于复盘统计命中率
    if (trapBonus > 0 && fa == trapMv.a && fb == trapMv.b) g_path = "trap";

    // ------------------------------------------------------------------
    //  6) 送死检验（保守防守 / conservative defense）
    //
    //  把最终着法落下去，让**对手**当攻击方跑一次 CTSS：对手能不能靠连续
    //  双迫着 / 连续四强制走成 6？
    //
    //  为什么静态评分筛不掉这类手：连续双迫着是 4~8 步的强制序列，任何
    //  静态指标（窗口分、双迫着点个数、紧贴度）在第一步都看不出问题——
    //  实测里「每一步单看都不亏、五步之后被带走」就是这么发生的。
    //  所以最后一关必须是搜索证明，不能是打分。
    //
    //  只在对手确实有进攻材料（存在双迫着点）时才跑，代价可忽略；
    //  时间不够（余量 < 700ms）就跳过，绝不为了保险把自己拖到超时。
    // ------------------------------------------------------------------
    if (k == 2 && !oppDt.empty() && steady_ms() + 700 < g_deadline_ms) {
        auto looksSafe = [&](int a, int b) -> bool {
            if (a < 0 || B.cell[a]) return false;
            if (b >= 0 && (b == a || B.cell[b])) return false;
            B.place_raw(a, me);
            if (b >= 0) B.place_raw(b, me);
            bool safe;
            if (B.hot[oppSide] > 0) safe = false;          // 这一手等于送礼：对手一手成 6
            else {
                long long left = g_deadline_ms - steady_ms() - 250;
                int bud = (left > 600) ? 600 : (int)left;
                if (bud < 120) safe = true;                // 时间不够，不改变决定
                else {
                    int xa = -1, xb = -1;
                    safe = !ctss_find(B, oppSide, xa, xb, bud, true);
                    g_ctss_deadline_ms = 0;
                    g_timed_out = false;
                }
            }
            if (b >= 0) B.remove_raw(b);
            B.remove_raw(a);
            return safe;
        };
        if (!looksSafe(fa, fb)) {
            std::vector<int> ord(arb.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = (int)i;
            std::sort(ord.begin(), ord.end(), [&](int x, int y) { return sc[x] > sc[y]; });
            int tries = 0;
            for (int i : ord) {
                if (tries >= 5) break;
                if (steady_ms() + 700 >= g_deadline_ms) break;
                if (arb[i].a == fa && arb[i].b == fb) continue;
                ++tries;
                if (looksSafe(arb[i].a, arb[i].b)) {
                    fa = arb[i].a; fb = arb[i].b; g_path = "safeguard"; break;
                }
            }
        }
    }
    return {fa, fb};
}

// ----------------------------------------------------------------------------
//  极简 JSON 解析
// ----------------------------------------------------------------------------
struct JVal {
    enum T { NUL, NUM, STR, ARR, OBJ, BOOL } t = NUL;
    double num = 0; std::string str; std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj; bool b = false;
    const JVal* get(const char* k) const {
        for (auto& p : obj) if (p.first == k) return &p.second; return nullptr;
    }
};
struct JParser {
    const char* s; size_t i = 0;
    void skipws() { while (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') ++i; }
    JVal parse() {
        skipws(); JVal v; char c = s[i];
        if (c == '{') {
            v.t = JVal::OBJ; ++i; skipws();
            while (s[i] && s[i] != '}') {
                JVal key = parse(); skipws();
                if (s[i] == ':') ++i; skipws();
                JVal val = parse(); v.obj.push_back({ key.str, std::move(val) });
                skipws(); if (s[i] == ',') ++i; skipws();
            }
            if (s[i] == '}') ++i;
        } else if (c == '[') {
            v.t = JVal::ARR; ++i; skipws();
            while (s[i] && s[i] != ']') { v.arr.push_back(parse()); skipws(); if (s[i] == ',') ++i; skipws(); }
            if (s[i] == ']') ++i;
        } else if (c == '"') {
            v.t = JVal::STR; ++i; std::string out;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\') { ++i; char e = s[i]; if (e == 'n') out += '\n'; else if (e == 't') out += '\t';
                else if (e == 'r') out += '\r'; else out += e; ++i; }
                else out += s[i++];
            }
            if (s[i] == '"') ++i; v.str = out;
        } else if ((c >= '0' && c <= '9') || c == '-') {
            v.t = JVal::NUM; char* end; v.num = std::strtod(s + i, &end); i = (size_t)(end - s);
        } else if (c == 't') { v.t = JVal::BOOL; v.b = true; i += 4; }
        else if (c == 'f') { v.t = JVal::BOOL; v.b = false; i += 5; }
        else if (c == 'n') { v.t = JVal::NUL; i += 4; }
        return v;
    }
};
// 回显 request_id。
// 旧实现是对原始报文做字符串匹配，只认 `"request_id": "xxx"` 这一种写法；
// 平台若发数字（`"request_id": 12`）或带转义的字符串，匹配失败就会回一个空串，
// 在严格校验 request_id 的 runner 上直接构成 PROTOCOL_ERROR。
// 现在按解析结果的**类型**回显：数字原样输出，字符串重新转义后输出。
static std::string rid_json(const JVal& req) {
    const JVal* v = req.get("request_id");
    if (!v) return "\"\"";
    if (v->t == JVal::NUM) {
        char b[40];
        double d = v->num;
        if (d == (double)(long long)d) std::snprintf(b, sizeof(b), "%lld", (long long)d);
        else std::snprintf(b, sizeof(b), "%.17g", d);
        return std::string(b);
    }
    if (v->t == JVal::STR) {
        std::string o = "\"";
        for (char c : v->str) {
            switch (c) {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': o += "\\r";  break;
                case '\t': o += "\\t";  break;
                default:   o += c;      break;
            }
        }
        o += "\"";
        return o;
    }
    if (v->t == JVal::BOOL) return v->b ? "true" : "false";
    return "null";
}

// ----------------------------------------------------------------------------
//  跨请求第二子规划状态
// ----------------------------------------------------------------------------
static bool     g_plan_valid = false;
static int      g_plan_b = -1;
static uint64_t g_plan_hash = 0;
static int      g_plan_color = 0;

// 安全取数：JVal::get() 找不到字段时返回 nullptr，直接 ->num 会段错误（= 平台 CRASH 判负）。
// 平台上「CRASH」和「PROTOCOL_ERROR」比「棋力不够」更常见（本轮实测同一天就有两名
// 选手因这两类原因零分），所以每一个外部字段都必须能被缺省值兜住。
static inline double jnum(const JVal* v, double def = 0.0) {
    return (v && v->t == JVal::NUM) ? v->num : def;
}

static int run_bot() {
    if (const char* tr = std::getenv("C6_TRACE")) if (tr[0] == '1') g_trace = true;
    // 用 getline 而不是 fgets(70000)：固定缓冲在平台附加大 payload（history /
    // 回放片段）时会截断输入 → JSON 解析出半个对象 → 拿不到 board → 静默 skip
    // → 平台等不到应答 → PROTOCOL_ERROR。
    std::string raw;
    while (std::getline(std::cin, raw)) {
        if (raw.find("observation") == std::string::npos) continue;
        JParser p; p.s = raw.c_str(); JVal req = p.parse();
        const JVal* obs = req.get("observation"); if (!obs) continue;
        const JVal* boardv = obs->get("board"); if (!boardv) continue;
        if (boardv->t != JVal::ARR || (int)boardv->arr.size() != N) continue;
        int my_color = (int)jnum(obs->get("current_player"), 0);
        if (my_color != 1 && my_color != 2) continue;
        int required  = (int)jnum(obs->get("required_stones_this_turn"), 1);
        int mit       = (int)jnum(obs->get("moves_in_turn"), 0);
        int move_no   = (int)jnum(obs->get("move_number"), 0);
        int tlim      = (int)jnum(req.get("time_limit_ms"), 10000);
        if (tlim <= 0 || tlim > 60000) tlim = 10000;
        (void)required;

        uint8_t src[CELLS];
        bool shapeOk = true;
        for (int r = 0; r < N && shapeOk; ++r) {
            const JVal& rowv = boardv->arr[r];
            if (rowv.t != JVal::ARR || (int)rowv.arr.size() != N) { shapeOk = false; break; }
            for (int c = 0; c < N; ++c) {
                int v = (int)jnum(&rowv.arr[c], 0);
                if (v < 0 || v > 2) v = 0;
                src[r * N + c] = (uint8_t)v;
            }
        }
        if (!shapeOk) continue;

        Board B; B.load(src);
        uint64_t curHash = B.hash;

        int out_r, out_c;
        if (mit == 1 && g_plan_valid &&
            my_color == g_plan_color && curHash == g_plan_hash &&
            g_plan_b >= 0 && g_plan_b < CELLS && B.cell[g_plan_b] == 0) {
            // 次子：直接落之前规划好的第二子（同一回合，对手未动，第二子必仍为空）。
            // 除了棋盘哈希，还额外核验「该点确实仍为空」—— 平台若因任何原因
            // 重发/乱序请求，这里会安全地退回完整搜索，而不是输出一个非法落点。
            out_r = g_plan_b / N; out_c = g_plan_b % N;
            g_plan_valid = false;
        } else {
            g_plan_valid = false;
            // 单子请求模式：首子(mit==0)按「双子回合」搜索以发挥双子协同，并规划第二子；
            // 次子(mit==1)若规划失效则回退为单子决策（仅补一子，安全合法）。
            // 判据用 moves_in_turn 而非 required：平台恒为 required=1，但某些 runner
            // 会把 required 传成「本回合剩余子数」，用 mit 判断更鲁棒。
            int k = (mit == 0) ? 2 : 1;
            Decision d = decide(B, my_color, k, tlim);
            if (g_trace) {
                char bb[16]; 
                if (d.b < 0) std::snprintf(bb, sizeof(bb), "-");
                else std::snprintf(bb, sizeof(bb), "%d,%d", d.b / N, d.b % N);
                std::fprintf(stderr, "[mv%d k=%d] path=%s a=(%d,%d) b=%s tcMe=%d tcOpp=%d opp3=%d trap(c=%d,kp=%d,u=%d)\n",
                             move_no, k, g_path, d.a / N, d.a % N, bb,
                             g_tcMeDbg, g_tcOppDbg, g_oppThreeDbg,
                             g_trapCoverDbg, g_trapKpDbg, g_trapUniqDbg);
            }
            // ★ 最终合法性闸门：不管 decide 返回什么，发给平台的必须是一个
            //   真实空点且在 0..18 内。这里任何一次越界/占用都会直接变成
            //   平台的 ILLEGAL_ACTION / PROTOCOL_ERROR（= 一局白送）。
            if (d.a < 0 || d.a >= CELLS || B.cell[d.a] != 0) {
                int bestDeg = 1 << 30; d.a = -1;
                for (int i = 0; i < CELLS; ++i) {
                    if (B.cell[i]) continue;
                    int deg = std::abs(i / N - N / 2) + std::abs(i % N - N / 2);
                    if (deg < bestDeg) { bestDeg = deg; d.a = i; }
                }
                if (d.a < 0) d.a = CENTER;      // 棋盘已满（和棋边界），占位保协议
            }
            out_r = d.a / N; out_c = d.a % N;
            bool aWins = would_win(B, d.a, my_color);
            if (k == 2 && !aWins && d.b >= 0 && B.cell[d.b] == 0) {
                Board B2 = B; B2.place_raw(d.a, my_color);
                g_plan_valid = true; g_plan_b = d.b; g_plan_hash = B2.hash; g_plan_color = my_color;
            }
        }
        std::string rid = rid_json(req);
        std::printf("{\"protocol\":\"arena.v1\",\"request_id\":%s,\"type\":\"move_response\","
                    "\"action\":{\"kind\":\"PLACE\",\"row\":%d,\"col\":%d}}\n",
                    rid.c_str(), out_r, out_c);
        std::fflush(stdout);
    }
    return 0;
}

// ----------------------------------------------------------------------------
//  自检
// ----------------------------------------------------------------------------
static int selftest() {
    init_windows(); init_zob();
    int fails = 0;
    if (g_windows.empty()) { std::printf("FAIL: windows empty\n"); ++fails; }

    // ==== 窗口表完整性（防 uint8_t 索引截断回归，曾导致下半盘威胁判断全失效）====
    {
        int bad = 0;
        for (size_t w = 0; w < g_windows.size(); ++w) {
            int i0 = g_windows[w].c[0], i5 = g_windows[w].c[5];
            int r0 = i0 / N, c0 = i0 % N, r5 = i5 / N, c5 = i5 % N;
            int sr = (r5 - r0) / 5, sc = (c5 - c0) / 5;
            for (int k = 0; k < 6; ++k) {
                int expect = (r0 + sr * k) * N + (c0 + sc * k);
                if ((int)g_windows[w].c[k] != expect) { ++bad; break; }
            }
        }
        if (bad) { std::printf("FAIL: window table corrupted (%d/%zu)\n", bad, g_windows.size()); ++fails; }
        // 每个格子都必须有若干窗口覆盖；且最大索引 360 必须出现
        bool hasMax = false;
        for (size_t w = 0; w < g_windows.size(); ++w)
            for (int k = 0; k < 6; ++k) if ((int)g_windows[w].c[k] == CELLS - 1) hasMax = true;
        if (!hasMax) { std::printf("FAIL: window table missing last cell (index truncation?)\n"); ++fails; }
    }

    Board B; B.init();
    for (int c = 5; c <= 9; ++c) B.place_raw(9 * N + c, 1);
    if (!would_win(B, 9 * N + 4, 1)) { std::printf("FAIL: winpoint-left\n"); ++fails; }
    if (!would_win(B, 9 * N + 10, 1)) { std::printf("FAIL: winpoint-right\n"); ++fails; }
    B.place_raw(9 * N + 4, 1);
    if (!just_won(B, 9 * N + 4, 1)) { std::printf("FAIL: just_won\n"); ++fails; }
    B.remove_raw(9 * N + 4);
    if (evaluate(B, 1) < 50000) { std::printf("FAIL: eval-five got %d\n", evaluate(B, 1)); ++fails; }

    // 精确防御测试：单条「五连」(对手5子，两头空) → 两个立即致胜点，k=2 可完全覆盖
    {
        Board G; G.init();
        for (int c = 0; c <= 4; ++c) G.place_raw(0 * N + c, 2);   // 对手五连
        std::vector<std::vector<int>> sol; exact_defense(G, 2, 2, sol);
        if (sol.empty()) { std::printf("FAIL: exact_defense single-five should have sol\n"); ++fails; }
        else {
            std::vector<int> emp; std::vector<std::vector<int>> thr; threat_empties(G, 2, emp, thr);
            for (auto& s : sol) if (!covers_all(s, thr)) { std::printf("FAIL: exact_defense sol not covering\n"); ++fails; break; }
        }
    }
    // 两条「五连」(不相交) → 需 3 个堵点 > k=2 → 必败，应返回无解（保守）
    {
        Board G; G.init();
        for (int c = 0; c <= 4; ++c) G.place_raw(0 * N + c, 2);
        for (int c = 10; c <= 14; ++c) G.place_raw(5 * N + c, 2);
        std::vector<std::vector<int>> sol; exact_defense(G, 2, 2, sol);
        if (!sol.empty()) { std::printf("FAIL: exact_defense two-five should be unsolvable (needs 3 blocks)\n"); ++fails; }
    }
    // 单一 4+2 威胁（对手 4 子在窗口，2 空）→ k=2 应能用一个空点挡住
    {
        Board G; G.init();
        for (int c = 0; c <= 3; ++c) G.place_raw(0 * N + c, 2);
        std::vector<std::vector<int>> sol; exact_defense(G, 2, 2, sol);
        std::vector<int> emp; std::vector<std::vector<int>> thr; threat_empties(G, 2, emp, thr);
        bool ok = false;
        for (auto& s : sol) if (covers_all(s, thr)) ok = true;
        if (!ok) { std::printf("FAIL: exact_defense 4+2 no covering sol\n"); ++fails; }
    }

    // ==== hot[] 增量维护一致性：随机放/撤子，与暴力统计逐轮比对 ====
    {
        Board R; R.init();
        std::srand(7);
        bool bad = false;
        for (int step = 0; step < 600 && !bad; ++step) {
            int idx = std::rand() % CELLS;
            if (!R.cell[idx]) R.place_raw(idx, 1 + (std::rand() & 1));
            else R.remove_raw(idx);
            int hb = 0, hw = 0;
            for (size_t w = 0; w < g_windows.size(); ++w) {
                int hs = Board::hot_side(R.w_own[w], R.w_opp[w]);
                if (hs == 1) ++hb; else if (hs == 2) ++hw;
            }
            if (hb != R.hot[1] || hw != R.hot[2]) {
                std::printf("FAIL: hot mismatch step=%d brute=%d/%d inc=%d/%d\n",
                            step, hb, hw, R.hot[1], R.hot[2]);
                ++fails; bad = true;
            }
        }
    }

    // ==== 回归：白守「黑竖四连」（实战中曾返回 (0,0) 角落的硬 bug）====
    {
        Board G; G.init();
        for (int r = 9; r <= 12; ++r) G.place_raw(r * N + 9, 1);   // 黑竖四连
        G.place_raw(13 * N + 10, 1);
        G.place_raw(10 * N + 10, 2); G.place_raw(8 * N + 8, 2);
        G.place_raw(9 * N + 12, 2);  G.place_raw(9 * N + 11, 2);
        std::vector<int> emp; std::vector<std::vector<int>> thr;
        threat_empties(G, 1, emp, thr);
        if (thr.empty()) { std::printf("FAIL: black vertical four not detected\n"); ++fails; }
        Decision d = decide(G, 2, 2, 500);
        bool onThreat = (std::find(emp.begin(), emp.end(), d.a) != emp.end());
        if (!onThreat) {
            std::printf("FAIL: white ignored black vertical four (a=%d,%d)\n", d.a / N, d.a % N);
            ++fails;
        }
        Board H = G; H.place_raw(d.a, 2);
        if (d.b >= 0 && d.b != d.a && !H.cell[d.b]) H.place_raw(d.b, 2);
        if (H.hot[1] > 0) { std::printf("FAIL: black still has hand-win after white block\n"); ++fails; }
    }

    // ==== 回归：「对手四连必须堵」（用户实测反馈的致命漏洞）====
    // 白在 row9 中间四连 (5..8)：一手 2 子可补成 6，属立即必杀。
    {
        Board G; G.init();
        for (int c = 5; c <= 8; ++c) G.place_raw(9 * N + c, 2);
        G.place_raw(2 * N + 2, 1);
        if (!opp_has_immwin(G, 1)) { std::printf("FAIL: 4-in-row not flagged imm-win\n"); ++fails; }
        if (G.hot[2] <= 0)         { std::printf("FAIL: hot[2] should be >0\n"); ++fails; }
        // k=2：判「两子联合是否真正解除威胁」（才是正确判据；单看首子会误判）
        Decision d2 = decide(G, 1, 2, 500);
        {
            Board H = G;
            H.place_raw(d2.a, 1);
            if (d2.b >= 0 && d2.b != d2.a && !H.cell[d2.b]) H.place_raw(d2.b, 1);
            if (H.hot[2] > 0) {
                std::printf("FAIL: k=2 did not neutralize 4-in-row (a=%d b=%d)\n", d2.a, d2.b);
                ++fails;
            }
        }
        // k=1：单子无法完全解除中间四连，但必须落在威胁空点上（最大化阻断）
        Decision d1 = decide(G, 1, 1, 300);
        {
            std::vector<int> emp; std::vector<std::vector<int>> thr;
            threat_empties(G, 2, emp, thr);
            if (std::find(emp.begin(), emp.end(), d1.a) == emp.end()) {
                std::printf("FAIL: k=1 not on any threat point (a=%d)\n", d1.a); ++fails;
            }
        }
    }
    // ==== 回归：我方 4+2 必须立即取胜（多得分）====
    {
        Board G; G.init();
        for (int c = 5; c <= 8; ++c) G.place_raw(9 * N + c, 1);
        G.place_raw(0 * N + 0, 2);
        Decision d = decide(G, 1, 2, 500);
        G.place_raw(d.a, 1);
        bool w = just_won(G, d.a, 1);
        if (!w) { G.place_raw(d.b, 1); w = just_won(G, d.b, 1); }
        if (!w) { std::printf("FAIL: own 4+2 not won (a=%d b=%d)\n", d.a, d.b); ++fails; }
    }

    // ==== 迫着（threat）系统 ====
    {
        // 单四 (9,4)-(9,7)：两个端点都要堵 → 2 个迫着
        Board G; G.init();
        for (int c = 4; c <= 7; ++c) G.place_raw(9 * N + c, 1);
        if (threat_count(G, 1) != 2) { std::printf("FAIL: four should be 2 threats (got %d)\n", threat_count(G, 1)); ++fails; }
        // 单五 (9,4)-(9,8)：两端仍要堵 → 也是 2 个迫着（一手正好挡住）
        Board G5; G5.init();
        for (int c = 4; c <= 8; ++c) G5.place_raw(9 * N + c, 1);
        if (threat_count(G5, 1) != 2) { std::printf("FAIL: five should be 2 threats (got %d)\n", threat_count(G5, 1)); ++fails; }
        // 断四 (9,4)(9,5)(9,6)(9,8)：只要补中间的空 (9,7) → 1 个迫着
        Board G4; G4.init();
        for (int c : {4, 5, 6, 8}) G4.place_raw(9 * N + c, 1);
        if (threat_count(G4, 1) != 1) { std::printf("FAIL: broken four should be 1 threat (got %d)\n", threat_count(G4, 1)); ++fails; }
        // 双四（两行各一个四）→ 4 个迫着 > 2，理论必败
        Board G6; G6.init();
        for (int c = 5; c <= 8; ++c) { G6.place_raw(9 * N + c, 1); G6.place_raw(12 * N + c, 1); }
        if (threat_count(G6, 1) < 3) { std::printf("FAIL: double four should be >=3 threats (got %d)\n", threat_count(G6, 1)); ++fails; }
        // 空地 → 0
        Board G0; G0.init();
        if (threat_count(G0, 1) != 0) { std::printf("FAIL: empty board should be 0 threats\n"); ++fails; }
    }
    // ==== 双迫着制造点（活三 / 双四前身）与「必须抢占」====
    {
        Board G; G.init();
        for (int c = 4; c <= 6; ++c) G.place_raw(9 * N + c, 1);      // 黑横向三
        for (int r = 6; r <= 8; ++r) G.place_raw(r * N + 7, 1);      // 黑纵向三
        G.place_raw(2 * N + 2, 2); G.place_raw(3 * N + 3, 2);
        std::vector<int> dt, tc;
        double_threat_points(G, 1, dt, &tc);
        int key = 9 * N + 7;                                         // 两线的交点
        int kidx = -1;
        for (size_t i = 0; i < dt.size(); ++i) if (dt[i] == key) kidx = (int)i;
        if (kidx < 0) { std::printf("FAIL: double-four point (9,7) not found\n"); ++fails; }
        else if (tc[kidx] < 3) { std::printf("FAIL: (9,7) should yield >=3 threats (got %d)\n", tc[kidx]); ++fails; }
        // 白必须占掉它（两子合力）
        Decision d = decide(G, 2, 2, 800);
        if (!(d.a == key || d.b == key)) {
            std::printf("FAIL: did not block double-four point (a=%d b=%d)\n", d.a, d.b); ++fails;
        }
    }


    std::srand(1);
    Board G; G.init();
    int cur = 1, moves = 0, over = 0;
    while (!over && moves < 60) {
        int k = (moves == 0 && cur == 1) ? 1 : 2;
        if (cur == 1) {
            Decision d = decide(G, 1, k, 3);
            if (G.cell[d.a] != 0) { std::printf("FAIL: bot illegal a\n"); ++fails; break; }
            if (apply_move(G, Move{ d.a, d.b }, 1, k)) over = 1;
        } else {
            for (int s = 0; s < k; ++s) {
                int tries = 0, e = -1;
                while (tries++ < 1000) { e = std::rand() % CELLS; if (!G.cell[e]) break; }
                if (e < 0 || G.cell[e]) continue;
                G.place_raw(e, 2);
                if (just_won(G, e, 2)) { over = 1; break; }
            }
        }
        cur = 3 - cur; ++moves;
    }

#if USE_CTSS
    // CTSS 专项 1：黑活四 → 应证明必胜且着法合法且立即获胜
    {
        Board G2; G2.init();
        for (int c = 4; c <= 7; ++c) G2.place_raw(9 * N + c, 1);
        G2.place_raw(3 * N + 3, 2); G2.place_raw(15 * N + 15, 2);
        int ca, cb; bool proven = ctss_find(G2, 1, ca, cb, 3000);
        if (!proven) { std::printf("FAIL: ctss open-four not proven\n"); ++fails; }
        else if (ca < 0 || cb < 0 || ca == cb || G2.cell[ca] != 0 || G2.cell[cb] != 0) { std::printf("FAIL: ctss illegal\n"); ++fails; }
        else { Board T = G2; T.place_raw(ca, 1); T.place_raw(cb, 1);
            if (!(just_won(T, ca, 1) || just_won(T, cb, 1))) { std::printf("FAIL: ctss not winning\n"); ++fails; } }
    }
    // CTSS 专项 2：白活四，黑无杀 → 不应假判黑胜
    {
        Board G3; G3.init();
        for (int c = 4; c <= 7; ++c) G3.place_raw(5 * N + c, 2);
        int ca, cb; bool proven = ctss_find(G3, 1, ca, cb, 3000);
        if (proven) {
            Board T = G3; T.place_raw(ca, 1);
            bool aW = just_won(T, ca, 1);
            if (!aW) { T.place_raw(cb, 1); if (!(just_won(T, cb, 1))) { std::printf("FAIL: ctss false-win\n"); ++fails; } }
        }
    }
#endif

    if (fails == 0) std::printf("SELFTEST PASS (windows=%zu, selfplay moves=%d, over=%d)\n",
                                g_windows.size(), moves, over);
    else std::printf("SELFTEST FAIL (%d)\n", fails);
    return fails ? 1 : 0;
}

// ============================================================================
//  做局自检工具（随交付物一起编译，不依赖外部脚本）
//
//  --trap diag     : 读 stdin 的一条请求，打印该局面的做局诊断
//                    （tc / hot / 杀点对 / 覆盖数 / 唯一解 / 引擎会选的做局手）
//  --trap verify a b: 读一条请求，然后在「落下 a,b」之后**穷举对手全部 2 子应手**，
//                    逐条检查是否真的存在存活的杀点对。这是「做局不是假杀」的
//                    唯一硬证据 —— cover>=3 时任何应手都应该留活口。
// ============================================================================
static bool parse_board_req(const std::string& raw, Board& B, int& me, int& k) {
    JParser p; p.s = raw.c_str(); JVal req = p.parse();
    const JVal* obs = req.get("observation");
    if (!obs) return false;
    const JVal* bv = obs->get("board");
    if (!bv || (int)bv->arr.size() != N) return false;
    me = (int)jnum(obs->get("current_player"), 1.0);
    if (me != 1 && me != 2) me = 1;
    int reqd = (int)jnum(obs->get("required_stones_this_turn"), 2.0);
    k = (reqd == 1) ? 1 : 2;
    uint8_t src[CELLS];
    for (int r = 0; r < N; ++r) {
        if ((int)bv->arr[r].arr.size() != N) return false;
        for (int c = 0; c < N; ++c) {
            double v = bv->arr[r].arr[c].num;
            src[r * N + c] = (uint8_t)((v == 1 || v == 2) ? (int)v : 0);
        }
    }
    B.load(src);
    return true;
}

static int trap_diag_tool(bool verify, int va, int vb) {
    static char buf[200000];
    while (std::fgets(buf, sizeof(buf), stdin)) {
        std::string raw(buf);
        if (raw.find('{') == std::string::npos) continue;
        Board B; int me = 1, k = 2;
        if (!parse_board_req(raw, B, me, k)) { std::printf("{\"error\":\"bad_request\"}\n"); continue; }
        int oppSide = 3 - me;
        std::vector<int> pts;
        kill_graph_points(B, me, pts, 36);
        std::vector<KillPair> K;
        collect_kill_pairs(B, me, K, pts, 96, 0);
        std::vector<std::pair<int,int>> c2;
        int c1 = -1;
        int cover0 = kill_cover_number(K, &c2, &c1);
        std::printf("{\"tcMe\":%d,\"tcOpp\":%d,\"hotMe\":%d,\"hotOpp\":%d,"
                    "\"kpBefore\":%d,\"coverBefore\":%d,\"nPoints\":%d",
                    threat_count(B, me), threat_count(B, oppSide),
                    B.hot[me], B.hot[oppSide], (int)K.size(), cover0, (int)pts.size());

        if (!verify) {
            TrapInfo best; Move mv{-1, -1};
            bool ok = trap_best(B, me, k, mv, best, 3000);
            std::printf(",\"trapPick\":%s,\"pickA\":[%d,%d],\"pickCover\":%d,\"pickKp\":%d,\"pickUniq\":%d,\"pickRate\":%d",
                        ok ? "true" : "false",
                        ok ? mv.a / N : -1, ok ? mv.a % N : -1,
                        best.cover, best.kp, best.uniq, best.rate);
            if (ok) std::printf(",\"pickB\":[%d,%d]", mv.b / N, mv.b % N);
            std::printf("}\n");
            std::fflush(stdout);
            continue;
        }
        // ---- 穷举验证：落下 (va,vb) 之后，做局是不是**真的**锁死 ----
        //
        // ★ 判据必须包含「对手的落子会隔空废掉我方的四连窗口」这一条。
        //   只查「杀点对的两格没被占」是不够的：杀点对 (c,d) 之所以成立，靠的
        //   是我们已有的 4 颗子 + 2 空点组成窗口；对手在自己那一手落下的子，
        //   完全可能正好落进那个窗口的空点里，把这条威胁整条废掉。
        //   漏掉这一条就会得到「看似锁死、其实一步就被拆」的假必胜 ——
        //   引擎会按「我要赢了」乱走，几步之内被反杀。
        //   所以每次都要**真正把子摆上去重算 tc**，而不是查格子空不空。
        if (va < 0 || vb < 0 || B.cell[va] || B.cell[vb] || va == vb) {
            std::printf(",\"verify\":\"bad_move\"}\n"); std::fflush(stdout); continue;
        }
        B.place_raw(va, me); B.place_raw(vb, me);
        std::vector<int> pts2;
        kill_graph_points(B, me, pts2, 36);
        std::vector<KillPair> K2;
        collect_kill_pairs(B, me, K2, pts2, 96, 0);
        std::vector<std::pair<int,int>> c22;
        int cover2 = kill_cover_number(K2, &c22, nullptr);
        long long replies = 0, escapes = 0, checks = 0;
        int firstEscA = -1, firstEscB = -1;
        for (int x = 0; x < CELLS; ++x) {
            if (B.cell[x]) continue;
            for (int y = x + 1; y < CELLS; ++y) {
                if (B.cell[y]) continue;
                ++replies;
                B.place_raw(x, 3 - me);
                B.place_raw(y, 3 - me);
                // 对手自己做出 hot、或自己做出 3 迫着 → 他都比我们先成 6
                bool alive = (B.hot[3 - me] == 0) && (threat_count(B, 3 - me) < 3);
                for (const auto& e : K2) {
                    if (!alive) break;
                    if (e.c == x || e.d == x || e.c == y || e.d == y) continue;
                    B.place_raw(e.c, me);
                    B.place_raw(e.d, me);
                    ++checks;
                    if (threat_count(B, me) >= 3) alive = true;
                    B.remove_raw(e.d);
                    B.remove_raw(e.c);
                    if (alive) break;
                }
                B.remove_raw(y);
                B.remove_raw(x);
                if (!alive) {
                    ++escapes;
                    if (firstEscA < 0) { firstEscA = x; firstEscB = y; }
                }
            }
        }
        long long okRate = replies ? (replies - escapes) * 1000 / replies : 0;
        std::printf(",\"afterKp\":%d,\"afterCover\":%d,\"replies\":%lld,\"escapes\":%lld,"
                    "\"okRate\":%lld.%lld%%,\"tcChecks\":%lld",
                    (int)K2.size(), cover2, replies, escapes,
                    okRate / 10, okRate % 10, checks);
        if (firstEscA >= 0)
            std::printf(",\"firstEscape\":[[%d,%d],[%d,%d]]",
                        firstEscA / N, firstEscA % N, firstEscB / N, firstEscB % N);
        std::printf(",\"locked\":%s}\n", (escapes == 0 && !K2.empty()) ? "true" : "false");
        std::fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    init_windows(); init_zob();
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--selftest") == 0) return selftest();
        if (std::strcmp(argv[i], "--trap") == 0) {
            // --trap diag | --trap verify <a_idx> <b_idx>
            if (i + 1 < argc && std::strcmp(argv[i + 1], "verify") == 0 && i + 3 < argc)
                return trap_diag_tool(true, std::atoi(argv[i + 2]), std::atoi(argv[i + 3]));
            return trap_diag_tool(false, -1, -1);
        }
    }
    return run_bot();
}
