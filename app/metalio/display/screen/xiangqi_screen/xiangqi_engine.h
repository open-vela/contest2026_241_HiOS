// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

namespace Xiangqi {
enum Piece { Empty, King, Advisor, Elephant, Horse, Rook, Cannon, Pawn };
enum Result { Playing, RedWins, BlackWins, Draw };
struct Move { uint8_t from, to; };
struct Position {
    int8_t board[90]; // Positive = red; row 0 is black's back rank.
    int8_t turn;
};
int Side(int piece);
bool InCheck(const Position &p, int side);
const char *InvalidReason(const Position &p, Move m);
bool Legal(const Position &p, Move m);
bool HasMove(const Position &p);
enum Difficulty { Beginner, Intermediate, Advanced };
bool BestMove(const Position &p, Move &out, Difficulty level = Beginner);
class Search {
public:
    void Begin(const Position &p, Difficulty level = Beginner);
    bool Step(); // Bounded work per GUI tick, including inner search nodes.
    bool move(Move &out) const { out = move_; return found_; }
    int completed_depth() const { return completed_depth_; }
private:
    struct Frame {
        Position pos;
        int cursor, depth, alpha, beta, best;
        bool found;
        Move incoming, best_move;
        Move moves[256];
        int16_t priorities[256];
        int count, next;
    };
    void Push(const Position &p, int depth, int alpha, int beta, Move incoming);
    Frame stack_[6]{};
    Position initial_{};
    int top_ = -1, target_ = 2, iteration_ = 2, completed_depth_ = 0;
    uint32_t work_ = 0, limit_ = 0;
    bool found_ = false;
    Move move_{};
};
class Game {
public:
    Game() { Reset(); }
    void Reset();
    bool Play(Move m);
    bool Undo();
    const Position &position() const { return pos_; }
    Result result() const { return result_; }
    int move_count() const { return plies_; }
    bool last_move(Move &m) const;
private:
    struct Record { Move move; int8_t captured; uint16_t quiet; };
    Position pos_;
    Record history_[256]; // Fixed RAM; retain the most recent 256 half-moves.
    uint16_t used_, quiet_;
    int plies_;
    Result result_;
};
}
