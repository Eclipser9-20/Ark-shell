// ════════════════════════════════════════════════════════════════════════
//  PISTIN — Visual Studio × VS Code, from scratch, in the terminal.
//  v0.2 — autocomplete · auto-close brackets · Command Palette · mouse
//  Pure C++20 + raw termios + ANSI truecolor. No ncurses, no dependencies.
//      clang++ -std=c++20 -O2 -o pistin src/main.cpp   (or: make)
// ════════════════════════════════════════════════════════════════════════
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <csignal>

namespace fs = std::filesystem;

// ── color ─────────────────────────────────────────────────────────────
struct RGB { int r, g, b;
  bool operator==(const RGB& o) const { return r==o.r&&g==o.g&&b==o.b; }
  bool operator!=(const RGB& o) const { return !(*this==o); } };

// Switchable theme (default TokyoNight, to match the terminal; VS Dark available).
struct Theme { RGB bg,bg_panel,bg_menu,bg_tab,bg_curln,bg_sel,bg_pop,blue_bar,bar_fg,
                   fg,fg_dim,fg_bright,kw,ctl,type,func,str,num,cmt,pre,folder; };
static Theme TOKYO(){ return Theme{
  {26,27,38},{31,35,53},{22,22,30},{26,27,38},{31,35,53},{40,52,87},{31,35,53},{122,162,247},{26,27,38},
  {192,202,245},{86,95,137},{255,255,255},{187,154,247},{187,154,247},{42,195,222},{122,162,247},
  {158,206,106},{255,158,100},{86,95,137},{125,207,255},{122,162,247} }; }
static Theme VSDARK(){ return Theme{
  {30,30,30},{37,37,38},{45,45,48},{30,30,30},{42,42,42},{38,79,120},{37,37,38},{0,122,204},{255,255,255},
  {212,212,212},{133,133,133},{255,255,255},{86,156,214},{197,134,192},{78,201,176},{220,220,170},
  {206,145,120},{181,206,168},{106,153,85},{155,155,155},{220,220,170} }; }
static Theme th = TOKYO();

static int u8len(unsigned char c) {
  if (c<0x80) return 1; if ((c>>5)==0x6) return 2; if ((c>>4)==0xE) return 3; if ((c>>3)==0x1E) return 4; return 1;
}
static bool ci_starts(const std::string& w, const std::string& p) {
  if (w.size() < p.size()) return false;
  for (size_t i=0;i<p.size();++i) if (tolower((unsigned char)w[i])!=tolower((unsigned char)p[i])) return false;
  return true;
}

// ── screen buffer (truecolor, flicker-free) ───────────────────────────
struct Cell { std::string ch=" "; RGB fg=th.fg; RGB bg=th.bg; bool bold=false; };
struct Screen {
  int w=80, h=24; std::vector<Cell> cells;
  void resize(int W,int H){ w=W;h=H;cells.assign(w*h,Cell{}); }
  Cell& at(int x,int y){ return cells[y*w+x]; }
  bool in(int x,int y){ return x>=0&&x<w&&y>=0&&y<h; }
  void clear(RGB b){ for(auto&c:cells) c=Cell{" ",th.fg,b,false}; }
  void fill(int x,int y,int ww,int hh,RGB b){
    for(int j=y;j<y+hh;++j) for(int i=x;i<x+ww;++i)
      if(in(i,j)){auto&c=at(i,j);c.ch=" ";c.bg=b;c.fg=th.fg;c.bold=false;} }
  void putcell(int x,int y,const std::string&g,RGB fg,RGB bg,bool bold=false){
    if(in(x,y)){auto&c=at(x,y);c.ch=g;c.fg=fg;c.bg=bg;c.bold=bold;} }
  int text(int x,int y,const std::string&s,RGB fg,RGB bg,bool bold=false,int maxx=-1){
    if(maxx<0)maxx=w; int i=0;
    while(i<(int)s.size()&&x<maxx){int n=u8len((unsigned char)s[i]);putcell(x,y,s.substr(i,n),fg,bg,bold);i+=n;x++;}
    return x; }
  std::string render(){
    std::string o; o.reserve(w*h*16); o+="\x1b[?2026h\x1b[H";
    for(int y=0;y<h;++y){ o+="\x1b["+std::to_string(y+1)+";1H";
      RGB cf{-1,-1,-1},cb{-1,-1,-1}; bool cbold=false;
      for(int x=0;x<w;++x){ Cell&c=at(x,y);
        if(c.bold!=cbold){o+=c.bold?"\x1b[1m":"\x1b[22m";cbold=c.bold;cf={-1,-1,-1};cb={-1,-1,-1};}
        if(c.fg!=cf){o+="\x1b[38;2;"+std::to_string(c.fg.r)+";"+std::to_string(c.fg.g)+";"+std::to_string(c.fg.b)+"m";cf=c.fg;}
        if(c.bg!=cb){o+="\x1b[48;2;"+std::to_string(c.bg.r)+";"+std::to_string(c.bg.g)+";"+std::to_string(c.bg.b)+"m";cb=c.bg;}
        o+=c.ch; }
      o+="\x1b[0m"; }
    o+="\x1b[?2026l"; return o; }
};

// ── terminal ──────────────────────────────────────────────────────────
struct Terminal {
  termios orig{};
  void enter(){ tcgetattr(STDIN_FILENO,&orig); termios r=orig;
    r.c_lflag&=~(ECHO|ICANON|IEXTEN|ISIG); r.c_iflag&=~(IXON|ICRNL|BRKINT|INPCK|ISTRIP);
    r.c_oflag&=~OPOST; r.c_cflag|=CS8; r.c_cc[VMIN]=1; r.c_cc[VTIME]=0;
    tcsetattr(STDIN_FILENO,TCSAFLUSH,&r);
    const char* s="\x1b[?1049h\x1b[?25l\x1b[?1000h\x1b[?1002h\x1b[?1006h\x1b[?2004h"; write(STDOUT_FILENO,s,strlen(s)); }
  void leave(){ const char* s="\x1b[?2004l\x1b[?1000l\x1b[?1002l\x1b[?1006l\x1b[?25h\x1b[?1049l";
    write(STDOUT_FILENO,s,strlen(s)); tcsetattr(STDIN_FILENO,TCSAFLUSH,&orig); }
  void size(int&w,int&h){ winsize ws{}; if(ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws)==0&&ws.ws_col){w=ws.ws_col;h=ws.ws_row;} else {w=80;h=24;} }
};

