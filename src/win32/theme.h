// theme.h — 界面配色单一来源。
//
// 取自经过校验的参考调色板(CVD 色盲安全、对比度达标),不是随手挑的颜色。
// 全部按**角色**命名 —— 代码里只写角色,不写裸 hex,改主题只改这里。
//
// 硬规矩(照做,别绕):
//  1. 文字永远用 ink 系(primary/secondary/muted；强调底用 onAccent),**绝不用数据色**
//     —— 浅色系列色(黄/青)当文字在浅底上根本看不清。身份靠文字**旁边**的色块承载。
//  2. status 四色(good/warning/serious/critical)是保留色,不得挪用作"第 N 条序列"。
//     浅底上 warning/serious 对比度不足 3:1 是**设计如此** —— 必须配文字标签,不能只靠颜色表意。
//  3. 网格线/坐标轴:一档灰、1px 实线、退让。**不要虚线**。
//  4. 绝不画双 Y 轴。两个量纲不同的量 → 两张图。
#pragma once
#include <windows.h>

// hex 直译成 COLORREF(GDI 是 BGR 序,RGB() 宏已处理)
#define HEX2RGB(h) RGB(((h) >> 16) & 0xFF, ((h) >> 8) & 0xFF, (h) & 0xFF)

namespace th {

inline bool dark = false;
inline bool highContrast = false;

// ---- 表面 ----
inline COLORREF surface  = HEX2RGB(0xffffff);  // 卡片/输入/内容表面
inline COLORREF page     = HEX2RGB(0xf5f7fa);  // 页面底板
inline COLORREF nav      = HEX2RGB(0xf0f3f8);  // 导航底板
inline COLORREF zebra    = HEX2RGB(0xf7f9fb);  // 表格斑马纹
inline COLORREF border   = HEX2RGB(0xe1e6ed);  // 发丝描边
inline COLORREF hover    = HEX2RGB(0xe8edf5);  // 中性悬停
inline COLORREF accentSoft = HEX2RGB(0xdceafd); // 选中导航/强调浅底
inline COLORREF accent   = HEX2RGB(0x1769d2);  // 交互强调色
inline COLORREF accentHover = HEX2RGB(0x0f5dbf);

// ---- 文字(唯一允许承载文字的颜色) ----
inline COLORREF inkPri   = HEX2RGB(0x161a21);  // 主要文字/hero 数字
inline COLORREF inkSec   = HEX2RGB(0x4d5868);  // 次要文字
inline COLORREF inkMuted = HEX2RGB(0x7a8696);  // 轴标签/弱化说明
inline COLORREF onAccent = HEX2RGB(0xffffff);

// ---- 图表骨架 ----
inline COLORREF grid     = HEX2RGB(0xe5e9ef);  // 网格发丝线(实线)
inline COLORREF axis     = HEX2RGB(0xb8c1cd);  // 基线/坐标轴

// ---- 状态色(保留,不得当序列色用;须配文字标签) ----
inline COLORREF good     = HEX2RGB(0x15803d);
inline COLORREF warning  = HEX2RGB(0xe6a000);
inline COLORREF serious  = HEX2RGB(0xdf754d);
inline COLORREF critical = HEX2RGB(0xc93c43);

// ---- 分类序列色(固定顺序,不得循环取用) ----
// 顺序本身就是 CVD 安全机制,不是审美排序 —— 别重排。
inline COLORREF s1_blue    = HEX2RGB(0x2a78d6);
inline COLORREF s2_green   = HEX2RGB(0x008300);
inline COLORREF s3_magenta = HEX2RGB(0xe87ba4);
inline COLORREF s4_yellow  = HEX2RGB(0xeda100);
inline COLORREF s5_aqua    = HEX2RGB(0x1baf7a);
inline COLORREF s6_orange  = HEX2RGB(0xeb6834);
inline COLORREF s7_violet  = HEX2RGB(0x6656c9);
inline COLORREF s8_red     = HEX2RGB(0xe34948);

// ---- 断网带:critical 的极淡水洗(~10% 不透明度压在 surface 上),不是饱和色块 ----
inline COLORREF outageBand = HEX2RGB(0xf8e5e5);

// ---- 指标表状态底色(必须与单元格文字/列名共同表达,不可只靠颜色) ----
inline COLORREF cellStall  = HEX2RGB(0xffe1f4);  // ΔRX=0
inline COLORREF cellWeak   = HEX2RGB(0xffeec8);  // CSQ<10
inline COLORREF qualityExcellent = HEX2RGB(0xe2f4e8);
inline COLORREF qualityGood      = HEX2RGB(0xe4eefb);
inline COLORREF qualityFair      = HEX2RGB(0xfff3d6);
inline COLORREF qualityPoor      = HEX2RGB(0xf9e3e3);

// ---- 时间线行着色(小字号,需 WCAG≥4.5,与图表/仪表盘的大元素色解耦)----
// 序列色(good/s1_blue/s5_aqua)是为大色块/大字设计的,直接做小字对比度不足
// (good 3.35 / s1_blue 4.42 / s5_aqua 2.82)。这里给行文字用同色相的加深版,
// 大元素仍用原序列色。rowFault/rowErr/rowSdk 原本就达标,沿用。
inline COLORREF rowRecovered = HEX2RGB(0x0a7a32);
inline COLORREF rowFault     = critical;
inline COLORREF rowRoamlink  = HEX2RGB(0x147c5b);
inline COLORREF rowState     = HEX2RGB(0x2369bd);
inline COLORREF rowSdk       = HEX2RGB(0x5b49b7);
inline COLORREF rowWarn      = HEX2RGB(0x836000);
inline COLORREF rowErr       = critical;

inline void ApplyPalette(bool useDark, bool useHighContrast = false) {
    highContrast = useHighContrast;
    dark = useDark;
    if (highContrast) {
        surface = page = nav = zebra = GetSysColor(COLOR_WINDOW);
        border = grid = axis = GetSysColor(COLOR_WINDOWTEXT);
        hover = accentSoft = accent = accentHover = GetSysColor(COLOR_HIGHLIGHT);
        inkPri = inkSec = inkMuted = GetSysColor(COLOR_WINDOWTEXT);
        onAccent = GetSysColor(COLOR_HIGHLIGHTTEXT);
        good = warning = serious = critical = GetSysColor(COLOR_WINDOWTEXT);
        s1_blue = s2_green = s3_magenta = s4_yellow = s5_aqua = s6_orange =
            s7_violet = s8_red = GetSysColor(COLOR_WINDOWTEXT);
        outageBand = cellStall = cellWeak = qualityExcellent = qualityGood =
            qualityFair = qualityPoor = GetSysColor(COLOR_WINDOW);
        rowRecovered = rowFault = rowRoamlink = rowState = rowSdk = rowWarn = rowErr =
            GetSysColor(COLOR_WINDOWTEXT);
        return;
    }
    if (!dark) {
        surface = HEX2RGB(0xffffff); page = HEX2RGB(0xf5f7fa); nav = HEX2RGB(0xf0f3f8);
        zebra = HEX2RGB(0xf7f9fb); border = HEX2RGB(0xe1e6ed); hover = HEX2RGB(0xe8edf5);
        accentSoft = HEX2RGB(0xdceafd); accent = HEX2RGB(0x1769d2); accentHover = HEX2RGB(0x0f5dbf);
        inkPri = HEX2RGB(0x161a21); inkSec = HEX2RGB(0x4d5868); inkMuted = HEX2RGB(0x7a8696);
        onAccent = HEX2RGB(0xffffff); grid = HEX2RGB(0xe5e9ef); axis = HEX2RGB(0xb8c1cd);
        good = HEX2RGB(0x15803d); warning = HEX2RGB(0xe6a000); serious = HEX2RGB(0xdf754d);
        critical = HEX2RGB(0xc93c43); s1_blue = HEX2RGB(0x2a78d6); s2_green = HEX2RGB(0x008300);
        s3_magenta = HEX2RGB(0xe87ba4); s4_yellow = HEX2RGB(0xeda100); s5_aqua = HEX2RGB(0x1baf7a);
        s6_orange = HEX2RGB(0xeb6834); s7_violet = HEX2RGB(0x6656c9); s8_red = HEX2RGB(0xe34948);
        outageBand = HEX2RGB(0xf8e5e5); cellStall = HEX2RGB(0xffe1f4);
        cellWeak = HEX2RGB(0xffeec8);
        qualityExcellent = HEX2RGB(0xe2f4e8); qualityGood = HEX2RGB(0xe4eefb);
        qualityFair = HEX2RGB(0xfff3d6); qualityPoor = HEX2RGB(0xf9e3e3);
        rowRecovered = HEX2RGB(0x0a7a32); rowFault = critical; rowRoamlink = HEX2RGB(0x147c5b);
        rowState = HEX2RGB(0x2369bd); rowSdk = HEX2RGB(0x5b49b7); rowWarn = HEX2RGB(0x836000);
        rowErr = critical;
        return;
    }
    surface = HEX2RGB(0x1c2026); page = HEX2RGB(0x111419); nav = HEX2RGB(0x171b21);
    zebra = HEX2RGB(0x22272e); border = HEX2RGB(0x343b45); hover = HEX2RGB(0x2a3038);
    accentSoft = HEX2RGB(0x19385e); accent = HEX2RGB(0x4c9aff); accentHover = HEX2RGB(0x69aaff);
    inkPri = HEX2RGB(0xf3f5f8); inkSec = HEX2RGB(0xc3cad4); inkMuted = HEX2RGB(0x8e99a8);
    onAccent = HEX2RGB(0x08111f); grid = HEX2RGB(0x303741); axis = HEX2RGB(0x596473);
    good = HEX2RGB(0x49bd73); warning = HEX2RGB(0xf0b945); serious = HEX2RGB(0xf09570);
    critical = HEX2RGB(0xf06c73); s1_blue = HEX2RGB(0x65a7f3); s2_green = HEX2RGB(0x56bb78);
    s3_magenta = HEX2RGB(0xf090b5); s4_yellow = HEX2RGB(0xefb94e); s5_aqua = HEX2RGB(0x4dc79b);
    s6_orange = HEX2RGB(0xf28a60); s7_violet = HEX2RGB(0x9b8cf2); s8_red = HEX2RGB(0xf07174);
    outageBand = HEX2RGB(0x40252a); cellStall = HEX2RGB(0x4b2941);
    cellWeak = HEX2RGB(0x4a3c20);
    qualityExcellent = HEX2RGB(0x1c3b29); qualityGood = HEX2RGB(0x1d344e);
    qualityFair = HEX2RGB(0x463a20); qualityPoor = HEX2RGB(0x44282b);
    rowRecovered = HEX2RGB(0x67ce88); rowFault = critical; rowRoamlink = HEX2RGB(0x59c89d);
    rowState = HEX2RGB(0x75b5ff); rowSdk = HEX2RGB(0xad9eff); rowWarn = HEX2RGB(0xf0bd58);
    rowErr = critical;
}

} // namespace th
