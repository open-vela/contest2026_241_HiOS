#include "arcade_engine.h"
#include <cstdlib>
#include <initializer_list>
#include <cstring>
namespace Arcade {
// NuttX's float-based rand() loses low bits at INT_MAX; modulo then
// produces visibly repetitive boards. Keep game randomness in integer space.
static uint32_t Random() {
 static uint32_t state = static_cast<uint32_t>(std::rand()) ^ 0x9e3779b9u;
 if (!state) state = 0x6d2b79f5u;
 state ^= state << 13; state ^= state >> 17; state ^= state << 5;
 return state;
}
static const int dx[]={0,1,0,-1},dy[]={-1,0,1,0};
void Link::reset(){std::memset(cells,0,sizeof(cells));remaining=48;score=0;int n=0;for(int y=1;y<=6;y++)for(int x=1;x<=8;x++)cells[y][x]=(n++/2)%8+1;shuffle();}
bool Link::path(int a,int b)const{
 int ax=a%8+1,ay=a/8+1,bx=b%8+1,by=b/8+1;
 if(a==b||!cells[ay][ax]||cells[ay][ax]!=cells[by][bx])return false;
 int best[8][10][4];for(auto &row:best)for(auto &cell:row)for(int &v:cell)v=3;
 for(int d=0;d<4;d++)best[ay][ax][d]=0;
 // Bellman relaxation over a tiny padded board avoids unbounded queues.
 for(int pass=0;pass<80;pass++){bool changed=false;for(int y=0;y<8;y++)for(int x=0;x<10;x++)for(int d=0;d<4;d++)if(best[y][x][d]<=2)for(int nd=0;nd<4;nd++){
 int nx=x+dx[nd],ny=y+dy[nd],turns=best[y][x][d]+(d!=nd);
 if(nx<0||nx>=10||ny<0||ny>=8||turns>2)continue;
 if(nx==bx&&ny==by)return true;
 if(cells[ny][nx]||turns>=best[ny][nx][nd])continue;
 best[ny][nx][nd]=turns;changed=true;
 }if(!changed)break;}return false;
}
bool Link::pair(int a,int b){if(!path(a,b))return false;cells[a/8+1][a%8+1]=cells[b/8+1][b%8+1]=0;remaining-=2;score+=10;return true;}
bool Link::hint(int&a,int&b)const{for(a=0;a<48;a++)for(b=a+1;b<48;b++)if(path(a,b))return true;return false;}
void Link::shuffle(){int values[48],n=0;for(int i=0;i<48;i++)if(cells[i/8+1][i%8+1])values[n++]=cells[i/8+1][i%8+1];for(int i=n-1;i>0;i--){int j=Random()%(i+1),v=values[i];values[i]=values[j];values[j]=v;}int k=0;for(int i=0;i<48;i++)if(cells[i/8+1][i%8+1])cells[i/8+1][i%8+1]=values[k++];int a,b;if(n&& !hint(a,b)){// Put a pair on the first two occupied cells in the topmost row/column boundary.
 int first=-1,second=-1;for(int i=0;i<48;i++)if(cells[i/8+1][i%8+1]){if(first<0)first=i;else{second=i;break;}}
 if(second>=0){int v=cells[first/8+1][first%8+1];for(int i=second;i<48;i++)if(cells[i/8+1][i%8+1]==v){int &p=cells[second/8+1][second%8+1],&q=cells[i/8+1][i%8+1];int t=p;p=q;q=t;break;}}
 }}
bool Tank::blocked(int x,int y)const{return x<0||y<0||x>=14||y>=12||map[y][x]==1||map[y][x]==2||map[y][x]==3||map[y][x]==5;}
bool Tank::move(Unit&u,int d,bool enemy){u.dir=d;int x=u.x+dx[d],y=u.y+dy[d];if(blocked(x,y))return false;if(enemy&&player.hp&&player.x==x&&player.y==y)return false;for(const auto&e:enemies)if(&e!=&u&&e.hp&&e.x==x&&e.y==y)return false;u.x=x;u.y=y;return true;}
void Tank::reset(int stage){std::memset(map,0,sizeof(map));for(auto&e:enemies)e=Unit{};for(auto&s:shots)s=Shot{};level=stage;lives=3;score=0;kills=spawned=tick=0;lost=won=false;shield=30;player={6,10,0,1};
 for(int y=2;y<9;y+=2)for(int x=1;x<13;x++)if(x%4!=2)map[y][x]=1;
 // Twelve handcrafted-feeling layouts rotate through distinct obstacle
 // patterns, while the campaign ramps enemy count and durability each stage.
 for(int y=2;y<9;y++)for(int x=1;x<13;x++) {
   switch ((stage - 1) % 6) {
   case 0: break; // classic brick lanes
   case 1: map[y][x]=(x%3==0&&y%3!=0)?1:0; break;
   case 2: map[y][x]=(y==4&&x!=6)?3:0; break;
   case 3: map[y][x]=(x%3==0&&y%2==0)?2:(y%3==0?4:0); break;
   case 4: map[y][x]=(x+y)%5==0?1:((x+y)%7==0?2:0); break;
   default: map[y][x]=((x==3||x==10)&&(y==3||y==5||y==7))?1:
                        ((y==5&&x>4&&x<9)?3:0); break;
   }
 }
 const bool boss_stage = stage >= kTankCampaignFinalStage;
 bossHp=boss_stage?30:0;if(boss_stage)enemies[0]={6,0,2,bossHp};
 map[4][3]=map[4][10]=2;map[6][5]=map[6][8]=3;map[8][3]=map[8][10]=4;map[11][7]=5;map[10][7]=map[10][8]=map[11][6]=map[11][8]=1;
}
void Tank::fire(const Unit&u,bool enemy){for(auto&s:shots)if(!s.active){s={u.x,u.y,u.dir,enemy,true};break;}}
void Tank::step(int direction){if(lost||won)return;tick++;if(shield)shield--;if(direction>=0&&tick%2==0)move(player,direction,false);if(tick%6==0)fire(player,false);
 int target=6+level*2;
 if(level<kTankCampaignFinalStage&&tick%16==1&&spawned<target)for(auto&e:enemies)if(!e.hp){int x=(spawned%3)*6;if(!blocked(x,0)){bool occupied=false;for(auto&o:enemies)if(o.hp&&o.x==x&&o.y==0)occupied=true;if(!occupied){e={x,0,2,(level>=7||spawned%3==2?2:1)};spawned++;}}break;}
 for(auto&e:enemies)if(e.hp){if(tick%5==0){int d=Random()%5==0?Random()%4:e.dir;if(!move(e,d,true))e.dir=Random()%4;}if(tick%12==0)fire(e,true);}
 for(auto&s:shots)if(s.active){s.x+=dx[s.dir];s.y+=dy[s.dir];if(s.x<0||s.y<0||s.x>=14||s.y>=12){s.active=false;continue;}int&t=map[s.y][s.x];if(t==1){t=0;s.active=false;continue;}if(t==2){s.active=false;continue;}if(t==5){if(s.enemy)lost=true;s.active=false;continue;}
 if(s.enemy&&s.x==player.x&&s.y==player.y){s.active=false;if(!shield){lives--;if(lives<=0)lost=true;else{player={6,10,0,1};shield=30;}}}
 if(!s.enemy)for(auto&e:enemies)if(e.hp&&e.x==s.x&&e.y==s.y){s.active=false;if(--e.hp==0){kills++;score+=100;}if(level>=kTankCampaignFinalStage)bossHp=e.hp;break;}}
 won=level>=kTankCampaignFinalStage?bossHp<=0:kills>=target;
}
}