// ── input ─────────────────────────────────────────────────────────────
enum class K { None,Char,Enter,Back,Tab,Up,Down,Left,Right,Esc,PgUp,PgDn,Home,End,
               CtrlQ,CtrlS,CtrlP,CtrlB,CtrlSpace,CtrlC,CtrlX,CtrlV,CtrlZ,CtrlY,CtrlA,CtrlF,Paste,Mouse };
enum class MB { None,Left,WheelUp,WheelDown };
struct Event { K key=K::None; std::string ch; MB mb=MB::None; int mx=0,my=0; bool mdown=false,mmotion=false,shift=false; };

static std::string g_inbuf;
static Event readEvent(){
  Event e;
  if(g_inbuf.empty()){ char b[256]; int n=read(STDIN_FILENO,b,sizeof b); if(n<=0)return e; g_inbuf.assign(b,b+n); }
  auto eat=[&](int k){ g_inbuf.erase(0,k); };
  unsigned char c=(unsigned char)g_inbuf[0];
  if(c=='\x1b'){
    if(g_inbuf.size()<2){ e.key=K::Esc; eat(1); return e; }
    if(g_inbuf[1]=='['||g_inbuf[1]=='O'){
      if(g_inbuf.compare(0,6,"\x1b[200~")==0){                       // bracketed paste
        size_t pe=g_inbuf.find("\x1b[201~",6);
        e.key=K::Paste; e.ch=(pe==std::string::npos)?g_inbuf.substr(6):g_inbuf.substr(6,pe-6);
        eat(pe==std::string::npos?g_inbuf.size():pe+6); return e; }
      if(g_inbuf.size()>=3 && g_inbuf[2]=='<'){                      // SGR mouse
        size_t end=3; while(end<g_inbuf.size()&&g_inbuf[end]!='M'&&g_inbuf[end]!='m')++end;
        if(end<g_inbuf.size()){ int b=0,x=1,y=1; sscanf(g_inbuf.c_str()+3,"%d;%d;%d",&b,&x,&y);
          e.key=K::Mouse; e.mx=x-1; e.my=y-1; e.mdown=(g_inbuf[end]=='M'); e.mmotion=(b&32);
          e.mb=(b==64)?MB::WheelUp:(b==65)?MB::WheelDown:((b&3)==0?MB::Left:MB::None); eat(end+1); return e; }
        e.key=K::Esc; eat(1); return e; }
      size_t i=2; while(i<g_inbuf.size() && !(g_inbuf[i]>=0x40 && g_inbuf[i]<=0x7e)) ++i;
      if(i>=g_inbuf.size()){ e.key=K::Esc; eat(1); return e; }       // incomplete sequence
      char fin=g_inbuf[i]; std::string par=g_inbuf.substr(2,i-2);
      int p[4]={0,0,0,0},pc=0,v=0; bool any=false;
      for(char ch:par){ if(ch>='0'&&ch<='9'){v=v*10+(ch-'0');any=true;} else if(ch==';'){if(pc<4)p[pc++]=v;v=0;any=false;} }
      if(any&&pc<4)p[pc++]=v;
      if(p[1]>0)e.shift=((p[1]-1)&1);
      auto modkey=[&](int code,int mods){ bool ctrl=(mods>0)&&((mods-1)&4);   // CtrlShiftP etc.
        if(ctrl){ int lc=tolower(code);
          if(lc=='p')e.key=K::CtrlP; else if(lc=='b')e.key=K::CtrlB; else if(lc=='s')e.key=K::CtrlS;
          else if(lc=='q')e.key=K::CtrlQ; else if(lc=='c')e.key=K::CtrlC; else if(lc=='x')e.key=K::CtrlX;
          else if(lc=='v')e.key=K::CtrlV; else if(lc=='z')e.key=K::CtrlZ; else if(lc=='y')e.key=K::CtrlY;
          else if(lc=='a')e.key=K::CtrlA; else if(lc=='f')e.key=K::CtrlF; else if(code==32)e.key=K::CtrlSpace; else e.key=K::None; }
        else if(code>=32&&code<127){ e.key=K::Char; e.ch=std::string(1,(char)code); } };
      switch(fin){
        case 'A':e.key=K::Up;break; case 'B':e.key=K::Down;break; case 'C':e.key=K::Right;break; case 'D':e.key=K::Left;break;
        case 'H':e.key=K::Home;break; case 'F':e.key=K::End;break;
        case 'u': modkey(p[0],p[1]); break;                          // CSI-u : code;mods u
        case '~': if(p[0]==27) modkey(p[2],p[1]);                    // modifyOtherKeys : 27;mods;code ~
                  else if(p[0]==5)e.key=K::PgUp; else if(p[0]==6)e.key=K::PgDn;
                  else if(p[0]==1||p[0]==7)e.key=K::Home; else if(p[0]==4||p[0]==8)e.key=K::End; break;
        default: e.key=K::None; break;
      }
      eat(i+1); return e;
    }
    e.key=K::Esc; eat(1); return e;
  }
  if(c==0){ e.key=K::CtrlSpace; eat(1); return e; }
  if(c==2){ e.key=K::CtrlB; eat(1); return e; }
  if(c==16){ e.key=K::CtrlP; eat(1); return e; }
  if(c==17){ e.key=K::CtrlQ; eat(1); return e; }
  if(c==19){ e.key=K::CtrlS; eat(1); return e; }
  if(c==1){ e.key=K::CtrlA; eat(1); return e; }
  if(c==3){ e.key=K::CtrlC; eat(1); return e; }
  if(c==6){ e.key=K::CtrlF; eat(1); return e; }
  if(c==22){ e.key=K::CtrlV; eat(1); return e; }
  if(c==24){ e.key=K::CtrlX; eat(1); return e; }
  if(c==25){ e.key=K::CtrlY; eat(1); return e; }
  if(c==26){ e.key=K::CtrlZ; eat(1); return e; }
  if(c==13||c==10){ e.key=K::Enter; eat(1); return e; }
  if(c==127||c==8){ e.key=K::Back; eat(1); return e; }
  if(c==9){ e.key=K::Tab; eat(1); return e; }
  if(c>=32){ int len=std::min(u8len(c),(int)g_inbuf.size()); e.key=K::Char; e.ch=g_inbuf.substr(0,len); eat(len); return e; }
  eat(1); return e;
}

