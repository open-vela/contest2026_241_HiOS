// SPDX-License-Identifier: Apache-2.0
#include "pinyin_engine.h"
#include <cstring>
#include <cstdint>

namespace hios {
namespace {
struct DictionaryEntry { const char *key; const char *text; const char *double_key=nullptr; };
#include "pinyin_dictionary.inc"
// Phrases separated by |. Single syllables below remain individual characters.
const DictionaryEntry kPhrases[] = {
    {"nihao", "你好", "nihc"},
    {"xiexie", "谢谢", "xpxp"},
    {"zaijian", "再见", "zdjm"},
    {"shijie", "世界|视界", "uijp"},
    {"zhongguo", "中国", "vsgo"},
    {"tianqi", "天气", "tmqi"},
    {"jintian", "今天", "jbtm"},
    {"mingtian", "明天", "mktm"},
    {"tupian", "图片", "tupm"},
    {"shengcheng", "生成", "ugig"},
    {"shuru", "输入", "uuru"},
    {"shurufa", "输入法", "uurufa"},
    {"kaiji", "开机", "kdji"},
    {"guanji", "关机", "grji"},
    {"shezhi", "设置", "uevi"},
    {"yinliang", "音量", "ybll"},
    {"yinyue", "音乐", "ybyt"},
    {"beijing", "北京", "bwjk"},
    {"shanghai", "上海", "uhhd"},
    {"fengjing", "风景", "fgjk"},
    {"maomi", "猫咪", "mcmi"},
    {"lvse", "绿色", "lvse"},
    {"nihaoshijie", "你好世界", "nihcuijp"},
    {"woxiangyao", "我想要", "woxlyc"},
    {"yizhimao", "一只猫", "yivimc"},
    {"xiaomao", "小猫", "xnmc"},
    {"xiaogou", "小狗", "xngz"},
    {"lantian", "蓝天", "ljtm"},
    {"baiyun", "白云", "bdyy"},
    {"dahai", "大海", "dahd"},
    {"senlin", "森林", "sflb"},
    {"shanmai", "山脉", "ujmd"},
    {"riguang", "日光", "rigl"},
    {"yangguang", "阳光", "yhgl"},
    {"yewan", "夜晚", "yewj"},
    {"xingkong", "星空", "xkks"},
    {"katong", "卡通", "kats"},
    {"dongman", "动漫", "dsmj"},
    {"xiezhen", "写真", "xpvf"},
    {"keai", "可爱", "keai"},
    {"meili", "美丽", "mwli"},
    {"gaoqing", "高清", "gcqk"},
    {"bihua", "壁画", "bihx"},
    {"youhua", "油画", "yzhx"},
    {"shuicai", "水彩", "uvcd"},
    {"shouji", "手机", "uzji"},
    {"diannao", "电脑", "dmnc"},
    {"huanying", "欢迎", "hryk"},
    {"zaoshanghao", "早上好", "zcuhhc"},
    {"wanan", "晚安", "wjan"},
    {"meiguanxi", "没关系", "mwgrxi"},
    {"duibuqi", "对不起", "dvbuqi"},
    {"keyi", "可以", "keyi"},
    {"bukeqi", "不客气", "bukeqi"},
    {"kaideng", "开灯", "kddg"},
    {"guandeng", "关灯", "grdg"},
    {"bofang", "播放", "bofh"},
    {"zanting", "暂停", "zjtk"},
    {"kaishi", "开始", "kdui"},
    {"women", "我们", "womf"},
    {"nimen", "你们", "nimf"},
    {"tamen", "他们", "tamf"},
    {"dajia", "大家", "dajx"},
    {"ziji", "自己", "ziji"},
    {"xianzai", "现在", "xmzd"},
    {"yihou", "以后", "yihz"},
    {"zhiqian", "之前", "viqm"},
    {"jintianwanshang", "今天晚上", "jbtmwjuh"},
    {"mingtianzaoshang", "明天早上", "mktmzcuh"},
    {"xiawu", "下午", "xxwu"},
    {"shangwu", "上午", "uhwu"},
    {"zhongwu", "中午", "vswu"},
    {"wanshang", "晚上", "wjuh"},
    {"xingqi", "星期", "xkqi"},
    {"zhoumo", "周末", "vzmo"},
    {"yuefen", "月份", "ytff"},
    {"shengri", "生日", "ugri"},
    {"chunjie", "春节", "iyjp"},
    {"jieri", "节日", "jpri"},
    {"xinnian", "新年", "xbnm"},
    {"kuaile", "快乐", "kkle"},
    {"xingfu", "幸福", "xkfu"},
    {"jiankang", "健康", "jmkh"},
    {"pingan", "平安", "pkan"},
    {"pengyou", "朋友", "pgyz"},
    {"tongxue", "同学", "tsxt"},
    {"laoshi", "老师", "lcui"},
    {"jiaren", "家人", "jxrf"},
    {"baba", "爸爸", "baba"},
    {"mama", "妈妈", "mama"},
    {"gege", "哥哥", "gege"},
    {"jiejie", "姐姐", "jpjp"},
    {"didi", "弟弟", "didi"},
    {"meimei", "妹妹", "mwmw"},
    {"haizi", "孩子", "hdzi"},
    {"baobao", "宝宝", "bcbc"},
    {"nver", "女儿", "nver"},
    {"erzi", "儿子", "erzi"},
    {"gongzuo", "工作", "gszo"},
    {"xuexi", "学习", "xtxi"},
    {"shenghuo", "生活", "ugho"},
    {"xuexiao", "学校", "xtxn"},
    {"gongsi", "公司", "gssi"},
    {"yiyuan", "医院", "yiyr"},
    {"yinhang", "银行", "ybhh"},
    {"shangdian", "商店", "uhdm"},
    {"fandian", "饭店", "fjdm"},
    {"chaoshi", "超市", "icui"},
    {"gongyuan", "公园", "gsyr"},
    {"ditie", "地铁", "ditp"},
    {"gongjiao", "公交", "gsjn"},
    {"qiche", "汽车", "qiie"},
    {"huoche", "火车", "hoie"},
    {"feiji", "飞机", "fwji"},
    {"chezhan", "车站", "ievj"},
    {"jichang", "机场", "jiih"},
    {"jiudian", "酒店", "jqdm"},
    {"ditu", "地图", "ditu"},
    {"daohang", "导航", "dchh"},
    {"dizhi", "地址", "divi"},
    {"weizhi", "位置", "wwvi"},
    {"guangzhou", "广州", "glvz"},
    {"shenzhen", "深圳", "ufvf"},
    {"hangzhou", "杭州", "hhvz"},
    {"nanjing", "南京", "njjk"},
    {"suzhou", "苏州", "suvz"},
    {"chengdu", "成都", "igdu"},
    {"chongqing", "重庆", "isqk"},
    {"wuhan", "武汉", "wuhj"},
    {"xian", "现|西安", "xian"},
    {"tianjin", "天津", "tmjb"},
    {"changsha", "长沙", "ihua"},
    {"zhengzhou", "郑州", "vgvz"},
    {"jinan", "济南", "jban"},
    {"qingdao", "青岛", "qkdc"},
    {"xiamen", "厦门", "xxmf"},
    {"fuzhou", "福州", "fuvz"},
    {"kunming", "昆明", "kymk"},
    {"haikou", "海口", "hdkz"},
    {"xianggang", "香港", "xlgh"},
    {"aomen", "澳门", "aomf"},
    {"taiwan", "台湾", "tdwj"},
    {"zhongguoyidong", "中国移动", "vsgoyids"},
    {"zhongguoliantong", "中国联通", "vsgolmts"},
    {"zhongguodianxin", "中国电信", "vsgodmxb"},
    {"zhongguoguangdian", "中国广电", "vsgogldm"},
    {"shoujihaoma", "手机号码", "uzjihcma"},
    {"dianhuahaoma", "电话号码", "dmhxhcma"},
    {"lianxiren", "联系人", "lmxirf"},
    {"tongxunlu", "通讯录", "tsxylu"},
    {"tonghuajilu", "通话记录", "tshxjilu"},
    {"weijielaidian", "未接来电", "wwjplddm"},
    {"bodadianhua", "拨打电话", "bodadmhx"},
    {"jietingdianhua", "接听电话", "jptkdmhx"},
    {"guaduandianhua", "挂断电话", "gxdrdmhx"},
    {"fasongduanxin", "发送短信", "fassdrxb"},
    {"jieshouduanxin", "接收短信", "jpuzdrxb"},
    {"huifuduanxin", "回复短信", "hvfudrxb"},
    {"duanxinlingsheng", "短信铃声", "drxblkug"},
    {"laidianlingsheng", "来电铃声", "lddmlkug"},
    {"naozhonglingsheng", "闹钟铃声", "ncvslkug"},
    {"shoujianren", "收件人", "uzjmrf"},
    {"fajianren", "发件人", "fajmrf"},
    {"yuyinxiaoxi", "语音消息", "yuybxnxi"},
    {"shurumima", "输入密码", "uurumima"},
    {"wuxianwangluo", "无线网络", "wuxmwhlo"},
    {"wangluopeizhi", "网络配置", "whlopwvi"},
    {"feixingmoshi", "飞行模式", "fwxkmoui"},
    {"lanyaerji", "蓝牙耳机", "ljyaerji"},
    {"lianjiechenggong", "连接成功", "lmjpiggs"},
    {"lianjieshibai", "连接失败", "lmjpuibd"},
    {"chongxinlianjie", "重新连接", "isxblmjp"},
    {"zhengzailianjie", "正在连接", "vgzdlmjp"},
    {"qingshaohou", "请稍后", "qkuchz"},
    {"dengyixia", "等一下", "dgyixx"},
    {"meiwenti", "没问题", "mwwfti"},
    {"haode", "好的", "hcde"},
    {"zhidaole", "知道了", "vidcle"},
    {"shoudaole", "收到了", "uzdcle"},
    {"xiexieni", "谢谢你", "xpxpni"},
    {"buhaoyisi", "不好意思", "buhcyisi"},
    {"qingwen", "请问", "qkwf"},
    {"keyima", "可以吗", "keyima"},
    {"shenme", "什么", "ufme"},
    {"zenme", "怎么", "zfme"},
    {"weishenme", "为什么", "wwufme"},
    {"nali", "哪里|那里", "nali"},
    {"duoshao", "多少", "douc"},
    {"jidian", "几点", "jidm"},
    {"shenmeshihou", "什么时候", "ufmeuihz"},
    {"xuyao", "需要", "xuyc"},
    {"bangmang", "帮忙", "bhmh"},
    {"dakai", "打开", "dakd"},
    {"guanbi", "关闭", "grbi"},
    {"jieshu", "结束", "jpuu"},
    {"fanhui", "返回", "fjhv"},
    {"queren", "确认", "qtrf"},
    {"quxiao", "取消", "quxn"},
    {"baocun", "保存", "bccy"},
    {"shanchu", "删除", "ujiu"},
    {"fuzhi", "复制", "fuvi"},
    {"zhantie", "粘贴", "vjtp"},
    {"jianqie", "剪切", "jmqp"},
    {"quanxuan", "全选", "qrxr"},
    {"sousuo", "搜索", "szso"},
    {"chazhao", "查找", "iavc"},
    {"shuaxin", "刷新", "uxxb"},
    {"qingkong", "清空", "qkks"},
    {"shengyin", "声音", "ugyb"},
    {"jingyin", "静音", "jkyb"},
    {"xiangling", "响铃", "xllk"},
    {"zhendong", "震动", "vfds"},
    {"liangdu", "亮度", "lldu"},
    {"pingmu", "屏幕", "pkmu"},
    {"dianchi", "电池", "dmii"},
    {"chongdian", "充电", "isdm"},
    {"dianliang", "电量", "dmll"},
    {"xitong", "系统", "xits"},
    {"gengxin", "更新", "ggxb"},
    {"chongqi", "重启", "isqi"},
    {"gequ", "歌曲", "gequ"},
    {"geshou", "歌手", "geuz"},
    {"zhuanji", "专辑", "vrji"},
    {"shangyishou", "上一首", "uhyiuz"},
    {"xiayishou", "下一首", "xxyiuz"},
    {"suijibofang", "随机播放", "svjibofh"},
    {"shunxubofang", "顺序播放", "uyxubofh"},
    {"danquxunhuan", "单曲循环", "djquxyhr"},
    {"shoucang", "收藏", "uzch"},
    {"liebiao", "列表", "lpbn"},
    {"erji", "耳机", "erji"},
    {"yangshengqi", "扬声器", "yhugqi"},
    {"maikefeng", "麦克风", "mdkefg"},
    {"luyin", "录音", "luyb"},
    {"luzhi", "录制", "luvi"},
    {"bofangluyin", "播放录音", "bofhluyb"},
    {"paizhao", "拍照", "pdvc"},
    {"zhaopian", "照片", "vcpm"},
    {"xiangji", "相机", "xlji"},
    {"xiangce", "相册", "xlce"},
    {"wenjian", "文件", "wfjm"},
    {"wenjianjia", "文件夹", "wfjmjx"},
    {"neicunka", "内存卡", "nwcyka"},
    {"cunchukongjian", "存储空间", "cyiuksjm"},
    {"wenjianguanli", "文件管理", "wfjmgrli"},
    {"yuyinzhushou", "语音助手", "yuybvuuz"},
    {"zhihuiping", "智慧屏", "vihvpk"},
    {"zhuye", "主页", "vuye"},
    {"quanpin", "全拼", "qrpb"},
    {"shuangpin", "双拼", "ulpb"},
    {"pinyin", "拼音", "pbyb"},
    {"jiugongge", "九宫格", "jqgsge"},
    {"houxuanci", "候选词", "hzxrci"},
    {"shijieshizhong", "世界时钟", "uijpuivs"},
    {"naozhong", "闹钟", "ncvs"},
    {"miaobiao", "秒表", "mnbn"},
    {"jishiqi", "计时器", "jiuiqi"},
    {"beijingshijian", "北京时间", "bwjkuijm"},
    {"xiaoshi", "小时", "xnui"},
    {"fenzhong", "分钟", "ffvs"},
    {"miaozhong", "秒钟", "mnvs"},
    {"shaohoutixing", "稍后提醒", "uchztixk"},
    {"tianqiyubao", "天气预报", "tmqiyubc"},
    {"jinritianqi", "今日天气", "jbritmqi"},
    {"mingtiantianqi", "明天天气", "mktmtmqi"},
    {"wendu", "温度", "wfdu"},
    {"xiayu", "下雨", "xxyu"},
    {"qingtian", "晴天", "qktm"},
    {"yintian", "阴天", "ybtm"},
    {"duoyun", "多云", "doyy"},
    {"xiaxue", "下雪", "xxxt"},
    {"dafeng", "大风", "dafg"},
    {"kongqizhiliang", "空气质量", "ksqivill"},
    {"rili", "日历", "rili"},
    {"nongli", "农历", "nsli"},
    {"riqi", "日期", "riqi"},
    {"xingqiyi", "星期一", "xkqiyi"},
    {"xingqier", "星期二", "xkqier"},
    {"xingqisan", "星期三", "xkqisj"},
    {"xingqisi", "星期四", "xkqisi"},
    {"xingqiwu", "星期五", "xkqiwu"},
    {"xingqiliu", "星期六", "xkqilq"},
    {"xingqiri", "星期日", "xkqiri"},
    {"xingqitian", "星期天", "xkqitm"},
    {"yiyue", "一月", "yiyt"},
    {"eryue", "二月", "eryt"},
    {"sanyue", "三月", "sjyt"},
    {"siyue", "四月", "siyt"},
    {"wuyue", "五月", "wuyt"},
    {"liuyue", "六月", "lqyt"},
    {"qiyue", "七月", "qiyt"},
    {"bayue", "八月", "bayt"},
    {"jiuyue", "九月", "jqyt"},
    {"shiyue", "十月", "uiyt"},
    {"shiyiyue", "十一月", "uiyiyt"},
    {"shieryue", "十二月", "uieryt"},
    {"zaocan", "早餐", "zccj"},
    {"wucan", "午餐", "wucj"},
    {"wancan", "晚餐", "wjcj"},
    {"chifan", "吃饭", "iifj"},
    {"heshui", "喝水", "heuv"},
    {"huijia", "回家", "hvjx"},
    {"chumen", "出门", "iumf"},
    {"shangban", "上班", "uhbj"},
    {"xiaban", "下班", "xxbj"},
    {"daojia", "到家", "dcjx"},
    {"daole", "到了", "dcle"},
    {"lushang", "路上", "luuh"},
    {"chufa", "出发", "iufa"},
    {"daoda", "到达", "dcda"},
    {"zaoan", "早安", "zcan"},
    {"mingtianjian", "明天见", "mktmjm"},
    {"yihuier", "一会儿", "yihver"},
    {"mashang", "马上", "mauh"},
    {"shaodeng", "稍等", "ucdg"},
    {"yijing", "已经", "yijk"},
    {"zhengzai", "正在", "vgzd"},
    {"haiyou", "还有", "hdyz"},
    {"haiyouma", "还有吗", "hdyzma"},
    {"yiqi", "一起", "yiqi"},
    {"yidian", "一点", "yidm"},
    {"yixie", "一些", "yixp"},
    {"yixia", "一下", "yixx"},
    {"zheli", "这里", "veli"},
    {"zhege", "这个", "vege"},
    {"nage", "那个", "nage"},
    {"zheyang", "这样", "veyh"},
    {"nayang", "那样", "nayh"},
    {"keneng", "可能", "keng"},
    {"yinggai", "应该", "ykgd"},
    {"bixu", "必须", "bixu"},
    {"yiding", "一定", "yidk"},
    {"buyong", "不用", "buys"},
    {"buneng", "不能", "bung"},
    {"buyao", "不要", "buyc"},
    {"meiyou", "没有", "mwyz"},
    {"bushi", "不是", "buui"},
    {"zhende", "真的", "vfde"},
    {"feichang", "非常", "fwih"},
    {"tebie", "特别", "tebp"},
    {"bijiao", "比较", "bijn"},
    {"qingchu", "清楚", "qkiu"},
    {"qingxi", "清晰", "qkxi"},
    {"piaoliang", "漂亮", "pnll"},
    {"haokan", "好看", "hckj"},
    {"xihuan", "喜欢", "xihr"},
    {"xiangnian", "想念", "xlnm"},
    {"ganxie", "感谢", "gjxp"},
    {"zhufu", "祝福", "vufu"},
    {"anquan", "安全", "anqr"},
    {"wending", "稳定", "wfdk"},
    {"zhengchang", "正常", "vgih"},
    {"zidong", "自动", "zids"},
    {"shoudong", "手动", "uzds"},
    {"shiyong", "使用", "uiys"},
    {"gongneng", "功能", "gsng"},
    {"wenti", "问题", "wfti"},
    {"xiufu", "修复", "xqfu"},
    {"wancheng", "完成", "wjig"},
    {"chenggong", "成功", "iggs"},
    {"shibai", "失败", "uibd"},
    {"zhongshi", "重试", "vsui"},
    {"xuanze", "选择", "xrze"},
    {"neirong", "内容", "nwrs"},
    {"wenzi", "文字", "wfzi"},
    {"zhongwen", "中文", "vswf"},
    {"yingwen", "英文", "ykwf"},
    {"shuzi", "数字", "uuzi"},
    {"fuhao", "符号", "fuhc"},
    {"biaoqing", "表情", "bnqk"},
    {"xiaoxi", "消息", "xnxi"},
    {"tongzhi", "通知", "tsvi"},
    {"weidu", "未读", "wwdu"},
    {"yidu", "已读", "yidu"},
    {"fasong", "发送", "fass"},
    {"jieshou", "接收", "jpuz"},
    {"baocunchenggong", "保存成功", "bccyiggs"},
    {"shanchuchenggong", "删除成功", "ujiuiggs"},
    {"caozuowancheng", "操作完成", "cczowjig"},
    {"jintianxiawu", "今天下午", "jbtmxxwu"},
    {"jintianshangwu", "今天上午", "jbtmuhwu"},
    {"mingtianxiawu", "明天下午", "mktmxxwu"},
    {"houtian", "后天", "hztm"},
    {"zuotian", "昨天", "zotm"},
    {"jinnian", "今年", "jbnm"},
    {"mingnian", "明年", "mknm"},
    {"xiageyue", "下个月", "xxgeyt"},
    {"xiaxingqi", "下星期", "xxxkqi"},
    {"shangxingqi", "上星期", "uhxkqi"},
    {"xiawuhao", "下午好", "xxwuhc"},
    {"wanshanghao", "晚上好", "wjuhhc"},
    {"zhongwuhao", "中午好", "vswuhc"},
    {"shengrikuaile", "生日快乐", "ugrikkle"},
    {"xinniankuaile", "新年快乐", "xbnmkkle"},
    {"yilupingan", "一路平安", "yilupkan"},
    {"zhuyianquan", "注意安全", "vuyianqr"},
    {"baochilianxi", "保持联系", "bciilmxi"},
    {"qinghuifu", "请回复", "qkhvfu"},
    {"geiwodadianhua", "给我打电话", "gwwodadmhx"},
    {"geiwofaduanxin", "给我发短信", "gwwofadrxb"},
    {"wozailushang", "我在路上", "wozdluuh"},
    {"wodaojiale", "我到家了", "wodcjxle"},
    {"wozhidaole", "我知道了", "wovidcle"},
    {"womashangdao", "我马上到", "womauhdc"},
    {"woaini", "我爱你", "woaini"},
    {"wohenhao", "我很好", "wohfhc"},
    {"wobuqingchu", "我不清楚", "wobuqkiu"},
    {"meishi", "没事", "mwui"},
    {"huanyinghuilai", "欢迎回来", "hrykhvld"},
    {"zhinengjiaju", "智能家居", "vingjxju"},
    {"diantai", "电台", "dmtd"},
    {"guangbo", "广播", "glbo"},
    {"xinwen", "新闻", "xbwf"},
    {"tiyu", "体育", "tiyu"},
    {"xiaoshuo", "小说", "xnuo"},
    {"fanyi", "翻译", "fjyi"},
    {"zhinanzhen", "指南针", "vinjvf"},
    {"jisuanqi", "计算器", "jisrqi"},
    {"dingwei", "定位", "dkww"},
    {"ceshi", "测试", "ceui"},
    {"gouwu", "购物", "gzwu"},
    {"fukuan", "付款", "fukr"},
    {"shoukuan", "收款", "uzkr"},
    {"dingdan", "订单", "dkdj"},
    {"kuaidi", "快递", "kkdi"},
    {"qujian", "取件", "qujm"},
    {"fuwu", "服务", "fuwu"},
    {"haoma", "号码", "hcma"},
    {"heimingdan", "黑名单", "hwmkdj"},
    {"zhiding", "置顶", "vidk"},
    {"biaojiyidu", "标记已读", "bnjiyidu"},
    {"de", "得|的", "de"},
    {"yi", "以|一", "yi"},
    {"shi", "十|时|是", "ui"},
    {"zai", "再|在", "zd"},
    {"bu", "部|不", "bu"},
    {"le", "了", "le"},
    {"you", "有", "yz"},
    {"he", "和", "he"},
    {"ren", "人", "rf"},
    {"zhe", "这", "ve"},
    {"zhong", "种|中", "vs"},
    {"da", "大", "da"},
    {"wei", "为", "ww"},
    {"shang", "上", "uh"},
    {"ge", "个", "ge"},
    {"guo", "过|国", "go"},
    {"wo", "我", "wo"},
    {"yao", "要", "yc"},
    {"ta", "他", "ta"},
    {"lai", "来", "ld"},
    {"yong", "用", "ys"},
    {"men", "们", "mf"},
    {"sheng", "生", "ug"},
    {"dao", "到", "dc"},
    {"zuo", "作", "zo"},
    {"di", "地", "di"},
    {"yu", "于", "yu"},
    {"chu", "出", "iu"},
    {"jiu", "就", "jq"},
    {"fen", "分", "ff"},
    {"dui", "对", "dv"},
    {"cheng", "成", "ig"},
    {"hui", "会", "hv"},
    {"ke", "可", "ke"},
    {"zhu", "主", "vu"},
    {"fa", "法|发", "fa"},
    {"nian", "年", "nm"},
    {"dong", "动", "ds"},
    {"tong", "同", "ts"},
    {"gong", "工", "gs"},
    {"ye", "也", "ye"},
    {"neng", "能", "ng"},
    {"xia", "下", "xx"},
    {"zi", "自|子", "zi"},
    {"shuo", "说", "uo"},
    {"chan", "产", "ij"},
    {"mian", "面", "mm"},
    {"er", "二|而", "er"},
    {"fang", "方", "fh"},
    {"hou", "后", "hz"},
    {"duo", "多", "do"},
    {"ding", "定", "dk"},
    {"xing", "行", "xk"},
    {"xue", "学", "xt"},
    {"suo", "所", "so"},
    {"min", "民", "mb"},
    {"jing", "经", "jk"},
    {"san", "三", "sj"},
    {"zhi", "之", "vi"},
    {"jin", "进", "jb"},
    {"deng", "等", "dg"},
    {"du", "度", "du"},
    {"jia", "家", "jx"},
    {"dian", "电", "dm"},
    {"li", "理|里|力", "li"},
    {"ru", "如", "ru"},
    {"shui", "水", "uv"},
    {"hua", "化", "hx"},
    {"gao", "高", "gc"},
    {"qi", "起", "qi"},
    {"xiao", "小", "xn"},
    {"wu", "物", "wu"},
    {"hai", "还", "hd"},
    {"ni", "你", "ni"},
    {"qu", "去", "qu"},
    {"hao", "好", "hc"},
    {"ba", "把", "ba"},
    {"gei", "给", "gw"},
    {"qing", "请", "qk"},
    {"hen", "很", "hf"},
    {"xiang", "想", "xl"},
    {"ai", "爱", "ai"},
    {"ma", "吗", "ma"},
    {"ne", "呢", "ne"},
    {"a", "啊", "aa"},
};
static const char *const kSyllableCodes[]={
"aa","ai","an","ah","ao","ba","bd","bj","bh","bc","bw","bf","bg","bi","bm","bn","bp","bb","bk","bo","bu","ca","cd","cj","ch","cc","ce","cf","cg","ia","id","ij","ih","ic","ie","if","ig","ii","is","iz","iu","ix","ik","ir","il","iv","iy","io","ci","cs","cz","cu","cr","cv","cy","co","da","dd","dj","dh","dc","de","dw","dg","di","dm","dn","dp","dk","dq","ds","dz","du","dr","dv","dy","do","ee","en","er","fa","fj","fh","fw","ff","fg","fo","fz","fu","ga","gd","gj","gh","gc","ge","gw","gf","gg","gs","gz","gu","gx","gk","gr","gl","gv","gy","go","ha","hd","hj","hh","hc","he","hw","hf","hg","~~","hs","hz","hu","hx","hk","hr","hl","hv","hy","ho","ji","jx","jm","jl","jn","jp","jb","jk","js","jq","ju","jr","jt","jy","ka","kd","kj","kh","kc","ke","kw","kf","kg","ks","kz","ku","kx","kk","kr","kl","kv","ky","ko","la","ld","lj","lh","lc","le","lw","lg","li","lx","lm","ll","ln","lp","lb","lk","lq","lo","ls","lz","lu","lr","ly","lo","lv","lt","ma","md","mj","mh","mc","me","mw","mf","mg","mi","mm","mn","mp","mb","mk","mq","mo","mz","mu","na","nd","nj","nh","nc","ne","nw","nf","ng","ni","nm","nl","nn","np","nb","nk","nq","ns","nu","nr","no","nv","nt","oo","ou","pa","pd","pj","ph","pc","pw","pf","pg","pi","pm","pn","pp","pb","pk","po","pz","pu","qi","qx","qm","ql","qn","qp","qb","qk","qs","qq","qu","qr","qt","qy","rj","rh","rc","re","rf","rg","ri","rs","rz","ru","rr","rv","ry","ro","sa","sd","sj","sh","sc","se","sf","sg","ua","ud","uj","uh","uc","ue","uw","uf","ug","ui","uz","uu","ux","uk","ur","ul","uv","uy","uo","si","ss","sz","su","sr","sv","sy","so","ta","td","tj","th","tc","te","tg","ti","tm","tn","tp","tk","ts","tz","tu","tr","tv","ty","to","wa","wd","wj","wh","ww","wf","wg","wo","wu","xi","xx","xm","xl","xn","xp","xb","xk","xs","xq","xu","xr","xt","xy","ya","yj","yh","yc","ye","yi","yb","yk","yo","ys","yz","yu","yr","yt","yy","za","zd","zj","zh","zc","ze","zw","zf","zg","va","vd","vj","vh","vc","ve","vw","vf","vg","vi","vs","vz","vu","vx","vk","vr","vl","vv","vy","vo","zi","zs","zz","zu","zr","zv","zy","zo"
};
const char *Lookup(const char *key) {
    size_t lo = 0, hi = sizeof(kDictionary) / sizeof(kDictionary[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        int cmp = std::strcmp(key, kDictionary[mid].key);
        if (!cmp) return kDictionary[mid].text;
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}
size_t Utf8Bytes(const char *p) {
    unsigned char c = static_cast<unsigned char>(*p);
    return c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
}
}
bool PinyinEngine::Append(char c) {
    if (c>='A' && c<='Z') c+='a'-'A';
    if ((c<'a'||c>'z') && c!='\'') return false;
    if (length_==kMaxInput || (c=='\'' && (!length_ || input_[length_-1]=='\''))) return false;
    input_[length_++]=c;input_[length_]=0;dirty_=true;return true;
}
bool PinyinEngine::Backspace(){if(!length_)return false;input_[--length_]=0;dirty_=true;return true;}
void PinyinEngine::Clear(){length_=0;input_[0]=0;dirty_=true;}
void PinyinEngine::SetDoublePinyin(bool enabled){if(double_!=enabled){Clear();double_=enabled;}}
void PinyinEngine::Add(const char *text,size_t bytes,size_t used) const {
    if (!bytes || bytes>=kCandidateBytes || !used || count_==kMaxCandidates) return;
    for(size_t i=0;i<count_;++i)
        if(std::strlen(candidates_[i])==bytes && !std::strncmp(candidates_[i],text,bytes))return;
    std::memcpy(candidates_[count_],text,bytes);candidates_[count_][bytes]=0;
    consumed_[count_++]=used;
}
void PinyinEngine::Rebuild() const {
    if(!dirty_)return;dirty_=false;count_=0;if(!length_)return;
    // Remove explicit syllable separators for phrase lookup, retaining a
    // map back to physical keystrokes for partial candidate commits.
    char key[kMaxInput+1]{};uint8_t physical[kMaxInput+1]{};bool boundary[kMaxInput+1]{};size_t n=0;
    for(size_t i=0;i<length_;++i){if(input_[i]=='\'')boundary[n]=true;else{key[n++]=input_[i];physical[n]=i+1;}}
    auto used=[&](size_t end){size_t u=physical[end];if(input_[u]=='\'')++u;return u;};
    struct Path {int score=-100000;int previous=-1;const char *text=nullptr;size_t bytes=0;};
    Path best[kMaxInput+1];best[0].score=0;
    // Bounded dynamic programming: compose continuous syllables/phrases;
    // prefer longer vocabulary matches without dropping any input suffix.
    for(size_t pos=0;pos<n;++pos){
        if(best[pos].score<0)continue;
        auto offer=[&](const char *code,const char *text,bool phrase){
            size_t len=std::strlen(code);if(pos+len>n || std::strncmp(key+pos,code,len))return;
            if(!phrase)for(size_t j=pos+1;j<pos+len;++j)if(boundary[j])return;
            size_t bytes=phrase?std::strcspn(text,"|"):Utf8Bytes(text);
            int score=best[pos].score+static_cast<int>(len)*16-(phrase?2:12);
            if(score>best[pos+len].score)best[pos+len]={score,static_cast<int>(pos),text,bytes};
        };
        for(const auto &p:kPhrases)offer(double_?p.double_key:p.key,p.text,true);
        for(size_t j=0;j<sizeof(kDictionary)/sizeof(kDictionary[0]);++j)
            offer(double_?kSyllableCodes[j]:kDictionary[j].key,kDictionary[j].text,false);
    }
    if(n && best[n].previous>=0){
        size_t ends[kMaxInput],parts=0,bytes=0;int end=n;
        while(end>0 && parts<kMaxInput){ends[parts++]=end;bytes+=best[end].bytes;end=best[end].previous;}
        if(end==0 && bytes<kCandidateBytes){char sentence[kCandidateBytes]{};size_t at=0;
            while(parts){const auto &p=best[ends[--parts]];std::memcpy(sentence+at,p.text,p.bytes);at+=p.bytes;}
            Add(sentence,at,length_);
        }
    }
    // Longest matching phrase prefixes, then individual readings; unfinished
    // prefixes can also complete a word. Every candidate remembers its span.
    for(size_t span=n;span;--span)for(const auto &p:kPhrases){
        const char *code=double_?p.double_key:p.key;
        if(std::strlen(code)!=span || std::strncmp(key,code,span))continue;
        for(const char *t=p.text;*t;){size_t bytes=std::strcspn(t,"|");Add(t,bytes,used(span));t+=bytes;if(*t)++t;}
    }
    size_t longest=0,segment=n;
    for(size_t j=1;j<n;++j)if(boundary[j]){segment=j;break;}
    for(size_t j=0;j<sizeof(kDictionary)/sizeof(kDictionary[0]);++j){
        const char *code=double_?kSyllableCodes[j]:kDictionary[j].key;size_t len=std::strlen(code);
        if(len<=segment && !std::strncmp(key,code,len) && len>longest)longest=len;
    }
    // Interleave ambiguous double-pinyin readings by character rank.
    for(size_t rank=0;rank<128 && count_<kMaxCandidates;++rank){bool any=false;
        for(size_t j=0;j<sizeof(kDictionary)/sizeof(kDictionary[0]);++j){
            const char *code=double_?kSyllableCodes[j]:kDictionary[j].key;size_t len=std::strlen(code);
            if(longest ? len!=longest || std::strncmp(key,code,len) : len<segment || std::strncmp(key,code,segment))continue;
            const char *t=kDictionary[j].text;size_t r=rank;while(r && *t){t+=Utf8Bytes(t);--r;}
            if(*t){any=true;Add(t,Utf8Bytes(t),used(longest?longest:segment));}
        }if(!any)break;
    }
    if(n>=2)for(const auto &p:kPhrases){const char *code=double_?p.double_key:p.key;
        if(std::strlen(code)>n && !std::strncmp(key,code,n))Add(p.text,std::strcspn(p.text,"|"),length_);
    }
}
size_t PinyinEngine::Count() const{Rebuild();return count_;}
bool PinyinEngine::Candidate(size_t index,char *out,size_t cap) const{
    Rebuild();if(!out||!cap)return false;out[0]=0;if(index>=count_ || std::strlen(candidates_[index])>=cap)return false;
    std::strcpy(out,candidates_[index]);return true;
}
bool PinyinEngine::Consume(size_t index){Rebuild();if(index>=count_)return false;size_t used=consumed_[index];
    std::memmove(input_,input_+used,length_-used+1);length_-=used;dirty_=true;return true;
}
size_t PinyinEngine::SegmentBytes(const char *text){
    if(!text||!*text)return 0;size_t best=0;
    for(const auto &p:kPhrases)for(const char *t=p.text;*t;){size_t bytes=std::strcspn(t,"|");
        if(bytes>best && !std::strncmp(text,t,bytes))best=bytes;t+=bytes;if(*t)++t;
    }
    return best?best:Utf8Bytes(text);
}
namespace {
char Digit(char c) {
    const char *groups[] = {"abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
    for (size_t i = 0; i < 8; ++i) if (std::strchr(groups[i], c)) return '2' + i;
    return 0;
}
bool CodePrefix(const char *digits, size_t n, const char *key) {
    for (size_t i = 0; i < n; ++i) if (!key[i] || digits[i] != Digit(key[i])) return false;
    return true;
}
size_t PhraseLength(const char *digits, size_t segment) {
    size_t best = 0;
    for (const auto &entry : kPhrases) {
        size_t n = std::strlen(entry.key);
        if (n <= segment && n > best && CodePrefix(digits, n, entry.key)) best = n;
    }
    return best;
}
bool CopyText(const char *p, size_t bytes, char *out, size_t cap) {
    if (!out || bytes >= cap) return false;
    std::memcpy(out, p, bytes); out[bytes] = 0; return true;
}
}
bool NineKeyEngine::Append(char c) {
    if ((c < '2' || c > '9') && c != '\'') return false;
    if (length_ == PinyinEngine::kMaxInput || (c == '\'' && (!length_ || input_[length_ - 1] == '\''))) return false;
    input_[length_++] = c; input_[length_] = 0; spelling_[0] = 0; return true;
}
bool NineKeyEngine::Backspace() {
    if (!length_) return false;
    input_[--length_] = 0; spelling_[0] = 0; return true;
}
void NineKeyEngine::Clear() { length_ = 0; input_[0] = spelling_[0] = 0; }
size_t NineKeyEngine::SegmentLength() const {
    size_t n = 0; while (n < length_ && input_[n] != '\'') ++n; return n;
}
size_t NineKeyEngine::MatchedLength() const {
    size_t segment = SegmentLength();
    for (size_t n = segment < 6 ? segment : 6; n; --n)
        for (const auto &entry : kDictionary)
            if (std::strlen(entry.key) == n && CodePrefix(input_, n, entry.key)) return n;
    return 0;
}
size_t NineKeyEngine::SpellingCount() const {
    size_t segment = SegmentLength(), matched = MatchedLength(), count = 0;
    if (!segment) return 0;
    for (const auto &entry : kDictionary) {
        size_t n = std::strlen(entry.key);
        if ((matched && n == matched && CodePrefix(input_, matched, entry.key)) ||
            (!matched && n >= segment && CodePrefix(input_, segment, entry.key))) ++count;
    }
    return count;
}
bool NineKeyEngine::Spelling(size_t index, char *out, size_t cap) const {
    size_t segment = SegmentLength(), matched = MatchedLength();
    if (!segment) return false;
    for (const auto &entry : kDictionary) {
        size_t n = std::strlen(entry.key);
        if ((matched && n == matched && CodePrefix(input_, matched, entry.key)) ||
            (!matched && n >= segment && CodePrefix(input_, segment, entry.key))) {
            if (!index--) return CopyText(entry.key, n, out, cap);
        }
    }
    return false;
}
bool NineKeyEngine::SelectSpelling(size_t index) { return Spelling(index, spelling_, sizeof(spelling_)); }
bool NineKeyEngine::Preview(char *out, size_t cap) const {
    if (!out || !cap) return false;
    out[0] = 0;
    size_t segment = SegmentLength();
    if (!segment) return false;
    size_t used = 0;
    if (*spelling_) {
        if (!CopyText(spelling_, std::strlen(spelling_), out, cap)) return false;
        used = std::strlen(spelling_);
    }
    size_t best = PhraseLength(input_, segment);
    for (const auto &entry : kPhrases)
        if (!*out && best && std::strlen(entry.key) == best && CodePrefix(input_, best, entry.key)) {
            if (!CopyText(entry.key, best, out, cap)) return false;
            used = best;
        }
    char key[7];
    if (!*out) {
        for (size_t i = 0; i < 3 && Spelling(i, key, sizeof(key)); ++i) {
            size_t bytes = std::strlen(key);
            if (std::strlen(out) + bytes + (i ? 3 : 0) >= cap) break;
            if (i) std::strcat(out, " / ");
            std::strcat(out, key);
        }
        used = MatchedLength(); if (!used) used = segment;
    }
    if (used < length_) {
        if (input_[used] == '\'') ++used;
        if (used < length_ && std::strlen(out) + 4 + length_ - used < cap) {
            std::strcat(out, " · "); std::strcat(out, input_ + used);
        }
    }
    return *out != 0;
}
bool NineKeyEngine::Candidate(size_t index, char *out, size_t cap, size_t *consumed) const {
    if (out && cap) out[0] = 0;
    size_t segment = SegmentLength(), matched = MatchedLength();
    if (!segment) return false;
    // Exact phrases (or phrase prefixes in a continuous sentence) precede chars.
    size_t phrase_length = spelling_[0] ? 0 : PhraseLength(input_, segment);
    if (phrase_length) for (const auto &entry : kPhrases) {
        size_t n = std::strlen(entry.key);
        if (n != phrase_length || !CodePrefix(input_, n, entry.key)) continue;
        const char *p = entry.text;
        while (*p) {
            size_t bytes = std::strcspn(p, "|");
            if (!index--) {
                if (consumed) *consumed = n + (input_[n] == '\'');
                return CopyText(p, bytes, out, cap);
            }
            p += bytes; if (*p) ++p;
        }
    }
    // Match once, then interleave readings. This bounds paging work even for
    // incomplete one-key input; the small scratch arrays stay on the stack.
    constexpr size_t capacity = sizeof(kDictionary) / sizeof(kDictionary[0]);
    uint16_t ids[capacity], lengths[capacity];
    size_t matches = 0, total = 0, max_rank = 0;
    for (size_t id = 0; id < capacity; ++id) {
        const auto &entry = kDictionary[id];
        size_t n = std::strlen(entry.key);
        bool match = spelling_[0] ? !std::strcmp(spelling_, entry.key) :
            matched ? n == matched && CodePrefix(input_, matched, entry.key) :
            n >= segment && CodePrefix(input_, segment, entry.key);
        if (!match) continue;
        // Generated dictionary contains only three-byte BMP CJK characters.
        size_t count = std::strlen(entry.text) / 3;
        ids[matches] = id; lengths[matches++] = count;
        total += count; if (count > max_rank) max_rank = count;
    }
    if (index >= total) return false;
    for (size_t rank = 0; rank < max_rank; ++rank) {
        for (size_t i = 0; i < matches; ++i) {
            if (rank >= lengths[i]) continue;
            if (!index--) {
                const auto &entry = kDictionary[ids[i]];
                size_t n = std::strlen(entry.key);
                size_t used = n < segment ? n : segment;
                if (consumed) *consumed = used + (input_[used] == '\'');
                return CopyText(entry.text + rank * 3, 3, out, cap);
            }
        }
    }
    return false;
}
size_t NineKeyEngine::Count() const {
    // No candidate cache allocation; count matching entries in one pass.
    size_t segment = SegmentLength(), matched = MatchedLength(), count = 0;
    if (!segment) return 0;
    size_t phrase_length = spelling_[0] ? 0 : PhraseLength(input_, segment);
    if (phrase_length) for (const auto &entry : kPhrases)
        if (std::strlen(entry.key) == phrase_length && CodePrefix(input_, phrase_length, entry.key)) {
            ++count; for (const char *p = entry.text; *p; ++p) if (*p == '|') ++count;
        }
    for (const auto &entry : kDictionary) {
        size_t n = std::strlen(entry.key);
        bool match = spelling_[0] ? !std::strcmp(spelling_, entry.key) :
            matched ? n == matched && CodePrefix(input_, matched, entry.key) :
            n >= segment && CodePrefix(input_, segment, entry.key);
        if (match) for (const char *p = entry.text; *p; p += Utf8Bytes(p)) ++count;
    }
    return count;
}
bool NineKeyEngine::Consume(size_t index) {
    size_t used = 0; char text[PinyinEngine::kCandidateBytes];
    if (!Candidate(index, text, sizeof(text), &used) || !used) return false;
    std::memmove(input_, input_ + used, length_ - used + 1);
    length_ -= used; spelling_[0] = 0; return true;
}
}
