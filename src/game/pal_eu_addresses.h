// Addresses for the second European PAL executable.
#pragma once
#include <cstdint>

// Guest addresses in this image that replace the USA build's constants.
constexpr uint32_t TC_ADDR(uint32_t usa) {
    switch (usa) {
        case 0x00016E2Au: return 0x00016E4Au;
        case 0x000173D5u: return 0x000173F5u;
        case 0x0001CC6Cu: return 0x0001CC8Cu;
        case 0x0001E492u: return 0x0001E4B2u;
        case 0x00061A16u: return 0x00061A96u;
        case 0x0007E1BAu: return 0x0007E2DAu;
        case 0x0007EAC0u: return 0x0007EBE0u;
        case 0x0007F510u: return 0x0007F650u;
        case 0x000802E0u: return 0x00080420u;
        case 0x000854A0u: return 0x000855C0u;
        case 0x0008746Au: return 0x0008758Au;
        case 0x0008748Du: return 0x000875ADu;
        case 0x0009F460u: return 0x0009F580u;
        case 0x000A5221u: return 0x000A5411u;
        case 0x000A7558u: return 0x000A7798u;
        case 0x000A75DDu: return 0x000A781Du;
        case 0x000A7910u: return 0x000A7CA9u;
        case 0x000A794Bu: return 0x000A7D41u;
        case 0x000A7952u: return 0x000A7D48u;
        case 0x00110AD0u: return 0x00110FF0u;
        case 0x00137600u: return 0x00137B40u;
        case 0x001388AFu: return 0x00138DEFu;
        case 0x00138B90u: return 0x001390D0u;
        case 0x0013A1B0u: return 0x0013A6F0u;
        case 0x0013A480u: return 0x0013A9C0u;
        case 0x0013A850u: return 0x0013AD90u;
        case 0x0013ABB0u: return 0x0013B0F0u;
        case 0x0013C180u: return 0x0013C6C0u;
        case 0x0013C1D0u: return 0x0013C710u;
        case 0x0013C4C0u: return 0x0013CA00u;
        case 0x0013CB50u: return 0x0013D090u;
        case 0x0013CF40u: return 0x0013D480u;
        case 0x0013D350u: return 0x0013D890u;
        case 0x0013D750u: return 0x0013DC90u;
        case 0x0013E7F0u: return 0x0013ED30u;
        case 0x0013E91Du: return 0x0013EE5Du;
        case 0x0013EB08u: return 0x0013F048u;
        case 0x0013F188u: return 0x0013F6C8u;
        case 0x0013F2BBu: return 0x0013F7FBu;
        case 0x0013F3DAu: return 0x0013F91Au;
        case 0x0013F5CAu: return 0x0013FB0Au;
        case 0x0013F7C2u: return 0x0013FD02u;
        case 0x0013F840u: return 0x0013FD80u;
        case 0x0013FBE0u: return 0x00140120u;
        case 0x001400DAu: return 0x0014061Au;
        case 0x001420A0u: return 0x001425E0u;
        case 0x00142C90u: return 0x00143160u;
        case 0x00142CEEu: return 0x001431BEu;
        case 0x0014396Fu: return 0x00143EAFu;
        case 0x00144250u: return 0x00144790u;
        case 0x0014475Au: return 0x00144C9Au;
        case 0x00145350u: return 0x00145890u;
        case 0x00145AC6u: return 0x00146006u;
        case 0x00146CA0u: return 0x001471E0u;
        case 0x00146CC8u: return 0x00147208u;
        case 0x00146D66u: return 0x001472A6u;
        case 0x00146FC0u: return 0x00147500u;
        case 0x00146FE8u: return 0x00147528u;
        case 0x001472E0u: return 0x00147820u;
        case 0x00147308u: return 0x00147848u;
        case 0x0014A240u: return 0x0014A780u;
        case 0x0014F460u: return 0x0014F9A0u;
        case 0x00150034u: return 0x00150574u;
        case 0x0016FCC0u: return 0x00170200u;
        case 0x00191650u: return 0x00191B90u;
        case 0x00191800u: return 0x00191D40u;
        case 0x001B50ECu: return 0x001B57ECu;
        case 0x001CCC46u: return 0x001CD346u;
        case 0x001CCC72u: return 0x001CD372u;
        case 0x001CF101u: return 0x001CF801u;
        case 0x0021CF50u: return 0x0021D510u;
        case 0x0021D0E0u: return 0x0021D6A0u;
        case 0x0021DC00u: return 0x0021E1C0u;
        case 0x0021E1A0u: return 0x0021E760u;
        case 0x0021EC70u: return 0x0021F230u;
        case 0x0023F2D8u: return 0x0023F868u;
        case 0x00240D22u: return 0x002412B2u;
        case 0x00240E62u: return 0x002413F2u;
        case 0x00240F74u: return 0x00241504u;
        case 0x00240F8Eu: return 0x0024151Eu;
        case 0x0024BAB0u: return 0x0024C070u;
        case 0x00250E7Fu: return 0x002514AFu;
        case 0x00251138u: return 0x00251768u;
        case 0x002C8F50u: return 0x002C95F0u;
        case 0x002D4A48u: return 0x002D50F0u;
        case 0x002D4A58u: return 0x002D5100u;
        case 0x002D4A6Cu: return 0x002D5114u;
        case 0x002D4A7Cu: return 0x002D5124u;
        case 0x002D5A7Cu: return 0x002D6124u;
        case 0x002D9C44u: return 0x002DA2F4u;
        case 0x002D9C48u: return 0x002DA2F8u;
        case 0x002D9C4Cu: return 0x002DA2FCu;
        case 0x002D9C50u: return 0x002DA300u;
        case 0x002D9E0Cu: return 0x002DA4BCu;
        case 0x002D9E10u: return 0x002DA4C0u;
        case 0x002D9E14u: return 0x002DA4C4u;
        case 0x002D9E18u: return 0x002DA4C8u;
        case 0x002E78BCu: return 0x002E7F7Cu;
        case 0x002E78C0u: return 0x002E7F80u;
        case 0x002E7C5Cu: return 0x002E831Cu;
        case 0x002EA780u: return 0x002EAE40u;
        case 0x002EA794u: return 0x002EAE54u;
        case 0x002EA7B8u: return 0x002EAE78u;
        case 0x002EA7CCu: return 0x002EAE8Cu;
        case 0x002EA7E0u: return 0x002EAEA0u;
        case 0x002EA7E8u: return 0x002EAEA8u;
        case 0x002EA7F4u: return 0x002EAEB4u;
        case 0x002EA808u: return 0x002EAEC8u;
        case 0x002EA81Cu: return 0x002EAEDCu;
        case 0x002EA840u: return 0x002EAF00u;
        case 0x002EA854u: return 0x002EAF14u;
        case 0x002EA860u: return 0x002EAF20u;
        case 0x002EA868u: return 0x002EAF28u;
        case 0x002EA87Cu: return 0x002EAF3Cu;
        case 0x002EA890u: return 0x002EAF50u;
        case 0x002EA8A4u: return 0x002EAF64u;
        case 0x002EA8B8u: return 0x002EAF78u;
        case 0x002EA8CCu: return 0x002EAF8Cu;
        case 0x002EA8E0u: return 0x002EAFA0u;
        case 0x002EA8F4u: return 0x002EAFB4u;
        case 0x002EA908u: return 0x002EAFC8u;
        case 0x002EA920u: return 0x002EAFE0u;
        case 0x002EA934u: return 0x002EAFF4u;
        case 0x002EA980u: return 0x002EB040u;
        case 0x002EA9B8u: return 0x002EB078u;
        case 0x002EA9CCu: return 0x002EB08Cu;
        case 0x002EA9F0u: return 0x002EB0B0u;
        case 0x002ECDB0u: return 0x002ED470u;
        case 0x002F01B0u: return 0x002F0870u;
        case 0x002F01B4u: return 0x002F0874u;
        case 0x003376C8u: return 0x00337D88u;
        case 0x00348988u: return 0x00349148u;
        case 0x00348A80u: return 0x00349240u;
        case 0x00348B20u: return 0x003492E0u;
        case 0x00348B9Cu: return 0x0034935Cu;
        case 0x00348C24u: return 0x003493E4u;
        case 0x00348E10u: return 0x003495D0u;
        case 0x00349380u: return 0x00349B40u;
        case 0x003493CCu: return 0x00349B8Cu;
        case 0x00351544u: return 0x00351D04u;
        case 0x00351558u: return 0x00351D18u;
        case 0x0035C30Cu: return 0x0035CACCu;
        case 0x0035C310u: return 0x0035CAD0u;
        case 0x00360DD0u: return 0x0036158Cu;
        case 0x00360DE0u: return 0x0036159Cu;
        case 0x00360E00u: return 0x003615BCu;
        case 0x00360E0Cu: return 0x003615CCu;
        case 0x00363D0Cu: return 0x003644CCu;
        case 0x00363D38u: return 0x003644F8u;
        case 0x00363D3Cu: return 0x003644FCu;
        case 0x00363D44u: return 0x00364504u;
        case 0x00363D50u: return 0x00364510u;
        case 0x0036FE80u: return 0x00370640u;
        case 0x00373554u: return 0x00373D24u;
        case 0x0037493Cu: return 0x0037510Cu;
        case 0x0037BE08u: return 0x0037C5D8u;
        case 0x0037BE28u: return 0x0037C5F8u;
        case 0x0037D284u: return 0x0037DA54u;
        case 0x0037D33Cu: return 0x0037DB0Cu;
        case 0x0037D340u: return 0x0037DB10u;
        case 0x0037D348u: return 0x0037DB18u;
        case 0x0037F630u: return 0x0037FE00u;
        case 0x0037F8BCu: return 0x0038008Cu;
        case 0x0037F8D0u: return 0x003800A0u;
        case 0x0037F8F0u: return 0x003800C0u;
        case 0x0037F8FCu: return 0x003800CCu;
        case 0x0037F940u: return 0x00380110u;
        case 0x003EE31Au: return 0x003EEAEAu;
        case 0x003F54CCu: return 0x003F5C9Cu;
        case 0x003F58E0u: return 0x003F60B0u;
        case 0x00400000u: return 0x00400000u;
        case 0x00420468u: return 0x00420C38u;
        case 0x004204ECu: return 0x00420CBCu;  // input state: right stick bytes at +0x70
        case 0x004C632Cu: return 0x004C6B2Cu;
        case 0x004C6380u: return 0x004C6B80u;
        case 0x004D48A4u: return 0x004D50A4u;
        case 0x004D48A8u: return 0x004D50A8u;
        // Manual cases: data and guest constants the symbol scanner does not alias.
        // Keep these when regenerating the table.
        case 0x00018E46u: return 0x00018E66u;  // 0x15420 return in the 0x18C80 spawn check
        case 0x0001C8A0u: return 0x0001C8C0u;
        case 0x0001CE10u: return 0x0001CE30u;
        case 0x0001E370u: return 0x0001E390u;
        case 0x000C15AEu: return 0x000C195Eu;  // 0xAFF30 return after the Music Player list build
        case 0x00081CC0u: return 0x00081DE0u;
        case 0x000828F0u: return 0x00082A10u;
        case 0x00144670u: return 0x00144BB0u;  // 0x137C00 return in the instance draw cull
        case 0x00148D10u: return 0x00149250u;  // tile instance walk
        case 0x0014A73Bu: return 0x0014AC7Bu;  // 0x148D10 return in 0x14A240
        case 0x0014BD60u: return 0x0014C2A0u;
        case 0x0014C0D0u: return 0x0014C610u;
        case 0x0014C150u: return 0x0014C690u;
        case 0x002D3D9Cu: return 0x002D443Cu;
        case 0x002D3DB4u: return 0x002D4454u;
        case 0x002D9654u: return 0x002D9D04u;  // ambient traffic car vtable
        case 0x002E3500u: return 0x002E3BC0u;  // traffic despawn/spawn limits
        case 0x002E3504u: return 0x002E3BC4u;
        case 0x002E4110u: return 0x002E47D0u;
        case 0x003A7860u: return 0x003A8030u;
        default: return usa;
    }
}