// ── syntax highlighter ────────────────────────────────────────────────
struct Span { std::string text; RGB fg; };
static const char* KW[]={"int","char","void","bool","float","double","long","short","unsigned","signed",
  "const","static","struct","class","enum","union","template","typename","namespace","using","public",
  "private","protected","virtual","auto","constexpr","inline","extern","new","delete","true","false",
  "nullptr","sizeof","this","operator","friend","mutable","volatile","typedef","include","define"};
static const char* CTL[]={"if","else","for","while","do","switch","case","default","break","continue",
  "return","goto","try","catch","throw"};
static bool iskw(const std::string&w){ for(auto k:KW)if(w==k)return true; return false; }
static bool isctl(const std::string&w){ for(auto k:CTL)if(w==k)return true; return false; }
static std::vector<Span> highlight(const std::string&s,bool&inBlock){
  std::vector<Span> out; int i=0,n=s.size();
  auto push=[&](const std::string&t,RGB c){ if(!t.empty())out.push_back({t,c}); };
  while(i<n){
    if(inBlock){ int st=i; while(i<n){if(s[i]=='*'&&i+1<n&&s[i+1]=='/'){i+=2;inBlock=false;break;}i++;} push(s.substr(st,i-st),th.cmt); continue; }
    char c=s[i];
    if(c=='/'&&i+1<n&&s[i+1]=='/'){ push(s.substr(i),th.cmt); break; }
    if(c=='/'&&i+1<n&&s[i+1]=='*'){ inBlock=true;int st=i;i+=2; while(i<n){if(s[i]=='*'&&i+1<n&&s[i+1]=='/'){i+=2;inBlock=false;break;}i++;} push(s.substr(st,i-st),th.cmt); continue; }
    if(c=='#'){ push(s.substr(i),th.pre); break; }
    if(c=='"'||c=='\''){ char q=c;int st=i++; while(i<n){if(s[i]=='\\'){i+=2;continue;}if(s[i]==q){i++;break;}i++;} push(s.substr(st,i-st),th.str); continue; }
    if(isdigit((unsigned char)c)){ int st=i; while(i<n&&(isalnum((unsigned char)s[i])||s[i]=='.'||s[i]=='x'))i++; push(s.substr(st,i-st),th.num); continue; }
    if(isalpha((unsigned char)c)||c=='_'){ int st=i; while(i<n&&(isalnum((unsigned char)s[i])||s[i]=='_'))i++;
      std::string w=s.substr(st,i-st); RGB col=isctl(w)?th.ctl:iskw(w)?th.kw:(i<n&&s[i]=='(')?th.func:th.fg;
      if(!w.empty()&&isupper((unsigned char)w[0])&&col==th.fg)col=th.type; push(w,col); continue; }
    int st=i++; push(s.substr(st,1),th.fg);
  }
  return out;
}

// ── editor (auto-close, smart enter, indent) ──────────────────────────
struct Editor {
  std::vector<std::string> lines{""}; int cx=0,cy=0,rowoff=0,coloff=0;
  std::string path; bool dirty=false, autoclose=true;
  int ax=0,ay=0; bool selA=false;                     // selection anchor
  struct Snap{ std::vector<std::string> lines; int cx,cy; };
  std::vector<Snap> undoS,redoS;
  void load(const std::string&p){ path=p; lines.clear(); std::ifstream f(p); std::string ln;
    while(std::getline(f,ln)){ if(!ln.empty()&&ln.back()=='\r')ln.pop_back(); lines.push_back(ln); }
    if(lines.empty())lines.push_back(""); cx=cy=rowoff=coloff=0; dirty=false; }
  void save(){ if(path.empty())return; std::ofstream f(path);
    for(size_t i=0;i<lines.size();++i){f<<lines[i];if(i+1<lines.size())f<<"\n";} dirty=false; }
  std::string& cur(){ return lines[cy]; }
  int len(){ return cur().size(); }
  static char closer(char o){ switch(o){case '(':return ')';case '[':return ']';case '{':return '}';case '"':return '"';case '\'':return '\'';} return 0; }
  static std::string indentOf(const std::string&s){ size_t i=0; while(i<s.size()&&(s[i]==' '||s[i]=='\t'))i++; return s.substr(0,i); }
  void insert(const std::string&g){
    int p=std::min(len(),cx); char nx=p<len()?cur()[p]:0;
    if(g.size()==1&&autoclose){ char ch=g[0]; char cl=closer(ch);
      if((ch==')'||ch==']'||ch=='}'||ch=='"'||ch=='\'')&&nx==ch){ cx++; return; } // type over
      if(cl&&ch!='"'&&ch!='\''){ cur().insert(p,std::string()+ch+cl); cx++; dirty=true; return; }
      if(cl&&(ch=='"'||ch=='\'')){ bool word=(p>0&&(isalnum((unsigned char)cur()[p-1])||cur()[p-1]=='_'));
        if(!word){ cur().insert(p,std::string()+ch+cl); cx++; dirty=true; return; } }
    }
    cur().insert(p,g); cx+=g.size(); dirty=true;
  }
  void newline(){ int p=std::min(len(),cx); std::string ind=indentOf(cur());
    char bf=p>0?cur()[p-1]:0, af=p<len()?cur()[p]:0; std::string rest=cur().substr(p); cur().erase(p);
    if(bf=='{'&&af=='}'){ std::string inner=ind+"    ";
      lines.insert(lines.begin()+cy+1,ind+rest); lines.insert(lines.begin()+cy+1,inner);
      cy++; cx=inner.size(); dirty=true; return; }
    lines.insert(lines.begin()+cy+1,ind+rest); cy++; cx=ind.size(); dirty=true; }
  void back(){ if(cx>0){ char a=cur()[cx-1],b=cx<len()?cur()[cx]:0;
      if(autoclose&&((a=='('&&b==')')||(a=='['&&b==']')||(a=='{'&&b=='}')||(a=='"'&&b=='"')||(a=='\''&&b=='\''))){ cur().erase(cx-1,2); cx--; dirty=true; return; }
      cur().erase(cx-1,1); cx--; dirty=true; }
    else if(cy>0){ cx=lines[cy-1].size(); lines[cy-1]+=cur(); lines.erase(lines.begin()+cy); cy--; dirty=true; } }
  void indent(){ cur().insert(std::min(len(),cx),"    "); cx+=4; dirty=true; }
  void clampx(){ cx=std::max(0,std::min(cx,len())); }
  void move(int dx,int dy){ cy=std::max(0,std::min(cy+dy,(int)lines.size()-1));
    if(dx<0){if(cx>0)cx--;else if(cy>0){cy--;cx=len();}} else if(dx>0){if(cx<len())cx++;else if(cy<(int)lines.size()-1){cy++;cx=0;}} clampx(); }
  void clampY(){ if(cy>=(int)lines.size())cy=(int)lines.size()-1; if(cy<0)cy=0; clampx(); }
  void snapshot(){ undoS.push_back({lines,cx,cy}); if(undoS.size()>400)undoS.erase(undoS.begin()); redoS.clear(); }
  void undo(){ if(undoS.empty())return; redoS.push_back({lines,cx,cy}); auto s=undoS.back();undoS.pop_back(); lines=s.lines;cx=s.cx;cy=s.cy; selA=false; clampY(); dirty=true; }
  void redo(){ if(redoS.empty())return; undoS.push_back({lines,cx,cy}); auto s=redoS.back();redoS.pop_back(); lines=s.lines;cx=s.cx;cy=s.cy; selA=false; clampY(); dirty=true; }
  void selStart(){ ax=cx;ay=cy;selA=true; }
  void selClear(){ selA=false; }
  bool hasSel(){ return selA&&!(ax==cx&&ay==cy); }
  void selOrder(int&sy,int&sx,int&ey,int&ex){ if(ay<cy||(ay==cy&&ax<=cx)){sy=ay;sx=ax;ey=cy;ex=cx;}else{sy=cy;sx=cx;ey=ay;ex=ax;} }
  std::string selText(){ if(!hasSel())return ""; int sy,sx,ey,ex;selOrder(sy,sx,ey,ex);
    if(sy==ey)return lines[sy].substr(sx,ex-sx); std::string r=lines[sy].substr(sx);
    for(int i=sy+1;i<ey;++i)r+="\n"+lines[i]; r+="\n"+lines[ey].substr(0,ex); return r; }
  void selDelete(){ if(!hasSel())return; int sy,sx,ey,ex;selOrder(sy,sx,ey,ex);
    if(sy==ey)lines[sy].erase(sx,ex-sx); else{ std::string tail=lines[ey].substr(ex); lines[sy]=lines[sy].substr(0,sx)+tail; lines.erase(lines.begin()+sy+1,lines.begin()+ey+1); }
    cy=sy;cx=sx;selA=false;dirty=true; }
  void selectAll(){ ay=0;ax=0;cy=(int)lines.size()-1;cx=(int)lines[cy].size();selA=true; }
  void insertText(const std::string&s){ for(char c:s){ if(c=='\n'){ std::string rest=cur().substr(std::min(len(),cx)); cur().erase(std::min(len(),cx)); lines.insert(lines.begin()+cy+1,rest); cy++;cx=0; } else if(c!='\r'){ cur().insert(std::min(len(),cx),std::string(1,c)); cx++; } } dirty=true; }
};

