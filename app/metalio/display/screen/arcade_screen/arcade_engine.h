#pragma once
#include <cstdint>
namespace Arcade {
constexpr int kTankCampaignFinalStage = 12;
struct Link {
    int cells[8][10]{}; int remaining=48, score=0;
    void reset(); bool path(int a,int b) const; bool pair(int a,int b);
    bool hint(int &a,int &b) const; void shuffle();
};
struct Match {
 int cells[8][8]{},score=0,moves=24,level=1;
 void reset(int stage=1); bool canSwap(int a,int b) const; bool swap(int a,int b); bool hint(int&a,int&b)const;void shuffle(); int resolve();
};
struct Tank {
    struct Unit {int x=0,y=0,dir=2,hp=0;};
    struct Shot {int x=0,y=0,dir=0;bool enemy=false,active=false;};
    int map[12][14]{}; Unit player,enemies[4]; Shot shots[20];
    int lives=3,score=0,level=1,bossHp=0,kills=0,spawned=0,tick=0,shield=0;
    bool lost=false,won=false;
    void reset(int stage=1); void step(int direction); void fire(const Unit&,bool);
    bool blocked(int x,int y) const; bool move(Unit&,int,bool);
};
}
