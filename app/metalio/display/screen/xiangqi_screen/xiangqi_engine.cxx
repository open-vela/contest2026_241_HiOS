// SPDX-License-Identifier: Apache-2.0
#include "xiangqi_engine.h"
#include <string.h>

namespace Xiangqi {
namespace {
int Abs(int v) { return v < 0 ? -v : v; }
bool Palace(int row, int col, int side) {
    return col >= 3 && col <= 5 && (side > 0 ? row >= 7 : row <= 2);
}
int Between(const Position &p, int from, int to) {
    int step = from / 9 == to / 9 ? (to > from ? 1 : -1) : (to > from ? 9 : -9);
    int count = 0;
    for (int i = from + step; i != to; i += step) if (p.board[i]) ++count;
    return count;
}
bool Pseudo(const Position &p, Move m) {
    if (m.from >= 90 || m.to >= 90 || m.from == m.to) return false;
    int piece = p.board[m.from], side = Side(piece);
    if (!piece || Side(p.board[m.to]) == side) return false;
    int r = m.from / 9, c = m.from % 9, tr = m.to / 9, tc = m.to % 9;
    int dr = tr - r, dc = tc - c, ar = Abs(dr), ac = Abs(dc);
    switch (Abs(piece)) {
    case King:
        if (!dc && p.board[m.to] == -side * King && !Between(p, m.from, m.to)) return true;
        return ar + ac == 1 && Palace(tr, tc, side);
    case Advisor: return ar == 1 && ac == 1 && Palace(tr, tc, side);
    case Elephant:
        return ar == 2 && ac == 2 && (side > 0 ? tr >= 5 : tr <= 4) &&
               !p.board[(r + dr / 2) * 9 + c + dc / 2];
    case Horse:
        if (ar == 2 && ac == 1) return !p.board[(r + dr / 2) * 9 + c];
        if (ar == 1 && ac == 2) return !p.board[r * 9 + c + dc / 2];
        return false;
    case Rook: return (!dr || !dc) && !Between(p, m.from, m.to);
    case Cannon:
        return (!dr || !dc) && Between(p, m.from, m.to) == (p.board[m.to] ? 1 : 0);
    case Pawn:
        return (!dc && dr == -side) || (!dr && ac == 1 && (side > 0 ? r <= 4 : r >= 5));
    default: return false;
    }
}
Position Applied(const Position &p, Move m) {
    Position next = p;
    next.board[m.to] = next.board[m.from];
    next.board[m.from] = Empty;
    next.turn = -p.turn;
    return next;
}
int Evaluate(const Position &p, int side) {
    const int values[] = {0, 20000, 120, 120, 300, 600, 320, 70};
    int score = 0;
    for (int i = 0; i < 90; ++i) {
        int piece = p.board[i], type = Abs(piece), owner = Side(piece);
        if (!piece) continue;
        int value = values[type];
        if (type == Pawn) {
            int advance = owner > 0 ? 9 - i / 9 : i / 9;
            value += advance * 7 + (advance >= 5 ? 45 : 0);
        }
        if (type == Horse || type == Cannon) value += (4 - Abs(i % 9 - 4)) * 4;
        score += owner == side ? value : -value;
    }
    return score;
}
}
int Side(int piece) { return piece > 0 ? 1 : piece < 0 ? -1 : 0; }
bool InCheck(const Position &p, int side) {
    int king = -1;
    for (int i = 0; i < 90; ++i) if (p.board[i] == side * King) { king = i; break; }
    if (king < 0) return true;
    for (int i = 0; i < 90; ++i)
        if (Side(p.board[i]) == -side && Pseudo(p, Move{(uint8_t)i, (uint8_t)king})) return true;
    return false;
}
const char *InvalidReason(const Position&p,Move m){
 if(m.from>=90||m.to>=90)return "超出棋盘";
 int piece=p.board[m.from],dr=(int)m.to/9-(int)m.from/9,dc=(int)m.to%9-(int)m.from%9;
 if(Abs(piece)==Horse&&((Abs(dr)==2&&Abs(dc)==1)||(Abs(dr)==1&&Abs(dc)==2))){int leg=Abs(dr)==2?m.from+dr/2*9:m.from+dc/2;if(p.board[leg])return "马腿被挡 · 先移开紧邻马的棋子";}
 if(Pseudo(p,m)&&InCheck(Applied(p,m),p.turn))return "此步会使己方被将军 · 请应将";
 return "此处不可落子 · 请走到绿色提示处";
}
bool Legal(const Position &p, Move m) {
    return m.from < 90 && Side(p.board[m.from]) == p.turn && Pseudo(p, m) &&
           !InCheck(Applied(p, m), p.turn);
}
bool HasMove(const Position &p) {
    for (int i = 0; i < 90; ++i) if (Side(p.board[i]) == p.turn)
        for (int j = 0; j < 90; ++j) if (Legal(p, Move{(uint8_t)i, (uint8_t)j})) return true;
    return false;
}
bool BestMove(const Position &p, Move &out, Difficulty level) {
    Search search; search.Begin(p, level);
    while (!search.Step()) {}
    return search.move(out);
}
void Search::Push(const Position &p, int depth, int alpha, int beta, Move incoming) {
    Frame &f = stack_[++top_];
    f.pos=p; f.depth=depth; f.alpha=alpha; f.beta=beta;
    f.best=-90000; f.cursor=0; f.found=false; f.incoming=incoming; f.count=f.next=0;
}
void Search::Begin(const Position &p, Difficulty level) {
    initial_=p; top_=-1; iteration_=2; completed_depth_=0;
    target_=level==Advanced ? 5 : level==Intermediate ? 4 : 3;
    limit_=level==Advanced ? 48000000 : level==Intermediate ? 16000000 : 2000000;
    work_=0; found_=false;
    Push(p,iteration_,-90000,90000,Move{});
}
bool Search::Step() {
    // Explicit stack avoids recursion; each tick includes a bounded amount
    // of legal-move generation, so advanced search cannot freeze navigation.
    for (int budget=0; budget<(target_>=4 ? 16384 : 8192) && top_>=0; ++budget,++work_) {
        if (completed_depth_>=3 && work_>=limit_) { top_=-1; return true; }
        Frame &f=stack_[top_];
        bool finish=f.depth==0 || (f.cursor>=8100 && f.next>=f.count) || f.alpha>=f.beta;
        if (finish) {
            int score=f.depth==0 ? Evaluate(f.pos,f.pos.turn)-(InCheck(f.pos,f.pos.turn)?25:0)
                                    : f.found ? f.best : -80000+top_;
            Move incoming=f.incoming, chosen=f.best_move;
            bool valid=f.found;
            if (top_==0) {
                if (valid) { move_=chosen; found_=true; }
                completed_depth_=iteration_; top_=-1;
                if (!valid || iteration_>=target_) return true;
                ++iteration_; Push(initial_,iteration_,-90000,90000,Move{});
                continue;
            }
            --top_; Frame &parent=stack_[top_]; score=-score;
            if (!parent.found || score>parent.best) {parent.best=score;parent.best_move=incoming;}
            parent.found=true; if(score>parent.alpha)parent.alpha=score;
            continue;
        }
        if(f.cursor<8100) {
            int from=f.cursor/90,to=f.cursor%90;
            if (Side(f.pos.board[from])!=f.pos.turn) { f.cursor=(from+1)*90; continue; }
            ++f.cursor; Move m{(uint8_t)from,(uint8_t)to};
            if (!Legal(f.pos,m)) continue;
            const int values[]={0,20000,120,120,300,600,320,70};
            int priority=f.pos.board[to] ? 1000+values[Abs(f.pos.board[to])]*10-values[Abs(f.pos.board[from])] : 0;
            if(top_==0 && completed_depth_>=2 && m.from==move_.from && m.to==move_.to)priority=30000;
            if(priority>29000 && priority!=30000)priority=29000;
            if(f.count<256) {
                int slot=f.count++;
                while(slot>0 && priority>f.priorities[slot-1]) {
                    f.moves[slot]=f.moves[slot-1]; f.priorities[slot]=f.priorities[slot-1]; --slot;
                }
                f.moves[slot]=m; f.priorities[slot]=(int16_t)priority;
            }
            continue;
        }
        Move m=f.moves[f.next++]; Position child=Applied(f.pos,m);
        Push(child,f.depth-1,-f.beta,-f.alpha,m);

    }
    return top_<0;
}
void Game::Reset() {
    memset(&pos_, 0, sizeof(pos_));
    const int back[] = {Rook, Horse, Elephant, Advisor, King, Advisor, Elephant, Horse, Rook};
    for (int c = 0; c < 9; ++c) { pos_.board[c] = -back[c]; pos_.board[81 + c] = back[c]; }
    pos_.board[19] = pos_.board[25] = -Cannon;
    pos_.board[64] = pos_.board[70] = Cannon;
    for (int c = 0; c < 9; c += 2) { pos_.board[27 + c] = -Pawn; pos_.board[54 + c] = Pawn; }
    pos_.turn = 1; used_ = quiet_ = 0; plies_ = 0; result_ = Playing;
}
bool Game::Play(Move m) {
    if (result_ != Playing || !Legal(pos_, m)) return false;
    if (used_ == 256) { memmove(history_, history_ + 1, 255 * sizeof(Record)); --used_; }
    int captured = pos_.board[m.to];
    history_[used_++] = Record{m, (int8_t)captured, quiet_};
    quiet_ = captured ? 0 : quiet_ + 1;
    pos_ = Applied(pos_, m); ++plies_;
    if (!HasMove(pos_)) result_ = pos_.turn > 0 ? BlackWins : RedWins;
    else if (quiet_ >= 120) result_ = Draw;
    return true;
}
bool Game::Undo() {
    if (!used_) return false;
    Record rec = history_[--used_];
    pos_.board[rec.move.from] = pos_.board[rec.move.to];
    pos_.board[rec.move.to] = rec.captured;
    pos_.turn = -pos_.turn; quiet_ = rec.quiet; --plies_; result_ = Playing;
    return true;
}
bool Game::last_move(Move &m) const {
    if (!used_) return false;
    m = history_[used_ - 1].move; return true;
}
}