// ── solution explorer ─────────────────────────────────────────────────
struct Tree {
  fs::path root; std::vector<fs::directory_entry> items; int sel=0,off=0;
  void load(const fs::path&r){ std::error_code ec; root=fs::canonical(r,ec); if(ec)root=r; items.clear();
    for(auto&e:fs::directory_iterator(root,ec)){ auto nm=e.path().filename().string(); if(!nm.empty()&&nm[0]=='.')continue; items.push_back(e); }
    std::sort(items.begin(),items.end(),[](auto&a,auto&b){ bool da=a.is_directory(),db=b.is_directory(); if(da!=db)return da>db; return a.path().filename().string()<b.path().filename().string(); });
    sel=0;off=0; }
};
static std::string iconFor(const fs::directory_entry&e){
  if(e.is_directory())return ""; std::string x=e.path().extension().string();
  if(x==".cpp"||x==".cc"||x==".cxx"||x==".hpp")return ""; if(x==".c"||x==".h")return "";
  if(x==".py")return ""; if(x==".md")return ""; if(x==".sh")return ""; if(x==".json")return ""; if(x==".vim")return ""; return "";
}

// ── autocomplete + command palette state ──────────────────────────────
struct Comp { bool active=false; std::string prefix; std::vector<std::string> items; int sel=0,off=0; };
struct Cmd  { std::string name; std::function<void()> run; };
struct Palette { bool active=false; std::string query; std::vector<int> match; int sel=0; };

// ── the IDE ───────────────────────────────────────────────────────────
struct App {
  Screen scr; Terminal term; Editor ed; Tree tree;
  Comp comp; Palette pal; std::vector<Cmd> cmds;
  int focus=0; bool running=true, sidebar=true, mouseOn=true;
  std::string status="Ready";
  std::string clip; bool selecting=false; char lastOp=0;
  struct MItem{ std::string label,hint; std::function<void()> run; };
  struct Menu{ std::string title; std::vector<MItem> items; int x=0,w=0; };
  std::vector<Menu> menus; int menuOpen=-1, menuSel=0;
  bool findActive=false; std::string findQ;
  int LedX0,LedX1,LedY0,LedY1,LtrX0,LtreeW,Lgut;          // layout (for mouse)
  volatile static sig_atomic_t resized;

  void open(const fs::path&p){ ed.load(p.string()); focus=0; comp.active=false; status="Opened "+p.filename().string(); }
  void enableMouse(){ mouseOn=true; write(STDOUT_FILENO,"\x1b[?1000h\x1b[?1006h",16); status="Mouse enabled — click to place caret / pick files"; }
  void disableMouse(){ mouseOn=false; write(STDOUT_FILENO,"\x1b[?1000l\x1b[?1006l",16); status="Keyboard mode"; }

  void build(){ if(ed.path.empty()){status="No file to build";return;} ed.save();
    std::string cmd="clang++ -std=c++20 -fsyntax-only \""+ed.path+"\" 2>&1"; FILE*p=popen(cmd.c_str(),"r");
    if(!p){status="build: popen failed";return;} char buf[256]; while(fread(buf,1,sizeof buf,p)>0){}
    int rc=pclose(p); status=(rc==0)?"Build succeeded ✓":"Build FAILED — fix errors"; }

