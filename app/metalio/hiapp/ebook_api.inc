// Read-only ebook access. Included inside the HiApp runtime namespace.
// Bounded relative paths under /E-Book; never accepts traversal or write modes.
bool BookLeaf(const std::string&name){return !name.empty()&&name.size()<=160&&name!="."&&name!=".."&&name.find_first_of("/\\\r\n\t")==name.npos&&name.find('\0')==name.npos;}
std::string BookKind(const std::string&name){auto dot=name.rfind('.');if(dot==name.npos)return {};auto ext=name.substr(dot);for(char&c:ext)if(c>='A'&&c<='Z')c+=32;return ext==".txt"?"TXT":ext==".hir"?"HIR":ext==".epub"?"EPUB":ext==".pdf"?"PDF":"";}
bool BookPath(const std::string&name,bool empty=false){
 if(name.empty())return empty;if(name.size()>480)return false;size_t start=0;unsigned depth=0;
 while(start<name.size()){auto end=name.find('/',start);if(end==name.npos)end=name.size();if(++depth>8||!BookLeaf(name.substr(start,end-start)))return false;start=end+1;}return name.back()!='/';
}
// Component checks prohibit a symlink from escaping the SD book area.
bool BookResolve(const std::string&root,const std::string&name,std::string&path){
 path=root;size_t start=0;while(start<name.size()){auto end=name.find('/',start);if(end==name.npos)end=name.size();path+="/"+name.substr(start,end-start);struct stat st{};if(lstat(path.c_str(),&st)<0||S_ISLNK(st.st_mode))return false;if(end<name.size()&&!S_ISDIR(st.st_mode))return false;start=end+1;}return true;
}
bool BookLess(const std::string&a,const std::string&b){
 size_t i=0,j=0;while(i<a.size()&&j<b.size()){
  unsigned char x=a[i],y=b[j];if(x>='0'&&x<='9'&&y>='0'&&y<='9'){
   size_t e=i,f=j;while(e<a.size()&&a[e]>='0'&&a[e]<='9')++e;while(f<b.size()&&b[f]>='0'&&b[f]<='9')++f;
   size_t u=i,v=j;while(u<e&&a[u]=='0')++u;while(v<f&&b[v]=='0')++v;
   if(e-u!=f-v)return e-u<f-v;int c=a.compare(u,e-u,b,v,f-v);if(c)return c<0;i=e;j=f;continue;
  }if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return x<y;++i;++j;
 }if(i!=a.size()||j!=b.size())return i==a.size();return a<b;
}
std::string BookRoot(std::string&error){
 if(!Prepare(false,error))return {};std::string root="/sdcard/E-Book";DIR*d=opendir("/sdcard");if(!d){error="SD卡不可用";return {};}
 while(auto*e=readdir(d)){if(!strcasecmp(e->d_name,"E-Book")){root=std::string("/sdcard/")+e->d_name;break;}}closedir(d);
 struct stat st{};if(stat(root.c_str(),&st)<0||!S_ISDIR(st.st_mode)){error="请在SD卡根目录创建E-Book文件夹";return {};}return root;
}
int Books(lua_State*L){
 auto*s=S(L);auto op=Text(L,1,16);auto name=Text(L,2,481);std::string error,root;
 if(!s->package.storage||s->stop)error="书籍存储权限不可用";else root=BookRoot(error);
 lua_createtable(L,0,8);const int result=lua_gettop(L);
 auto str=[&](const char*key,const std::string&v){lua_pushlstring(L,v.data(),v.size());lua_setfield(L,-2,key);};
 auto number=[&](const char*key,lua_Integer v){lua_pushinteger(L,v);lua_setfield(L,-2,key);};
 if(error.empty()&&op=="list"){
  lua_Integer skip=lua_tointeger(L,2);auto folder=Text(L,3,481);std::string path;
  number("api",2);number("ui",1);
  if(skip<0||skip>512||!BookPath(folder,true))error="目录位置无效";
  else if(!BookResolve(root,folder,path))error="书籍目录不可用";
  else {DIR*d=opendir(path.c_str());if(!d)error="无法打开书籍目录";else{
   struct Entry{std::string name,kind;long long bytes,stamp;bool folder;};std::vector<Entry>entries;
   while(auto*e=readdir(d)){
    std::string leaf=e->d_name;if(!BookLeaf(leaf)||leaf[0]=='.')continue;std::string kind=BookKind(leaf);struct stat st{};
    if(lstat((path+"/"+leaf).c_str(),&st)<0||S_ISLNK(st.st_mode))continue;bool dir=S_ISDIR(st.st_mode);
    if(!dir&&(!S_ISREG(st.st_mode)||kind.empty()))continue;
    if(entries.size()>=512){error="此目录超过512项，请分文件夹存放";break;}
    entries.push_back({leaf,dir?"DIR":kind,(long long)st.st_size,(long long)st.st_mtime,dir});
   }closedir(d);
   std::sort(entries.begin(),entries.end(),[](const Entry&a,const Entry&b){return a.folder!=b.folder?a.folder:BookLess(a.name,b.name);});
   lua_newtable(L);unsigned count=0;for(size_t i=(size_t)skip;i<entries.size()&&count<16;++i){const auto&e=entries[i];lua_createtable(L,0,5);str("name",e.name);str("kind",e.kind);number("bytes",e.bytes);number("stamp",e.stamp);lua_rawseti(L,-2,++count);}
   lua_setfield(L,result,"entries");number("next",skip+count);number("total",entries.size());lua_pushboolean(L,skip+count<(lua_Integer)entries.size());lua_setfield(L,result,"more");
  }}
 }else if(error.empty()&&(op=="read"||op=="stat")){
  if(!BookPath(name)||BookKind(name).empty())error="书籍文件名无效";
  else {std::string path;int fd=BookResolve(root,name,path)?open(path.c_str(),O_RDONLY):-1;if(fd<0)error="无法打开书籍";else{
   struct stat st{};if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<0||st.st_size>32*1024*1024)error="书籍无效或超过32MiB";
   else {number("bytes",st.st_size);number("stamp",st.st_mtime);if(op=="read"){
    lua_Integer offset=lua_tointeger(L,3),wanted=lua_tointeger(L,4);
    if(offset<0||offset>st.st_size||wanted<0||wanted>48*1024)error="读取范围无效";
    else {size_t n=std::min((size_t)wanted,(size_t)(st.st_size-offset));std::string data(n,'\0');size_t at=0;
     if(lseek(fd,(off_t)offset,SEEK_SET)<0)error="书籍定位失败";
     else{while(at<n&&!s->stop){ssize_t got=read(fd,&data[at],n-at);if(got>0)at+=got;else if(got<0&&errno==EINTR)continue;else break;}if(at!=n)error="书籍读取中断";else str("data",data);}
    }
   }}close(fd);
  }}
 }else if(error.empty())error="未知书籍操作";
 str("error",error);s->until=Now()+4000;return 1;
}
