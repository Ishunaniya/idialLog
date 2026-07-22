// theme.h — 界面配色单一来源。
//
// 取自经过校验的参考调色板(CVD 色盲安全、对比度达标),不是随手挑的颜色。
// 全部按**角色**命名 —— 代码里只写角色,不写裸 hex,改主题只改这里。
//
// 硬规矩(照做,别绕):
//  1. 文字永远用 ink 系(primary/secondary/muted),**绝不用数据色**
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

// ---- 表面 ----
inline const COLORREF surface  = HEX2RGB(0xfcfcfb);  // 图表/内容表面
inline const COLORREF page     = HEX2RGB(0xf9f9f7);  // 页面底板(比表面暗一档)
inline const COLORREF zebra    = HEX2RGB(0xf4f4f2);  // 表格斑马纹(隔行浅灰,极淡不抢焦)
inline const COLORREF border   = HEX2RGB(0xe4e4e3);  // 发丝描边(ink 10% 压在 surface 上)

// ---- 文字(唯一允许承载文字的颜色) ----
inline const COLORREF inkPri   = HEX2RGB(0x0b0b0b);  // 主要文字/hero 数字
inline const COLORREF inkSec   = HEX2RGB(0x52514e);  // 次要文字
inline const COLORREF inkMuted = HEX2RGB(0x898781);  // 轴标签/弱化说明

// ---- 图表骨架 ----
inline const COLORREF grid     = HEX2RGB(0xe1e0d9);  // 网格发丝线(实线)
inline const COLORREF axis     = HEX2RGB(0xc3c2b7);  // 基线/坐标轴

// ---- 状态色(保留,不得当序列色用;须配文字标签) ----
inline const COLORREF good     = HEX2RGB(0x0ca30c);
inline const COLORREF warning  = HEX2RGB(0xfab219);
inline const COLORREF serious  = HEX2RGB(0xec835a);
inline const COLORREF critical = HEX2RGB(0xd03b3b);

// ---- 分类序列色(固定顺序,不得循环取用) ----
// 顺序本身就是 CVD 安全机制,不是审美排序 —— 别重排。
inline const COLORREF s1_blue    = HEX2RGB(0x2a78d6);
inline const COLORREF s2_green   = HEX2RGB(0x008300);
inline const COLORREF s3_magenta = HEX2RGB(0xe87ba4);
inline const COLORREF s4_yellow  = HEX2RGB(0xeda100);
inline const COLORREF s5_aqua    = HEX2RGB(0x1baf7a);
inline const COLORREF s6_orange  = HEX2RGB(0xeb6834);
inline const COLORREF s7_violet  = HEX2RGB(0x4a3aa7);
inline const COLORREF s8_red     = HEX2RGB(0xe34948);

// ---- 断网带:critical 的极淡水洗(~10% 不透明度压在 surface 上),不是饱和色块 ----
inline const COLORREF outageBand = HEX2RGB(0xf8e5e5);

// ---- 时间线行着色(小字号,需 WCAG≥4.5,与图表/仪表盘的大元素色解耦)----
// 序列色(good/s1_blue/s5_aqua)是为大色块/大字设计的,直接做小字对比度不足
// (good 3.35 / s1_blue 4.42 / s5_aqua 2.82)。这里给行文字用同色相的加深版,
// 大元素仍用原序列色。rowFault/rowErr/rowSdk 原本就达标,沿用。
inline const COLORREF rowRecovered = HEX2RGB(0x0a880a);   // good 加深:3.35→4.63
inline const COLORREF rowFault     = critical;            // 4.80 达标
inline const COLORREF rowRoamlink  = HEX2RGB(0x14855c);   // s5_aqua 加深:2.82→4.63
inline const COLORREF rowState     = HEX2RGB(0x2975d1);   // s1_blue 加深:4.42→4.61
inline const COLORREF rowSdk       = s7_violet;           // 8.56 达标
inline const COLORREF rowWarn      = HEX2RGB(0x8a6100);   // warning 在浅底上做文字不合格,用同色系深步进
inline const COLORREF rowErr       = critical;

} // namespace th