  void buildCommands(){ cmds={
    {"StartMouse",            [this]{ enableMouse(); }},
    {"StartKeyboard",         [this]{ disableMouse(); }},
    {"File: Save",            [this]{ ed.save(); status="Saved"; }},
    {"File: Quit",            [this]{ running=false; }},
    {"Build: Build File",     [this]{ build(); }},
    {"View: Toggle Sidebar",  [this]{ sidebar=!sidebar; }},
    {"View: Focus Explorer",  [this]{ sidebar=true; focus=1; }},
    {"Edit: Toggle Auto-Close",[this]{ ed.autoclose=!ed.autoclose; status=ed.autoclose?"Auto-close ON":"Auto-close OFF"; }},
    {"Theme: TokyoNight",     [this]{ th=TOKYO();  status="Theme: TokyoNight (matches the terminal)"; }},
    {"Theme: Visual Studio",  [this]{ th=VSDARK(); status="Theme: Visual Studio Dark"; }},
    {"Help: About Pistin",    [this]{ status="Pistin — Visual Studio x VS Code, from scratch. v0.2"; }},
  }; }

  void buildMenus(){ menus={
    {"File",{{"New File","",[this]{ ed=Editor(); status="New file"; }},{"Open Folder","",[this]{ sidebar=true; focus=1; }},
             {"Save","Ctrl+S",[this]{ ed.save(); status="Saved"; }},{"Quit","Ctrl+Q",[this]{ running=false; }}}},
    {"Edit",{{"Undo","Ctrl+Z",[this]{ ed.undo(); }},{"Redo","Ctrl+Y",[this]{ ed.redo(); }},{"Cut","Ctrl+X",[this]{ doCut(); }},
             {"Copy","Ctrl+C",[this]{ doCopy(); }},{"Paste","Ctrl+V",[this]{ doPaste(); }},{"Select All","Ctrl+A",[this]{ ed.selectAll(); }},
             {"Find","Ctrl+F",[this]{ findActive=true; findQ=""; }}}},
    {"View",{{"Toggle Sidebar","Ctrl+B",[this]{ sidebar=!sidebar; }},{"Command Palette","C-S-P",[this]{ openPalette(); }},
             {"Focus Explorer","",[this]{ sidebar=true; focus=1; }}}},
    {"Build",{{"Build File","F7",[this]{ build(); }},{"Toggle Auto-Close","",[this]{ ed.autoclose=!ed.autoclose; }}}},
    {"Theme",{{"TokyoNight","",[]{ th=TOKYO(); }},{"Visual Studio","",[]{ th=VSDARK(); }}}},
    {"Help",{{"About Pistin","",[this]{ status="Pistin v0.3 — VS x VS Code, from scratch"; }}}},
  }; }
  void openMenu(int i){ int n=(int)menus.size(); menuOpen=((i%n)+n)%n; menuSel=0; }
  void runMenuItem(){ if(menuOpen>=0&&menuSel<(int)menus[menuOpen].items.size()){ auto fn=menus[menuOpen].items[menuSel].run; menuOpen=-1; fn(); } }

  // clipboard: internal + OSC 52 to the SYSTEM clipboard (Ghostty supports it)
  void osc52(const std::string&s){ static const char* B="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o; unsigned val=0; int bits=0; for(unsigned char c:s){ val=(val<<8)|c; bits+=8; while(bits>=6){o+=B[(val>>(bits-6))&63];bits-=6;} }
    if(bits)o+=B[(val<<(6-bits))&63]; while(o.size()%4)o+='='; std::string seq="\x1b]52;c;"+o+"\x07"; write(STDOUT_FILENO,seq.data(),seq.size()); }
  void doCopy(){ if(!ed.hasSel())return; clip=ed.selText(); osc52(clip); status="Copied"; }
  void doCut(){ if(!ed.hasSel())return; clip=ed.selText(); osc52(clip); ed.snapshot(); ed.selDelete(); status="Cut"; lastOp='m'; }
  void doPaste(){ if(clip.empty())return; ed.snapshot(); if(ed.hasSel())ed.selDelete(); ed.insertText(clip); status="Pasted"; lastOp='m'; }
  void findNext(){ if(findQ.empty())return; int L=(int)ed.lines.size();
    for(int d=0;d<=L;++d){ int li=(ed.cy+d)%L; const std::string&s=ed.lines[li]; size_t from=(d==0)?(size_t)(ed.cx+1):0;
      size_t pos=(from<=s.size())?s.find(findQ,from):std::string::npos;
      if(pos!=std::string::npos){ ed.ay=li;ed.ax=(int)pos;ed.cy=li;ed.cx=(int)pos+(int)findQ.size();ed.selA=true; status="Found '"+findQ+"'"; return; } }
    status="No match: "+findQ; }

  // ── autocomplete ──
  std::vector<std::string> gatherWords(){
    std::set<std::string> s; for(auto k:KW)s.insert(k); for(auto k:CTL)s.insert(k);
    s.insert("std"); s.insert("string"); s.insert("vector"); s.insert("size"); s.insert("printf"); s.insert("cout"); s.insert("endl"); s.insert("include");
    for(auto&ln:ed.lines){ int i=0,n=ln.size(); while(i<n){ if(isalpha((unsigned char)ln[i])||ln[i]=='_'){ int st=i; while(i<n&&(isalnum((unsigned char)ln[i])||ln[i]=='_'))i++; if(i-st>=3)s.insert(ln.substr(st,i-st)); } else i++; } }
    return std::vector<std::string>(s.begin(),s.end());
  }
  void updateComp(bool force=false){
    std::string&ln=ed.cur(); int i=ed.cx; while(i>0&&(isalnum((unsigned char)ln[i-1])||ln[i-1]=='_'))i--;
    std::string pre=ln.substr(i,ed.cx-i); int minlen=force?1:2;
    if((int)pre.size()<minlen){ comp.active=false; return; }
    comp.items.clear(); for(auto&w:gatherWords()) if(w.size()>pre.size()&&ci_starts(w,pre)) comp.items.push_back(w);
    comp.prefix=pre; comp.sel=0; comp.off=0; comp.active=!comp.items.empty();
  }
  void acceptComp(){ if(!comp.active)return; std::string add=comp.items[comp.sel].substr(comp.prefix.size());
    ed.cur().insert(ed.cx,add); ed.cx+=add.size(); ed.dirty=true; comp.active=false; }