namespace Arcade {
static void matches(const Match&m,bool out[8][8]){for(int y=0;y<8;y++)for(int x=0;x<8;x++){if(x<=5&&m.cells[y][x]&&m.cells[y][x]==m.cells[y][x+1]&&m.cells[y][x]==m.cells[y][x+2]){int i=x;while(i<8&&m.cells[y][i]==m.cells[y][x])out[y][i++]=true;}if(y<=5&&m.cells[y][x]&&m.cells[y][x]==m.cells[y+1][x]&&m.cells[y][x]==m.cells[y+2][x]){int i=y;while(i<8&&m.cells[i][x]==m.cells[y][x])out[i++][x]=true;}}}
void Match::reset(int stage){level=stage;score=0;moves=24;for(int y=0;y<8;y++)for(int x=0;x<8;x++){do{cells[y][x]=1+Random()%6;}while((x>=2&&cells[y][x]==cells[y][x-1]&&cells[y][x]==cells[y][x-2])||(y>=2&&cells[y][x]==cells[y-1][x]&&cells[y][x]==cells[y-2][x]));}int a,b;if(!hint(a,b))shuffle();}
int Match::resolve(){int total=0;for(int chain=1;chain<=32;chain++){bool marked[8][8]{};matches(*this,marked);int count=0;for(int y=0;y<8;y++)for(int x=0;x<8;x++)if(marked[y][x]){cells[y][x]=0;count++;}if(!count)break;total+=count*10*chain;for(int x=0;x<8;x++){int dest=7;for(int y=7;y>=0;y--)if(cells[y][x])cells[dest--][x]=cells[y][x];while(dest>=0)cells[dest--][x]=1+Random()%6;}}score+=total;return total;}
bool Match::hint(int&a,int&b)const{for(a=0;a<64;a++)for(int d: {1,8}){b=a+d;if(b>=64||(d==1&&a/8!=b/8))continue;Match copy=*this;int t=copy.cells[a/8][a%8];copy.cells[a/8][a%8]=copy.cells[b/8][b%8];copy.cells[b/8][b%8]=t;bool marked[8][8]{};matches(copy,marked);for(auto&r:marked)for(bool v:r)if(v)return true;}return false;}
void Match::shuffle(){int stage=level,s=score,m=moves;reset(stage);score=s;moves=m;}
bool Match::canSwap(int a,int b)const{if(moves<=0||a<0||b<0||a>=64||b>=64||!((a/8==b/8&&std::abs(a-b)==1)||std::abs(a-b)==8))return false;Match copy=*this;int&t=copy.cells[a/8][a%8],&u=copy.cells[b/8][b%8];int v=t;t=u;u=v;bool marked[8][8]{};matches(copy,marked);for(auto&r:marked)for(bool m:r)if(m)return true;return false;}
bool Match::swap(int a,int b){if(!canSwap(a,b))return false;int&t=cells[a/8][a%8],&u=cells[b/8][b%8];int v=t;t=u;u=v;moves--;resolve();int x,y;if(!hint(x,y))shuffle();return true;}
}