// Recompiled function names in this image, so the host hooks attach to the right bodies.
#define sub_00013D90 sub_00013D90
#define __imp__sub_00013D90 __imp__sub_00013D90
#define sub_00014120 sub_00014120
#define __imp__sub_00014120 __imp__sub_00014120
#define sub_00014170 sub_00014170
#define __imp__sub_00014170 __imp__sub_00014170
#define sub_00014990 sub_00014990
#define __imp__sub_00014990 __imp__sub_00014990
#define sub_00014C30 sub_00014C30
#define __imp__sub_00014C30 __imp__sub_00014C30
#define sub_000158F0 sub_000158F0
#define __imp__sub_000158F0 __imp__sub_000158F0
#define sub_000168F0 sub_00016910
#define __imp__sub_000168F0 __imp__sub_00016910
#define sub_00017BE0 sub_00017C00
#define __imp__sub_00017BE0 __imp__sub_00017C00
#define sub_00018C80 sub_00018CA0
#define __imp__sub_00018C80 __imp__sub_00018CA0
#define sub_00019230 sub_00019250
#define __imp__sub_00019230 __imp__sub_00019250
#define sub_0001F030 sub_0001F050
#define __imp__sub_0001F030 __imp__sub_0001F050
#define sub_0001FD40 sub_0001FD60
#define __imp__sub_0001FD40 __imp__sub_0001FD60
#define sub_000205A0 sub_000205C0
#define __imp__sub_000205A0 __imp__sub_000205C0
#define sub_00021C10 sub_00021C30
#define __imp__sub_00021C10 __imp__sub_00021C30
#define sub_00024040 sub_00024060
#define __imp__sub_00024040 __imp__sub_00024060
#define sub_00025910 sub_00025930
#define __imp__sub_00025910 __imp__sub_00025930
#define sub_00029A40 sub_00029A60
#define __imp__sub_00029A40 __imp__sub_00029A60
#define sub_0002A7B0 sub_0002A7D0
#define __imp__sub_0002A7B0 __imp__sub_0002A7D0
#define sub_0002AAF0 sub_0002AB10
#define __imp__sub_0002AAF0 __imp__sub_0002AB10
#define sub_0001FCA0 sub_0001FCC0
#define __imp__sub_0001FCA0 __imp__sub_0001FCC0
#define sub_00027380 sub_000273A0
#define __imp__sub_00027380 __imp__sub_000273A0
#define sub_000225B0 sub_000225D0
#define __imp__sub_000225B0 __imp__sub_000225D0
#define sub_0002CFC0 sub_0002CFF0
#define __imp__sub_0002CFC0 __imp__sub_0002CFF0
#define sub_0002E200 sub_0002E220
#define __imp__sub_0002E200 __imp__sub_0002E220
#define sub_00058D90 sub_00058E00
#define __imp__sub_00058D90 __imp__sub_00058E00
#define sub_00068300 sub_000683A0
#define __imp__sub_00068300 __imp__sub_000683A0
#define sub_000703D0 sub_000704F0
#define __imp__sub_000703D0 __imp__sub_000704F0
#define sub_00074600 sub_00074720
#define __imp__sub_00074600 __imp__sub_00074720
#define sub_0007D820 sub_0007D940
#define __imp__sub_0007D820 __imp__sub_0007D940
#define sub_0007DA50 sub_0007DB70
#define __imp__sub_0007DA50 __imp__sub_0007DB70
#define sub_0007DBD0 sub_0007DCF0
#define __imp__sub_0007DBD0 __imp__sub_0007DCF0
#define sub_0007DD80 sub_0007DEA0
#define __imp__sub_0007DD80 __imp__sub_0007DEA0
#define sub_000823C0 sub_000824E0
#define __imp__sub_000823C0 __imp__sub_000824E0
#define sub_00083240 sub_00083360
#define __imp__sub_00083240 __imp__sub_00083360
#define sub_00087350 sub_00087470
#define __imp__sub_00087350 __imp__sub_00087470
#define sub_000A5200 sub_000A53F0
#define __imp__sub_000A5200 __imp__sub_000A53F0
#define sub_000A73F0 sub_000A7630
#define __imp__sub_000A73F0 __imp__sub_000A7630
#define sub_000AD5C0 sub_000AD980
#define __imp__sub_000AD5C0 __imp__sub_000AD980
#define sub_000B13C0 sub_000B1770
#define __imp__sub_000B13C0 __imp__sub_000B1770
#define sub_000B14F0 sub_000B18A0
#define __imp__sub_000B14F0 __imp__sub_000B18A0
#define sub_000B6400 sub_000B67F0
#define __imp__sub_000B6400 __imp__sub_000B67F0
#define sub_000C7B80 sub_000C7F40
#define __imp__sub_000C7B80 __imp__sub_000C7F40
#define sub_000CB400 sub_000CB7C0
#define __imp__sub_000CB400 __imp__sub_000CB7C0
#define sub_000DB4F0 sub_000DB9A0
#define __imp__sub_000DB4F0 __imp__sub_000DB9A0
#define sub_000F9C60 sub_000FA260
#define sub_0009DDB0 sub_0009DED0
#define __imp__sub_0009DDB0 __imp__sub_0009DED0
#define __imp__sub_000F9C60 __imp__sub_000FA260
#define sub_000FD220 sub_000FD760
#define __imp__sub_000FD220 __imp__sub_000FD760
#define sub_00136A70 sub_00136FB0
#define __imp__sub_00136A70 __imp__sub_00136FB0
#define sub_00136DB0 sub_001372F0
#define __imp__sub_00136DB0 __imp__sub_001372F0
#define sub_00137600 sub_00137B40
#define __imp__sub_00137600 __imp__sub_00137B40
#define sub_001378F0 sub_00137E30
#define __imp__sub_001378F0 __imp__sub_00137E30
#define sub_00138070 sub_001385B0
#define __imp__sub_00138070 __imp__sub_001385B0
#define sub_001387F0 sub_00138D30
#define __imp__sub_001387F0 __imp__sub_00138D30
#define sub_00138B90 sub_001390D0
#define __imp__sub_00138B90 __imp__sub_001390D0
#define sub_0013A1B0 sub_0013A6F0
#define __imp__sub_0013A1B0 __imp__sub_0013A6F0
#define sub_0013A480 sub_0013A9C0
#define __imp__sub_0013A480 __imp__sub_0013A9C0
#define sub_0013A850 sub_0013AD90
#define __imp__sub_0013A850 __imp__sub_0013AD90
#define sub_0013ABB0 sub_0013B0F0
#define __imp__sub_0013ABB0 __imp__sub_0013B0F0
#define sub_0013B460 sub_0013B9A0
#define __imp__sub_0013B460 __imp__sub_0013B9A0
#define sub_0013C390 sub_0013C8D0
#define __imp__sub_0013C390 __imp__sub_0013C8D0
#define sub_0013C4C0 sub_0013CA00
#define __imp__sub_0013C4C0 __imp__sub_0013CA00
#define sub_0013C8F0 sub_0013CE30
#define __imp__sub_0013C8F0 __imp__sub_0013CE30
#define sub_0013CB50 sub_0013D090
#define __imp__sub_0013CB50 __imp__sub_0013D090
#define sub_0013CF40 sub_0013D480
#define __imp__sub_0013CF40 __imp__sub_0013D480
#define sub_0013D100 sub_0013D640
#define __imp__sub_0013D100 __imp__sub_0013D640
#define sub_0013D350 sub_0013D890
#define __imp__sub_0013D350 __imp__sub_0013D890
#define sub_0013D750 sub_0013DC90
#define __imp__sub_0013D750 __imp__sub_0013DC90
#define sub_0013D880 sub_0013DDC0
#define __imp__sub_0013D880 __imp__sub_0013DDC0
#define sub_0013E4A0 sub_0013E9E0
#define __imp__sub_0013E4A0 __imp__sub_0013E9E0
#define sub_0013E7F0 sub_0013ED30
#define __imp__sub_0013E7F0 __imp__sub_0013ED30
#define sub_0013E91D sub_0013EE5D
#define __imp__sub_0013E91D __imp__sub_0013EE5D
#define sub_0013EB08 sub_0013F048
#define __imp__sub_0013EB08 __imp__sub_0013F048
#define sub_0013EF50 sub_0013F490
#define __imp__sub_0013EF50 __imp__sub_0013F490
#define sub_0013EFB0 sub_0013F4F0
#define __imp__sub_0013EFB0 __imp__sub_0013F4F0
#define sub_0013F090 sub_0013F5D0
#define __imp__sub_0013F090 __imp__sub_0013F5D0
#define sub_0013F4C0 sub_0013FA00
#define __imp__sub_0013F4C0 __imp__sub_0013FA00
#define sub_0013FBE0 sub_00140120
#define __imp__sub_0013FBE0 __imp__sub_00140120
#define sub_001400DA sub_0014061A
#define __imp__sub_001400DA __imp__sub_0014061A
#define sub_00141660 sub_00141BA0
#define __imp__sub_00141660 __imp__sub_00141BA0
#define sub_00141D40 sub_00142280
#define __imp__sub_00141D40 __imp__sub_00142280
#define sub_001420A0 sub_001425E0
#define __imp__sub_001420A0 __imp__sub_001425E0
#define sub_00142C90 sub_00143160
#define __imp__sub_00142C90 __imp__sub_00143160
#define sub_00142CE0 sub_001431B0
#define __imp__sub_00142CE0 __imp__sub_001431B0
#define sub_00142DB0 sub_001432F0
#define __imp__sub_00142DB0 __imp__sub_001432F0
#define sub_00143650 sub_00143B90
#define __imp__sub_00143650 __imp__sub_00143B90
#define sub_00144250 sub_00144790
#define __imp__sub_00144250 __imp__sub_00144790
#define sub_00145350 sub_00145890
#define __imp__sub_00145350 __imp__sub_00145890
#define sub_00145AC6 sub_00146006
#define __imp__sub_00145AC6 __imp__sub_00146006
#define sub_00146CA0 sub_001471E0
#define __imp__sub_00146CA0 __imp__sub_001471E0
#define sub_00146CC8 sub_00147208
#define __imp__sub_00146CC8 __imp__sub_00147208
#define sub_00146D66 sub_001472A6
#define __imp__sub_00146D66 __imp__sub_001472A6
#define sub_00146FC0 sub_00147500
#define __imp__sub_00146FC0 __imp__sub_00147500
#define sub_00146FE8 sub_00147528
#define __imp__sub_00146FE8 __imp__sub_00147528
#define sub_001472E0 sub_00147820
#define __imp__sub_001472E0 __imp__sub_00147820
#define sub_00147308 sub_00147848
#define __imp__sub_00147308 __imp__sub_00147848
#define sub_00148A00 sub_00148F40
#define __imp__sub_00148A00 __imp__sub_00148F40
#define sub_0014A240 sub_0014A780
#define __imp__sub_0014A240 __imp__sub_0014A780
#define sub_0014B390 sub_0014B8D0
#define __imp__sub_0014B390 __imp__sub_0014B8D0
#define sub_0014F460 sub_0014F9A0
#define __imp__sub_0014F460 __imp__sub_0014F9A0
#define sub_001785A0 sub_00178AE0
#define __imp__sub_001785A0 __imp__sub_00178AE0
#define __imp__sub_001785D0 __imp__sub_00178B10
#define sub_001806B0 sub_00180BF0
#define __imp__sub_001806B0 __imp__sub_00180BF0
#define sub_0017FA70 sub_0017FFB0
#define __imp__sub_0017FA70 __imp__sub_0017FFB0
#define sub_00182830 sub_00182D70
#define __imp__sub_00182830 __imp__sub_00182D70
#define sub_001828C0 sub_00182E00
#define __imp__sub_001828C0 __imp__sub_00182E00
#define sub_00185D80 sub_001862C0
#define __imp__sub_00185D80 __imp__sub_001862C0
#define sub_00185D90 sub_001862D0
#define __imp__sub_00185D90 __imp__sub_001862D0
#define sub_0019D3B0 sub_0019D900
#define __imp__sub_0019D3B0 __imp__sub_0019D900
#define sub_001B1C60 sub_001B2360
#define __imp__sub_001B1C60 __imp__sub_001B2360
#define sub_001B3C00 sub_001B4300
#define __imp__sub_001B3C00 __imp__sub_001B4300
#define sub_001B4F20 sub_001B5620
#define __imp__sub_001B4F20 __imp__sub_001B5620
#define sub_001CA180 sub_001CA880
#define __imp__sub_001CA180 __imp__sub_001CA880
#define sub_0021C330 sub_0021C8F0
#define __imp__sub_0021C330 __imp__sub_0021C8F0
#define sub_0021C520 sub_0021CAE0
#define __imp__sub_0021C520 __imp__sub_0021CAE0
#define sub_0021C570 sub_0021CB30
#define __imp__sub_0021C570 __imp__sub_0021CB30
#define sub_0021C5C0 sub_0021CB80
#define __imp__sub_0021C5C0 __imp__sub_0021CB80
#define sub_0021C600 sub_0021CBC0
#define __imp__sub_0021C600 __imp__sub_0021CBC0
#define sub_0021C640 sub_0021CC00
#define __imp__sub_0021C640 __imp__sub_0021CC00
#define sub_0021C6D0 sub_0021CC90
#define __imp__sub_0021C6D0 __imp__sub_0021CC90
#define sub_0021C800 sub_0021CDC0
#define __imp__sub_0021C800 __imp__sub_0021CDC0
#define sub_0021CF50 sub_0021D510
#define __imp__sub_0021CF50 __imp__sub_0021D510
#define sub_0021DC00 sub_0021E1C0
#define __imp__sub_0021DC00 __imp__sub_0021E1C0
#define sub_0021E1A0 sub_0021E760
#define __imp__sub_0021E1A0 __imp__sub_0021E760
#define sub_0021FC00 sub_002201C0
#define __imp__sub_0021FC00 __imp__sub_002201C0
#define sub_002219E0 sub_00221FA0
#define __imp__sub_002219E0 __imp__sub_00221FA0
#define sub_002291F0 sub_00229730
#define __imp__sub_002291F0 __imp__sub_00229730
#define __imp__sub_00229120 __imp__sub_00229660
#define sub_00229D50 sub_0022A2B0
#define __imp__sub_00229D50 __imp__sub_0022A2B0
#define sub_00240003 sub_00240593
#define __imp__sub_00240003 __imp__sub_00240593
#define sub_00240CC0 sub_00241250
#define __imp__sub_00240CC0 __imp__sub_00241250
#define sub_00242F20 sub_00243500
#define __imp__sub_00242F20 __imp__sub_00243500
#define sub_00242F30 sub_00243510
#define __imp__sub_00242F30 __imp__sub_00243510
#define sub_00243000 sub_002435E0
#define __imp__sub_00243000 __imp__sub_002435E0
#define sub_002453C0 sub_002459A0
#define __imp__sub_002453C0 __imp__sub_002459A0
#define sub_00245BB0 sub_00246190
#define __imp__sub_00245BB0 __imp__sub_00246190
#define sub_00245CE0 sub_002462C0
#define __imp__sub_00245CE0 __imp__sub_002462C0
#define sub_00245EF0 sub_002464D0
#define __imp__sub_00245EF0 __imp__sub_002464D0
#define sub_0024C020 sub_0024C620
#define __imp__sub_0024C020 __imp__sub_0024C620
#define sub_0024C6E0 sub_0024CCE0
#define __imp__sub_0024C6E0 __imp__sub_0024CCE0
#define sub_0024CEC0 sub_0024D4C0
#define __imp__sub_0024CEC0 __imp__sub_0024D4C0
#define sub_00250170 sub_002507A0
#define __imp__sub_00250170 __imp__sub_002507A0
#define sub_00258EB0 sub_002594E0
#define __imp__sub_00258EB0 __imp__sub_002594E0
#define sub_0025DA90 sub_0025E0C0
#define __imp__sub_0025DA90 __imp__sub_0025E0C0
#define sub_000AFF30 sub_000B02E0
#define __imp__sub_000AFF30 __imp__sub_000B02E0
#define sub_00079A60 sub_00079B80
#define __imp__sub_00079A60 __imp__sub_00079B80
#define sub_000B8740 sub_000B8B30
#define __imp__sub_000B8740 __imp__sub_000B8B30
#define sub_000B89F0 sub_000B8DE0
#define __imp__sub_000B89F0 __imp__sub_000B8DE0
#define sub_00260710 sub_00260D40
#define __imp__sub_00260710 __imp__sub_00260D40
#define sub_0025DE90 sub_0025E4C0
#define __imp__sub_0025DE90 __imp__sub_0025E4C0
#define sub_0029F14F sub_0029F7EF
#define __imp__sub_0029F14F __imp__sub_0029F7EF
#define sub_0029F21F sub_0029F8BF
#define __imp__sub_0029F21F __imp__sub_0029F8BF
#define sub_0029F25C sub_0029F8FC
#define __imp__sub_0029F25C __imp__sub_0029F8FC
#define sub_0029F261 sub_0029F901
#define __imp__sub_0029F261 __imp__sub_0029F901
#define sub_0029F296 sub_0029F936
#define __imp__sub_0029F296 __imp__sub_0029F936
#define sub_0029F2E0 sub_0029F980
#define __imp__sub_0029F2E0 __imp__sub_0029F980
#define sub_0029F31A sub_0029F9BA
#define __imp__sub_0029F31A __imp__sub_0029F9BA
#define sub_0029F380 sub_0029FA20
#define __imp__sub_0029F380 __imp__sub_0029FA20
#define sub_0029F4C5 sub_0029FB65
#define __imp__sub_0029F4C5 __imp__sub_0029FB65
#define sub_0029F4E3 sub_0029FB83
#define __imp__sub_0029F4E3 __imp__sub_0029FB83
#define sub_0029F547 sub_0029FBE7
#define __imp__sub_0029F547 __imp__sub_0029FBE7
#define sub_0029F637 sub_0029FCD7
#define __imp__sub_0029F637 __imp__sub_0029FCD7
#define sub_0029F66D sub_0029FD0D
#define __imp__sub_0029F66D __imp__sub_0029FD0D
#define sub_00250580 sub_00250BB0
#define __imp__sub_00250580 __imp__sub_00250BB0
#define sub_0025F100 sub_0025F730
#define __imp__sub_0025F100 __imp__sub_0025F730
#define sub_0026381D sub_00263E67
#define __imp__sub_0026381D __imp__sub_00263E67
#define sub_002688F9 sub_00268F79
#define __imp__sub_002688F9 __imp__sub_00268F79
#define sub_00269405 sub_00269A85
#define __imp__sub_00269405 __imp__sub_00269A85
#define sub_00293541 sub_00293BF1
#define __imp__sub_00293541 __imp__sub_00293BF1
#define sub_0029366D sub_00293D1D
#define __imp__sub_0029366D __imp__sub_00293D1D
#define sub_002BEDE0 sub_002BF480
#define __imp__sub_002BEDE0 __imp__sub_002BF480
#define sub_00015420 sub_00015420
#define __imp__sub_00015420 __imp__sub_00015420
#define sub_00044090 sub_000440B0
#define __imp__sub_00044090 __imp__sub_000440B0
#define sub_00137C00 sub_00138140
#define __imp__sub_00137C00 __imp__sub_00138140
#define sub_00148D10 sub_00149250
#define __imp__sub_00148D10 __imp__sub_00149250

#define sub_001400D0 sub_0013FD80
#define __imp__sub_001400D0 __imp__sub_0013FD80