  // ── command palette ──
  void openPalette(){ pal.active=true; pal.query=""; pal.sel=0; filterPalette(); }
  void filterPalette(){ pal.match.clear(); for(int i=0;i<(int)cmds.size();++i){
      std::string nm=cmds[i].name, q=pal.query; std::transform(nm.begin(),nm.end(),nm.begin(),::tolower); std::transform(q.begin(),q.end(),q.begin(),::tolower);
      if(q.empty()||nm.find(q)!=std::string::npos) pal.match.push_back(i); } if(pal.sel>=(int)pal.match.size())pal.sel=0; }
  void runPalette(){ if(pal.sel<(int)pal.match.size()){ pal.active=false; cmds[pal.match[pal.sel]].run(); } else pal.active=false; }

  // ── layout + draw ──
  void layout(){ int W=scr.w,H=scr.h; LtreeW=sidebar?std::min(34,W/4):0;
    LedX0=0; LedX1=W-LtreeW-1; LedY0=2; LedY1=H-2; LtrX0=W-LtreeW; Lgut=6; }

  void draw(){
    scr.clear(th.bg); int W=scr.w,H=scr.h; layout();
    // menu bar (clickable titles; positions recorded for hit-testing)
    scr.fill(0,0,W,1,th.bg_menu);
    int mxp=2; scr.text(mxp,0,"Pistin",th.kw,th.bg_menu,true); mxp+=8;
    for(int i=0;i<(int)menus.size();++i){ auto&m=menus[i]; m.x=mxp; m.w=(int)m.title.size()+2;
      RGB mb=(i==menuOpen)?th.bg_sel:th.bg_menu; scr.fill(mxp,0,m.w,1,mb); scr.text(mxp+1,0,m.title,th.fg,mb); mxp+=m.w; }
    scr.text(W-14,0,"Ctrl+Shift+P",th.fg_dim,th.bg_menu);
    // tab strip
    scr.fill(0,1,W,1,th.bg_panel);
    { std::string nm=ed.path.empty()?"untitled":fs::path(ed.path).filename().string();
      std::string tab="  "+nm+(ed.dirty?" ●  ":"   "); scr.fill(0,1,(int)tab.size()+2,1,th.bg_tab); scr.text(0,1,tab,th.fg,th.bg_tab); }
    // editor
    scr.fill(LedX0,LedY0,LedX1-LedX0+1,LedY1-LedY0+1,th.bg);
    int viewH=LedY1-LedY0+1, viewW=LedX1-LedX0+1-Lgut;
    if(ed.cy<ed.rowoff)ed.rowoff=ed.cy; if(ed.cy>=ed.rowoff+viewH)ed.rowoff=ed.cy-viewH+1;
    if(ed.cx<ed.coloff)ed.coloff=ed.cx; if(ed.cx>=ed.coloff+viewW)ed.coloff=ed.cx-viewW+1;
    bool inBlock=false; for(int k=0;k<ed.rowoff&&k<(int)ed.lines.size();++k){bool ib=inBlock;highlight(ed.lines[k],ib);inBlock=ib;}
    for(int row=0;row<viewH;++row){ int li=ed.rowoff+row, sy=LedY0+row;
      if(li>=(int)ed.lines.size()){ scr.text(LedX0,sy,"~",th.fg_dim,th.bg); continue; }
      bool cline=(li==ed.cy); RGB lbg=cline?th.bg_curln:th.bg; scr.fill(LedX0,sy,LedX1-LedX0+1,1,lbg);
      char num[8]; snprintf(num,sizeof num,"%4d ",li+1); scr.text(LedX0,sy,num,cline?th.fg:th.fg_dim,lbg);
      bool ib=inBlock; auto spans=highlight(ed.lines[li],ib); int x=LedX0+Lgut,col=0;
      for(auto&sp:spans){ int i=0,nn=sp.text.size(); while(i<nn){int l=u8len((unsigned char)sp.text[i]); if(col>=ed.coloff&&x<=LedX1){scr.putcell(x,sy,sp.text.substr(i,l),sp.fg,lbg);x++;} i+=l;col++;} }
      inBlock=ib;
      if(ed.hasSel()){ int sy,sx,ey,ex; ed.selOrder(sy,sx,ey,ex);
        if(li>=sy&&li<=ey){ int cs=(li==sy)?sx:0, ce=(li==ey)?ex:(int)ed.lines[li].size()+1;
          for(int cc=cs;cc<ce;++cc){ int sx2=LedX0+Lgut+(cc-ed.coloff); if(sx2>=LedX0+Lgut&&sx2<=LedX1) scr.at(sx2,sy).bg=th.bg_sel; } } }
      if(focus==0&&li==ed.cy){ int cxs=LedX0+Lgut+(ed.cx-ed.coloff);
        if(cxs>=LedX0+Lgut&&cxs<=LedX1){ auto&cc=scr.at(std::min(cxs,W-1),sy); std::string g=cc.ch==" "?" ":cc.ch; scr.putcell(std::min(cxs,W-1),sy,g,th.bg,th.fg_bright); } }
    }
    // solution explorer
    if(sidebar){ scr.fill(LtrX0,0,LtreeW,H,th.bg_panel);
      scr.text(LtrX0+1,2," SOLUTION EXPLORER",th.fg_dim,th.bg_panel,true);
      scr.text(LtrX0+1,3,"  "+tree.root.filename().string(),th.folder,th.bg_panel,true);
      int tvH=H-6; if(tree.sel<tree.off)tree.off=tree.sel; if(tree.sel>=tree.off+tvH)tree.off=tree.sel-tvH+1;
      for(int r=0;r<tvH;++r){ int idx=tree.off+r, sy=4+r; if(idx>=(int)tree.items.size())break; auto&it=tree.items[idx];
        bool sd=(idx==tree.sel); RGB rbg=sd?(focus==1?th.bg_sel:th.bg_curln):th.bg_panel; scr.fill(LtrX0,sy,LtreeW,1,rbg);
        scr.text(LtrX0+2,sy,iconFor(it),it.is_directory()?th.folder:th.kw,rbg);
        scr.text(LtrX0+4,sy,it.path().filename().string(),it.is_directory()?th.folder:th.fg,rbg,false,LtrX0+LtreeW); } }
    // status bar
    scr.fill(0,H-1,W,1,th.blue_bar); char pos[80]; snprintf(pos,sizeof pos,"Ln %d, Col %d   %s   UTF-8",ed.cy+1,ed.cx+1,mouseOn?"Mouse":"Kbd");
    scr.text(1,H-1,"▶ PISTIN",th.bar_fg,th.blue_bar,true); scr.text(12,H-1,status,th.bar_fg,th.blue_bar,false,W-(int)strlen(pos)-3);
    scr.text(W-(int)strlen(pos)-2,H-1,pos,th.bar_fg,th.blue_bar);
    // autocomplete popup
    if(comp.active&&focus==0&&!pal.active){
      int px=LedX0+Lgut+(ed.cx-ed.coloff-comp.prefix.size()), py=LedY0+(ed.cy-ed.rowoff)+1;
      int pw=10; for(auto&s:comp.items)pw=std::max(pw,(int)s.size()+2); pw=std::min(pw,40);
      int ph=std::min((int)comp.items.size(),9); if(py+ph>=H-1)py=LedY0+(ed.cy-ed.rowoff)-ph; if(py<LedY0)py=LedY0;
      px=std::max(0,std::min(px,W-pw-1)); if(comp.sel<comp.off)comp.off=comp.sel; if(comp.sel>=comp.off+ph)comp.off=comp.sel-ph+1;
      for(int r=0;r<ph;++r){ int idx=comp.off+r; RGB bg=(idx==comp.sel)?th.bg_sel:th.bg_pop; scr.fill(px,py+r,pw,1,bg);
        scr.text(px,py+r,"  ",th.kw,bg); scr.text(px+1,py+r,"",th.kw,bg);
        scr.text(px+3,py+r,comp.items[idx],idx==comp.sel?th.fg_bright:th.fg,bg,false,px+pw); } }
    // command palette
    if(pal.active){
      int pw=std::min(72,W-8), px=(W-pw)/2, py=3;
      int ph=std::min((int)pal.match.size(),12)+2;
      scr.fill(px-1,py-1,pw+2,ph+2,th.bg_pop);
      scr.text(px,py,"  "+pal.query+"▏",th.fg_bright,th.bg_pop);
      scr.text(px,py," ",th.kw,th.bg_pop);
      for(int r=0;r<ph-1;++r){ int idx=r; if(idx>=(int)pal.match.size())break; int sy=py+1+r;
        RGB bg=(idx==pal.sel)?th.bg_sel:th.bg_pop; scr.fill(px-1,sy,pw+2,1,bg);
        scr.text(px+1,sy,cmds[pal.match[idx]].name,idx==pal.sel?th.fg_bright:th.fg,bg,false,px+pw); }
    }
    // dropdown menu (top layer)
    if(menuOpen>=0&&menuOpen<(int)menus.size()){ auto&m=menus[menuOpen]; int dw=16;
      for(auto&it:m.items)dw=std::max(dw,(int)it.label.size()+(int)it.hint.size()+5);
      int dx=std::min(m.x,W-dw-1);
      for(int i=0;i<(int)m.items.size();++i){ RGB bg=(i==menuSel)?th.bg_sel:th.bg_pop; scr.fill(dx,1+i,dw,1,bg);
        scr.text(dx+1,1+i,m.items[i].label,(i==menuSel)?th.fg_bright:th.fg,bg,false,dx+dw);
        if(!m.items[i].hint.empty())scr.text(dx+dw-(int)m.items[i].hint.size()-1,1+i,m.items[i].hint,th.fg_dim,bg,false,dx+dw); } }
    if(findActive){ std::string t=" Find: "+findQ+"▏"; int fw=std::max(28,(int)t.size()+2); int fx=std::max(LedX0+Lgut,LedX1-fw+1);
      scr.fill(fx,LedY0,fw,1,th.bg_pop); scr.text(fx,LedY0,t,th.fg_bright,th.bg_pop,false,fx+fw); }
    std::string f=scr.render(); write(STDOUT_FILENO,f.data(),f.size());
  }

  void onMouse(const Event&e){
    if(e.mb==MB::WheelUp){ if(focus==1)tree.sel=std::max(0,tree.sel-2); else {ed.cy=std::max(0,ed.cy-3);ed.clampx();} return; }
    if(e.mb==MB::WheelDown){ if(focus==1)tree.sel=std::min((int)tree.items.size()-1,tree.sel+2); else {ed.cy=std::min((int)ed.lines.size()-1,ed.cy+3);ed.clampx();} return; }
    if(!e.mdown){ selecting=false; return; }                            // button release
    if(e.my==0&&!e.mmotion){                                            // menu bar
      for(int i=0;i<(int)menus.size();++i) if(e.mx>=menus[i].x&&e.mx<menus[i].x+menus[i].w){ menuOpen=(menuOpen==i?-1:i); menuSel=0; return; }
      menuOpen=-1; return; }
    if(menuOpen>=0&&!e.mmotion){                                        // dropdown item click
      auto&m=menus[menuOpen]; int dw=24, dx=std::min(m.x,scr.w-dw-1);
      if(e.mx>=dx&&e.mx<dx+dw&&e.my>=1&&e.my<=(int)m.items.size()){ menuSel=e.my-1; runMenuItem(); } else menuOpen=-1;
      return; }
    if(e.mb==MB::Left&&e.mx>=LedX0+Lgut&&e.my>=LedY0&&e.my<=LedY1){      // editor click / drag-select
      focus=0; comp.active=false;
      ed.cy=std::min((int)ed.lines.size()-1,ed.rowoff+(e.my-LedY0)); ed.cx=ed.coloff+(e.mx-LedX0-Lgut); ed.clampx();
      if(!e.mmotion){ ed.selStart(); selecting=true; }                  // press = anchor; drag = extend
      return; }
    if(sidebar&&e.mx>=LtrX0&&!e.mmotion){                               // explorer
      focus=1; int idx=tree.off+(e.my-4); if(idx>=0&&idx<(int)tree.items.size()){ tree.sel=idx; auto&it=tree.items[idx]; if(it.is_directory())tree.load(it.path()); else open(it.path()); } return; }
  }

  void onKey(const Event&e){
    if(e.key==K::Mouse){ onMouse(e); return; }
    if(findActive){
      if(e.key==K::Esc)findActive=false;
      else if(e.key==K::Enter)findNext();
      else if(e.key==K::CtrlF)findActive=false;
      else if(e.key==K::Back){ if(!findQ.empty())findQ.pop_back(); }
      else if(e.key==K::Char){ findQ+=e.ch; findNext(); }
      return; }
    if(pal.active){
      if(e.key==K::Esc)pal.active=false;
      else if(e.key==K::Enter)runPalette();
      else if(e.key==K::Up)pal.sel=std::max(0,pal.sel-1);
      else if(e.key==K::Down)pal.sel=std::min((int)pal.match.size()-1,pal.sel+1);
      else if(e.key==K::Back){ if(!pal.query.empty()){pal.query.pop_back();filterPalette();} }
      else if(e.key==K::Char){ pal.query+=e.ch; filterPalette(); }
      return;
    }
    if(menuOpen>=0){ auto&m=menus[menuOpen];
      if(e.key==K::Esc)menuOpen=-1;
      else if(e.key==K::Down)menuSel=(menuSel+1)%(int)m.items.size();
      else if(e.key==K::Up)menuSel=(menuSel-1+(int)m.items.size())%(int)m.items.size();
      else if(e.key==K::Left)openMenu(menuOpen-1);
      else if(e.key==K::Right)openMenu(menuOpen+1);
      else if(e.key==K::Enter)runMenuItem();
      return; }
    if(e.key==K::CtrlQ){ running=false; return; }
    if(e.key==K::CtrlP){ openPalette(); return; }
    if(e.key==K::CtrlS){ ed.save(); status="Saved "+(ed.path.empty()?"":fs::path(ed.path).filename().string()); return; }
    if(e.key==K::CtrlB){ sidebar=true; focus^=1; return; }
    if(focus==1){
      if(e.key==K::Up)tree.sel=std::max(0,tree.sel-1);
      else if(e.key==K::Down)tree.sel=std::min((int)tree.items.size()-1,tree.sel+1);
      else if(e.key==K::Esc)focus=0;
      else if(e.key==K::Enter&&!tree.items.empty()){ auto&it=tree.items[tree.sel]; if(it.is_directory())tree.load(it.path()); else open(it.path()); }
      return;
    }
    // editor (focus 0) — clipboard / undo first
    if(e.key==K::CtrlC){ doCopy(); return; }
    if(e.key==K::CtrlX){ doCut(); return; }
    if(e.key==K::CtrlV){ doPaste(); comp.active=false; return; }
    if(e.key==K::CtrlZ){ ed.undo(); comp.active=false; lastOp='m'; return; }
    if(e.key==K::CtrlY){ ed.redo(); comp.active=false; lastOp='m'; return; }
    if(e.key==K::CtrlA){ ed.selectAll(); return; }
    if(e.key==K::CtrlF){ findActive=true; findQ=""; status="Find — type, Enter=next, Esc=close"; return; }
    if(e.key==K::Paste){ ed.snapshot(); if(ed.hasSel())ed.selDelete(); ed.insertText(e.ch); comp.active=false; lastOp='m'; return; }
    if(comp.active){
      if(e.key==K::Up){ comp.sel=std::max(0,comp.sel-1); return; }
      if(e.key==K::Down){ comp.sel=std::min((int)comp.items.size()-1,comp.sel+1); return; }
      if(e.key==K::Tab||e.key==K::Enter){ acceptComp(); return; }
      if(e.key==K::Esc){ comp.active=false; return; }
    }
    bool mv=(e.key==K::Up||e.key==K::Down||e.key==K::Left||e.key==K::Right||e.key==K::Home||e.key==K::End||e.key==K::PgUp||e.key==K::PgDn);
    if(mv){ if(e.shift){ if(!ed.selA)ed.selStart(); } else ed.selClear(); comp.active=false; }
    switch(e.key){
      case K::Up: ed.move(0,-1); break;
      case K::Down: ed.move(0,1); break;
      case K::Left: ed.move(-1,0); break;
      case K::Right: ed.move(1,0); break;
      case K::Home: ed.cx=0; break;
      case K::End: ed.cx=ed.len(); break;
      case K::PgUp: ed.cy=std::max(0,ed.cy-12); ed.clampx(); break;
      case K::PgDn: ed.cy=std::min((int)ed.lines.size()-1,ed.cy+12); ed.clampx(); break;
      case K::Tab: if(ed.hasSel()){ed.snapshot();ed.selDelete();} ed.indent(); lastOp='m'; break;
      case K::Enter: ed.snapshot(); if(ed.hasSel())ed.selDelete(); ed.newline(); comp.active=false; lastOp='m'; break;
      case K::Back: if(ed.hasSel()){ed.snapshot();ed.selDelete();} else {ed.snapshot();ed.back();} updateComp(); lastOp='m'; break;
      case K::CtrlSpace: updateComp(true); break;
      case K::Char:
        if(ed.hasSel()){ ed.snapshot(); ed.selDelete(); ed.insert(e.ch); lastOp='m'; }
        else { if(lastOp!='c'){ ed.snapshot(); lastOp='c'; } ed.insert(e.ch); }
        updateComp(); break;
      case K::Esc: comp.active=false; ed.selClear(); break;
      default: break;
    }
  }

  void run(const std::vector<std::string>&args){
    buildCommands(); buildMenus(); tree.load(".");
    if(args.size()>1)open(args[1]); else if(!tree.items.empty()&&!tree.items[0].is_directory())open(tree.items[0].path());
    term.enter(); term.size(scr.w,scr.h); scr.resize(scr.w,scr.h);
    while(running){ if(resized){resized=0;term.size(scr.w,scr.h);scr.resize(scr.w,scr.h);} draw(); Event e=readEvent(); if(e.key!=K::None)onKey(e); }
    if(mouseOn)disableMouse(); term.leave();
  }
};
volatile sig_atomic_t App::resized=0;
static App* g_app=nullptr;
static void onResize(int){ if(g_app)App::resized=1; }
static void onExit(int){ if(g_app)g_app->term.leave(); std::_Exit(0); }

int main(int argc,char**argv){
  std::vector<std::string> args(argv,argv+argc);
  App app; g_app=&app; signal(SIGWINCH,onResize); signal(SIGINT,onExit); signal(SIGTERM,onExit);
  app.run(args); return 0;
}