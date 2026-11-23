// Generated for the European (PAL) XBE, except for the manual cases marked below.
// Keep those manual cases when regenerating the table.
// Included by src/game/game_addresses.h when TCNYC_PAL is set.
#pragma once
#include <cstdint>

// Guest addresses in this image that replace the USA build's constants.
constexpr uint32_t TC_ADDR(uint32_t usa) {
    switch (usa) {
        case 0x00016E2Au: return 0x00016DFAu;
        case 0x000173D5u: return 0x000173A5u;
        case 0x0001CC6Cu: return 0x0001CCACu;
        case 0x0001E492u: return 0x0001E4D2u;
        case 0x00061A16u: return 0x00061A46u;
        case 0x0007E1BAu: return 0x0007E31Au;
        case 0x0007EAC0u: return 0x0007EC20u;
        case 0x0007F510u: return 0x0007F690u;
        case 0x000802E0u: return 0x00080460u;
        case 0x000854A0u: return 0x00085630u;
        case 0x0008746Au: return 0x000875FAu;
        case 0x0008748Du: return 0x0008761Du;
        case 0x0009F460u: return 0x0009F640u;
        case 0x000A5221u: return 0x000A5501u;
        case 0x000A7558u: return 0x000A7888u;
        case 0x000A75DDu: return 0x000A790Du;
        case 0x000A7910u: return 0x000A7D99u;
        case 0x000A794Bu: return 0x000A7E31u;
        case 0x000A7952u: return 0x000A7E38u;
        case 0x00110AD0u: return 0x001110E0u;
        case 0x00137600u: return 0x00137E90u;
        case 0x001388AFu: return 0x0013913Fu;
        case 0x00138B90u: return 0x00139420u;
        case 0x0013A1B0u: return 0x0013AA40u;
        case 0x0013A480u: return 0x0013AD10u;
        case 0x0013A850u: return 0x0013B0E0u;
        case 0x0013ABB0u: return 0x0013B440u;
        case 0x0013C180u: return 0x0013CA10u;
        case 0x0013C1D0u: return 0x0013CA60u;
        case 0x0013C4C0u: return 0x0013CD50u;
        case 0x0013CB50u: return 0x0013D3E0u;
        case 0x0013CF40u: return 0x0013D7D0u;
        case 0x0013D350u: return 0x0013DBE0u;
        case 0x0013D750u: return 0x0013DFE0u;
        case 0x0013E7F0u: return 0x0013F080u;
        case 0x0013E91Du: return 0x0013F1ADu;
        case 0x0013EB08u: return 0x0013F398u;
        case 0x0013F188u: return 0x0013FA18u;
        case 0x0013F2BBu: return 0x0013FB4Bu;
        case 0x0013F3DAu: return 0x0013FC6Au;
        case 0x0013F5CAu: return 0x0013FE5Au;
        case 0x0013F7C2u: return 0x00140052u;
        case 0x0013F840u: return 0x001400D0u;
        case 0x0013FBE0u: return 0x00140470u;
        case 0x001400DAu: return 0x0014096Au;
        case 0x001420A0u: return 0x00142790u;
        case 0x00142C90u: return 0x00143380u;
        case 0x00142CEEu: return 0x001433DEu;
        case 0x0014396Fu: return 0x0014405Fu;
        case 0x00144250u: return 0x00144940u;
        case 0x0014475Au: return 0x00144E4Au;
        case 0x00145350u: return 0x00145A40u;
        case 0x00145AC6u: return 0x001461B6u;
        case 0x00146CA0u: return 0x00147390u;
        case 0x00146CC8u: return 0x001473B8u;
        case 0x00146D66u: return 0x00147456u;
        case 0x00146FC0u: return 0x001476B0u;
        case 0x00146FE8u: return 0x001476D8u;
        case 0x001472E0u: return 0x001479D0u;
        case 0x00147308u: return 0x001479F8u;
        case 0x0014A240u: return 0x0014A930u;
        case 0x0014F460u: return 0x0014FB50u;
        case 0x00150034u: return 0x00150724u;
        case 0x0016FCC0u: return 0x001703A0u;
        case 0x00191650u: return 0x00191D50u;
        case 0x00191800u: return 0x00191F00u;
        case 0x001B50ECu: return 0x001B594Cu;
        case 0x001BFFFBu: return 0x001C085Bu;
        case 0x001CCC46u: return 0x001CD4A6u;
        case 0x001CCC72u: return 0x001CD4D2u;
        case 0x001CF101u: return 0x001CF961u;
        case 0x0021CF50u: return 0x0021D670u;
        case 0x0021D0E0u: return 0x0021D800u;
        case 0x0021DC00u: return 0x0021E320u;
        case 0x0021E1A0u: return 0x0021E8C0u;
        case 0x0021EC70u: return 0x0021F390u;
        case 0x0023E2A4u: return 0x0023E8E4u;
        case 0x0023E2EBu: return 0x0023E92Bu;
        case 0x0023EBCDu: return 0x0023F20Du;
        case 0x0023F2D8u: return 0x0023F918u;
        case 0x00240D22u: return 0x00241362u;
        case 0x00240E62u: return 0x002414A2u;
        case 0x00240F74u: return 0x002415B4u;
        case 0x00240F8Eu: return 0x002415CEu;
        case 0x0024BAB0u: return 0x0024C140u;
        case 0x00250E7Fu: return 0x0025158Fu;
        case 0x00251138u: return 0x00251848u;
        case 0x002C8F50u: return 0x002C96B0u;
        case 0x002D4A48u: return 0x002D51B0u;
        case 0x002D4A58u: return 0x002D51C0u;
        case 0x002D4A6Cu: return 0x002D51D4u;
        case 0x002D4A7Cu: return 0x002D51E4u;
        case 0x002D5A7Cu: return 0x002D61E4u;
        case 0x002D9C44u: return 0x002DA40Cu;
        case 0x002D9C48u: return 0x002DA410u;
        case 0x002D9C4Cu: return 0x002DA414u;
        case 0x002D9C50u: return 0x002DA418u;
        case 0x002D9E0Cu: return 0x002DA5D4u;
        case 0x002D9E10u: return 0x002DA5D8u;
        case 0x002D9E14u: return 0x002DA5DCu;
        case 0x002D9E18u: return 0x002DA5E0u;
        case 0x002E78BCu: return 0x002E807Cu;
        case 0x002E78C0u: return 0x002E8080u;
        case 0x002E7C5Cu: return 0x002E841Cu;
        case 0x002EA780u: return 0x002EAF40u;
        case 0x002EA794u: return 0x002EAF54u;
        case 0x002EA7B8u: return 0x002EAF78u;
        case 0x002EA7CCu: return 0x002EAF8Cu;
        case 0x002EA7E0u: return 0x002EAFA0u;
        case 0x002EA7E8u: return 0x002EAFA8u;
        case 0x002EA7F4u: return 0x002EAFB4u;
        case 0x002EA808u: return 0x002EAFC8u;
        case 0x002EA81Cu: return 0x002EAFDCu;
        case 0x002EA840u: return 0x002EB000u;
        case 0x002EA854u: return 0x002EB014u;
        case 0x002EA860u: return 0x002EB020u;
        case 0x002EA868u: return 0x002EB028u;
        case 0x002EA87Cu: return 0x002EB03Cu;
        case 0x002EA890u: return 0x002EB050u;
        case 0x002EA8A4u: return 0x002EB064u;
        case 0x002EA8B8u: return 0x002EB078u;
        case 0x002EA8CCu: return 0x002EB08Cu;
        case 0x002EA8E0u: return 0x002EB0A0u;
        case 0x002EA8F4u: return 0x002EB0B4u;
        case 0x002EA908u: return 0x002EB0C8u;
        case 0x002EA920u: return 0x002EB0E0u;
        case 0x002EA934u: return 0x002EB0F4u;
        case 0x002EA980u: return 0x002EB140u;
        case 0x002EA9B8u: return 0x002EB178u;
        case 0x002EA9CCu: return 0x002EB18Cu;
        case 0x002EA9F0u: return 0x002EB1B0u;
        case 0x002ECDB0u: return 0x002ED570u;
        case 0x002F01B0u: return 0x002F0970u;
        case 0x002F01B4u: return 0x002F0974u;
        case 0x003376C8u: return 0x00337E88u;
        case 0x00348988u: return 0x00349248u;
        case 0x00348A80u: return 0x00349340u;
        case 0x00348B20u: return 0x003493E0u;
        case 0x00348B9Cu: return 0x0034945Cu;
        case 0x00348C24u: return 0x003494E4u;
        case 0x00348E10u: return 0x003496D0u;
        case 0x00349380u: return 0x00349C40u;
        case 0x003493CCu: return 0x00349C8Cu;
        case 0x00351544u: return 0x00351E04u;
        case 0x00351558u: return 0x00351E18u;
        case 0x0035C30Cu: return 0x0035CBCCu;
        case 0x0035C310u: return 0x0035CBD0u;
        case 0x00360DD0u: return 0x0036168Cu;
        case 0x00360DE0u: return 0x0036169Cu;
        case 0x00360E00u: return 0x003616BCu;
        case 0x00360E0Cu: return 0x003616CCu;
        case 0x00363D0Cu: return 0x003645CCu;
        case 0x00363D38u: return 0x003645F8u;
        case 0x00363D3Cu: return 0x003645FCu;
        case 0x00363D44u: return 0x00364604u;
        case 0x00363D50u: return 0x00364610u;
        case 0x0036FE80u: return 0x00370740u;
        case 0x00373554u: return 0x00373E24u;
        case 0x0037493Cu: return 0x0037520Cu;
        case 0x0037BE08u: return 0x0037C6D8u;
        case 0x0037BE28u: return 0x0037C6F8u;
        case 0x0037D284u: return 0x0037DB54u;
        case 0x0037D33Cu: return 0x0037DC0Cu;
        case 0x0037D340u: return 0x0037DC10u;
        case 0x0037D348u: return 0x0037DC18u;
        case 0x0037F630u: return 0x0037FF00u;
        case 0x0037F8BCu: return 0x0038018Cu;
        case 0x0037F8D0u: return 0x003801A0u;
        case 0x0037F8F0u: return 0x003801C0u;
        case 0x0037F8FCu: return 0x003801CCu;
        case 0x0037F940u: return 0x00380210u;
        case 0x003EE31Au: return 0x003EEBEAu;
        case 0x003F54CCu: return 0x003F5D9Cu;
        case 0x003F58E0u: return 0x003F61B0u;
        case 0x00400000u: return 0x00400000u;
        case 0x00420468u: return 0x00420D38u;
        case 0x004204ECu: return 0x00420DBCu;  // input state: right stick bytes at +0x70
        case 0x004C632Cu: return 0x004C6C2Cu;
        case 0x004C6380u: return 0x004C6C80u;
        case 0x004D48A4u: return 0x004D51A4u;
        case 0x004D48A8u: return 0x004D51A8u;
        // Manual cases: data and guest constants the symbol scanner does not alias.
        // Keep these when regenerating the table.
        case 0x00018E46u: return 0x00018E86u;  // 0x15420 return in the 0x18C80 spawn check
        case 0x0001C8A0u: return 0x0001C8E0u;
        case 0x0001CE10u: return 0x0001CE50u;
        case 0x0001E370u: return 0x0001E3B0u;
        case 0x000C15AEu: return 0x000C1A8Eu;  // 0xAFF30 return after the Music Player list build
        case 0x00081CC0u: return 0x00081E50u;
        case 0x000828F0u: return 0x00082A80u;
        case 0x00144670u: return 0x00144D60u;  // 0x137C00 return in the instance draw cull
        case 0x00148D10u: return 0x00149400u;  // tile instance walk
        case 0x0014A73Bu: return 0x0014AE2Bu;  // 0x148D10 return in 0x14A240
        case 0x0014BD60u: return 0x0014C450u;
        case 0x0014C0D0u: return 0x0014C7C0u;
        case 0x0014C150u: return 0x0014C840u;
        case 0x002D3D9Cu: return 0x002D44FCu;
        case 0x002D3DB4u: return 0x002D4514u;
        case 0x002D9654u: return 0x002D9E1Cu;  // ambient traffic car vtable
        case 0x002E3500u: return 0x002E3CC0u;  // traffic despawn/spawn limits
        case 0x002E3504u: return 0x002E3CC4u;
        case 0x002E4110u: return 0x002E48D0u;
        case 0x003A7860u: return 0x003A8130u;
        default: return usa;
    }
}

// Recompiled function names in this image, so the host hooks attach to the right bodies.
#define sub_00013D90 sub_00013D80
#define __imp__sub_00013D90 __imp__sub_00013D80
#define sub_00014120 sub_00014110
#define __imp__sub_00014120 __imp__sub_00014110
#define sub_00014170 sub_00014160
#define __imp__sub_00014170 __imp__sub_00014160
#define sub_00014990 sub_00014980
#define __imp__sub_00014990 __imp__sub_00014980
#define sub_00014C30 sub_00014C20
#define __imp__sub_00014C30 __imp__sub_00014C20
#define sub_000158F0 sub_000158E0
#define __imp__sub_000158F0 __imp__sub_000158E0
#define sub_000168F0 sub_000168C0
#define __imp__sub_000168F0 __imp__sub_000168C0
#define sub_00017BE0 sub_00017C20
#define __imp__sub_00017BE0 __imp__sub_00017C20
#define sub_00018C80 sub_00018CC0
#define __imp__sub_00018C80 __imp__sub_00018CC0
#define sub_00019230 sub_00019270
#define __imp__sub_00019230 __imp__sub_00019270
#define sub_0001F030 sub_0001F070
#define __imp__sub_0001F030 __imp__sub_0001F070
#define sub_0001FD40 sub_0001FD80
#define __imp__sub_0001FD40 __imp__sub_0001FD80
#define sub_000205A0 sub_000205E0
#define __imp__sub_000205A0 __imp__sub_000205E0
#define sub_00021C10 sub_00021C50
#define __imp__sub_00021C10 __imp__sub_00021C50
#define sub_00024040 sub_00024080
#define __imp__sub_00024040 __imp__sub_00024080
#define sub_00025910 sub_00025950
#define __imp__sub_00025910 __imp__sub_00025950
#define sub_00029A40 sub_00029A80
#define __imp__sub_00029A40 __imp__sub_00029A80
#define sub_0002A7B0 sub_0002A7F0
#define __imp__sub_0002A7B0 __imp__sub_0002A7F0
#define sub_0002AAF0 sub_0002AB30
#define __imp__sub_0002AAF0 __imp__sub_0002AB30
#define sub_0001FCA0 sub_0001FCE0
#define __imp__sub_0001FCA0 __imp__sub_0001FCE0
#define sub_00027380 sub_000273C0
#define __imp__sub_00027380 __imp__sub_000273C0
#define sub_000225B0 sub_000225F0
#define __imp__sub_000225B0 __imp__sub_000225F0
#define sub_0002CFC0 sub_0002D010
#define __imp__sub_0002CFC0 __imp__sub_0002D010
#define sub_0002E200 sub_0002E240
#define __imp__sub_0002E200 __imp__sub_0002E240
#define sub_00058D90 sub_00058DB0
#define __imp__sub_00058D90 __imp__sub_00058DB0
#define sub_00068300 sub_000683B0
#define __imp__sub_00068300 __imp__sub_000683B0
#define sub_000703D0 sub_00070550
#define __imp__sub_000703D0 __imp__sub_00070550
#define sub_00074600 sub_00074720
#define __imp__sub_00074600 __imp__sub_00074720
#define sub_0007D820 sub_0007D980
#define __imp__sub_0007D820 __imp__sub_0007D980
#define sub_0007DA50 sub_0007DBB0
#define __imp__sub_0007DA50 __imp__sub_0007DBB0
#define sub_0007DBD0 sub_0007DD30
#define __imp__sub_0007DBD0 __imp__sub_0007DD30
#define sub_0007DD80 sub_0007DEE0
#define __imp__sub_0007DD80 __imp__sub_0007DEE0
#define sub_000823C0 sub_00082550
#define __imp__sub_000823C0 __imp__sub_00082550
#define sub_00083240 sub_000833D0
#define __imp__sub_00083240 __imp__sub_000833D0
#define sub_00087350 sub_000874E0
#define __imp__sub_00087350 __imp__sub_000874E0
#define sub_000A5200 sub_000A54E0
#define __imp__sub_000A5200 __imp__sub_000A54E0
#define sub_000A73F0 sub_000A7720
#define __imp__sub_000A73F0 __imp__sub_000A7720
#define sub_000AD5C0 sub_000ADAB0
#define __imp__sub_000AD5C0 __imp__sub_000ADAB0
#define sub_000B13C0 sub_000B18A0
#define __imp__sub_000B13C0 __imp__sub_000B18A0
#define sub_000B14F0 sub_000B19D0
#define __imp__sub_000B14F0 __imp__sub_000B19D0
#define sub_000B6400 sub_000B6920
#define __imp__sub_000B6400 __imp__sub_000B6920
#define sub_000C7B80 sub_000C8070
#define __imp__sub_000C7B80 __imp__sub_000C8070
#define sub_000CB400 sub_000CB8F0
#define __imp__sub_000CB400 __imp__sub_000CB8F0
#define sub_000DB4F0 sub_000DBAA0
#define __imp__sub_000DB4F0 __imp__sub_000DBAA0
#define sub_000F9C60 sub_000FA2F0
#define sub_0009DDB0 sub_0009DF90
#define __imp__sub_0009DDB0 __imp__sub_0009DF90
#define __imp__sub_000F9C60 __imp__sub_000FA2F0
#define sub_000FD220 sub_000FD800
#define __imp__sub_000FD220 __imp__sub_000FD800
#define sub_00136A70 sub_00137300
#define __imp__sub_00136A70 __imp__sub_00137300
#define sub_00136DB0 sub_00137640
#define __imp__sub_00136DB0 __imp__sub_00137640
#define sub_00137600 sub_00137E90
#define __imp__sub_00137600 __imp__sub_00137E90
#define sub_001378F0 sub_00138180
#define __imp__sub_001378F0 __imp__sub_00138180
#define sub_00138070 sub_00138900
#define __imp__sub_00138070 __imp__sub_00138900
#define sub_001387F0 sub_00139080
#define __imp__sub_001387F0 __imp__sub_00139080
#define sub_00138B90 sub_00139420
#define __imp__sub_00138B90 __imp__sub_00139420
#define sub_0013A1B0 sub_0013AA40
#define __imp__sub_0013A1B0 __imp__sub_0013AA40
#define sub_0013A480 sub_0013AD10
#define __imp__sub_0013A480 __imp__sub_0013AD10
#define sub_0013A850 sub_0013B0E0
#define __imp__sub_0013A850 __imp__sub_0013B0E0
#define sub_0013ABB0 sub_0013B440
#define __imp__sub_0013ABB0 __imp__sub_0013B440
#define sub_0013B460 sub_0013BCF0
#define __imp__sub_0013B460 __imp__sub_0013BCF0
#define sub_0013C390 sub_0013CC20
#define __imp__sub_0013C390 __imp__sub_0013CC20
#define sub_0013C4C0 sub_0013CD50
#define __imp__sub_0013C4C0 __imp__sub_0013CD50
#define sub_0013C8F0 sub_0013D180
#define __imp__sub_0013C8F0 __imp__sub_0013D180
#define sub_0013CB50 sub_0013D3E0
#define __imp__sub_0013CB50 __imp__sub_0013D3E0
#define sub_0013CF40 sub_0013D7D0
#define __imp__sub_0013CF40 __imp__sub_0013D7D0
#define sub_0013D100 sub_0013D990
#define __imp__sub_0013D100 __imp__sub_0013D990
#define sub_0013D350 sub_0013DBE0
#define __imp__sub_0013D350 __imp__sub_0013DBE0
#define sub_0013D750 sub_0013DFE0
#define __imp__sub_0013D750 __imp__sub_0013DFE0
#define sub_0013D880 sub_0013E110
#define __imp__sub_0013D880 __imp__sub_0013E110
#define sub_0013E4A0 sub_0013ED30
#define __imp__sub_0013E4A0 __imp__sub_0013ED30
#define sub_0013E7F0 sub_0013F080
#define __imp__sub_0013E7F0 __imp__sub_0013F080
#define sub_0013E91D sub_0013F1AD
#define __imp__sub_0013E91D __imp__sub_0013F1AD
#define sub_0013EB08 sub_0013F398
#define __imp__sub_0013EB08 __imp__sub_0013F398
#define sub_0013EF50 sub_0013F7E0
#define __imp__sub_0013EF50 __imp__sub_0013F7E0
#define sub_0013EFB0 sub_0013F840
#define __imp__sub_0013EFB0 __imp__sub_0013F840
#define sub_0013F090 sub_0013F920
#define __imp__sub_0013F090 __imp__sub_0013F920
#define sub_0013F4C0 sub_0013FD50
#define __imp__sub_0013F4C0 __imp__sub_0013FD50
#define sub_0013FBE0 sub_00140470
#define __imp__sub_0013FBE0 __imp__sub_00140470
#define sub_001400DA sub_0014096A
#define __imp__sub_001400DA __imp__sub_0014096A
#define sub_00141660 sub_00141D50
#define __imp__sub_00141660 __imp__sub_00141D50
#define sub_00141D40 sub_00142430
#define __imp__sub_00141D40 __imp__sub_00142430
#define sub_001420A0 sub_00142790
#define __imp__sub_001420A0 __imp__sub_00142790
#define sub_00142C90 sub_00143380
#define __imp__sub_00142C90 __imp__sub_00143380
#define sub_00142CE0 sub_001433D0
#define __imp__sub_00142CE0 __imp__sub_001433D0
#define sub_00142DB0 sub_001434A0
#define __imp__sub_00142DB0 __imp__sub_001434A0
#define sub_00143650 sub_00143D40
#define __imp__sub_00143650 __imp__sub_00143D40
#define sub_00144250 sub_00144940
#define __imp__sub_00144250 __imp__sub_00144940
#define sub_00145350 sub_00145A40
#define __imp__sub_00145350 __imp__sub_00145A40
#define sub_00145AC6 sub_001461B6
#define __imp__sub_00145AC6 __imp__sub_001461B6
#define sub_00146CA0 sub_00147390
#define __imp__sub_00146CA0 __imp__sub_00147390
#define sub_00146CC8 sub_001473B8
#define __imp__sub_00146CC8 __imp__sub_001473B8
#define sub_00146D66 sub_00147456
#define __imp__sub_00146D66 __imp__sub_00147456
#define sub_00146FC0 sub_001476B0
#define __imp__sub_00146FC0 __imp__sub_001476B0
#define sub_00146FE8 sub_001476D8
#define __imp__sub_00146FE8 __imp__sub_001476D8
#define sub_001472E0 sub_001479D0
#define __imp__sub_001472E0 __imp__sub_001479D0
#define sub_00147308 sub_001479F8
#define __imp__sub_00147308 __imp__sub_001479F8
#define sub_00148A00 sub_001490F0
#define __imp__sub_00148A00 __imp__sub_001490F0
#define sub_0014A240 sub_0014A930
#define __imp__sub_0014A240 __imp__sub_0014A930
#define sub_0014B390 sub_0014BA80
#define __imp__sub_0014B390 __imp__sub_0014BA80
#define sub_0014F460 sub_0014FB50
#define __imp__sub_0014F460 __imp__sub_0014FB50
#define sub_001785A0 sub_00178C80
#define __imp__sub_001785A0 __imp__sub_00178C80
#define __imp__sub_001785D0 __imp__sub_00178CB0
#define sub_001806B0 sub_00180D80
#define __imp__sub_001806B0 __imp__sub_00180D80
#define sub_0017FA70 sub_00180140
#define __imp__sub_0017FA70 __imp__sub_00180140
#define sub_00182830 sub_00182F00
#define __imp__sub_00182830 __imp__sub_00182F00
#define sub_001828C0 sub_00182F90
#define __imp__sub_001828C0 __imp__sub_00182F90
#define sub_00185D80 sub_00186450
#define __imp__sub_00185D80 __imp__sub_00186450
#define sub_00185D90 sub_00186460
#define __imp__sub_00185D90 __imp__sub_00186460
#define sub_0019D3B0 sub_0019DAC0
#define __imp__sub_0019D3B0 __imp__sub_0019DAC0
#define sub_001B1C60 sub_001B24C0
#define __imp__sub_001B1C60 __imp__sub_001B24C0
#define sub_001B3C00 sub_001B4460
#define __imp__sub_001B3C00 __imp__sub_001B4460
#define sub_001B4F20 sub_001B5780
#define __imp__sub_001B4F20 __imp__sub_001B5780
#define sub_001CA180 sub_001CA9E0
#define __imp__sub_001CA180 __imp__sub_001CA9E0
#define sub_0021C330 sub_0021CA50
#define __imp__sub_0021C330 __imp__sub_0021CA50
#define sub_0021C520 sub_0021CC40
#define __imp__sub_0021C520 __imp__sub_0021CC40
#define sub_0021C570 sub_0021CC90
#define __imp__sub_0021C570 __imp__sub_0021CC90
#define sub_0021C5C0 sub_0021CCE0
#define __imp__sub_0021C5C0 __imp__sub_0021CCE0
#define sub_0021C600 sub_0021CD20
#define __imp__sub_0021C600 __imp__sub_0021CD20
#define sub_0021C640 sub_0021CD60
#define __imp__sub_0021C640 __imp__sub_0021CD60
#define sub_0021C6D0 sub_0021CDF0
#define __imp__sub_0021C6D0 __imp__sub_0021CDF0
#define sub_0021C800 sub_0021CF20
#define __imp__sub_0021C800 __imp__sub_0021CF20
#define sub_0021CF50 sub_0021D670
#define __imp__sub_0021CF50 __imp__sub_0021D670
#define sub_0021DC00 sub_0021E320
#define __imp__sub_0021DC00 __imp__sub_0021E320
#define sub_0021E1A0 sub_0021E8C0
#define __imp__sub_0021E1A0 __imp__sub_0021E8C0
#define sub_0021FC00 sub_00220320
#define __imp__sub_0021FC00 __imp__sub_00220320
#define sub_002219E0 sub_00222090
#define __imp__sub_002219E0 __imp__sub_00222090
#define sub_002291F0 sub_002297E0
#define __imp__sub_002291F0 __imp__sub_002297E0
#define __imp__sub_00229120 __imp__sub_00229710
#define sub_00229D50 sub_0022A360
#define __imp__sub_00229D50 __imp__sub_0022A360
#define sub_0023DAD0 sub_0023E110
#define __imp__sub_0023DAD0 __imp__sub_0023E110
#define sub_002BBB10 sub_002BC270
#define __imp__sub_002BBB10 __imp__sub_002BC270
#define sub_00240003 sub_00240643
#define __imp__sub_00240003 __imp__sub_00240643
#define sub_00240CC0 sub_00241300
#define __imp__sub_00240CC0 __imp__sub_00241300
#define sub_00242F20 sub_002435B0
#define __imp__sub_00242F20 __imp__sub_002435B0
#define sub_00242F30 sub_002435C0
#define __imp__sub_00242F30 __imp__sub_002435C0
#define sub_00243000 sub_00243690
#define __imp__sub_00243000 __imp__sub_00243690
#define sub_002453C0 sub_00245A70
#define __imp__sub_002453C0 __imp__sub_00245A70
#define sub_00245BB0 sub_00246260
#define __imp__sub_00245BB0 __imp__sub_00246260
#define sub_00245CE0 sub_00246390
#define __imp__sub_00245CE0 __imp__sub_00246390
#define sub_00245EF0 sub_002465A0
#define __imp__sub_00245EF0 __imp__sub_002465A0
#define sub_0024C020 sub_0024C670
#define __imp__sub_0024C020 __imp__sub_0024C670
#define sub_0024C6E0 sub_0024CDB0
#define __imp__sub_0024C6E0 __imp__sub_0024CDB0
#define sub_0024CEC0 sub_0024D590
#define __imp__sub_0024CEC0 __imp__sub_0024D590
#define sub_00250170 sub_00250840
#define __imp__sub_00250170 __imp__sub_00250840
#define sub_00258EB0 sub_002595C0
#define __imp__sub_00258EB0 __imp__sub_002595C0
#define sub_0025DA90 sub_0025E160
#define __imp__sub_0025DA90 __imp__sub_0025E160
#define sub_000AFF30 sub_000B0410
#define __imp__sub_000AFF30 __imp__sub_000B0410
#define sub_00079A60 sub_00079B80
#define __imp__sub_00079A60 __imp__sub_00079B80
#define sub_000B8740 sub_000B8C60
#define __imp__sub_000B8740 __imp__sub_000B8C60
#define sub_000B89F0 sub_000B8F10
#define __imp__sub_000B89F0 __imp__sub_000B8F10
#define sub_00260710 sub_00260DE0
#define __imp__sub_00260710 __imp__sub_00260DE0
#define sub_0025DE90 sub_0025E560
#define __imp__sub_0025DE90 __imp__sub_0025E560
#define sub_0029F14F sub_0029F8AF
#define __imp__sub_0029F14F __imp__sub_0029F8AF
#define sub_0029F21F sub_0029F97F
#define __imp__sub_0029F21F __imp__sub_0029F97F
#define sub_0029F25C sub_0029F9BC
#define __imp__sub_0029F25C __imp__sub_0029F9BC
#define sub_0029F261 sub_0029F9C1
#define __imp__sub_0029F261 __imp__sub_0029F9C1
#define sub_0029F296 sub_0029F9F6
#define __imp__sub_0029F296 __imp__sub_0029F9F6
#define sub_0029F2E0 sub_0029FA40
#define __imp__sub_0029F2E0 __imp__sub_0029FA40
#define sub_0029F31A sub_0029FA7A
#define __imp__sub_0029F31A __imp__sub_0029FA7A
#define sub_0029F380 sub_0029FAE0
#define __imp__sub_0029F380 __imp__sub_0029FAE0
#define sub_0029F4C5 sub_0029FC25
#define __imp__sub_0029F4C5 __imp__sub_0029FC25
#define sub_0029F4E3 sub_0029FC43
#define __imp__sub_0029F4E3 __imp__sub_0029FC43
#define sub_0029F547 sub_0029FCA7
#define __imp__sub_0029F547 __imp__sub_0029FCA7
#define sub_0029F637 sub_0029FD97
#define __imp__sub_0029F637 __imp__sub_0029FD97
#define sub_0029F66D sub_0029FDCD
#define __imp__sub_0029F66D __imp__sub_0029FDCD
#define sub_00250580 sub_00250C50
#define __imp__sub_00250580 __imp__sub_00250C50
#define sub_0025F100 sub_0025F7D0
#define __imp__sub_0025F100 __imp__sub_0025F7D0
#define sub_0026381D sub_00263F24
#define __imp__sub_0026381D __imp__sub_00263F24
#define sub_002688F9 sub_00269029
#define __imp__sub_002688F9 __imp__sub_00269029
#define sub_00269405 sub_00269B35
#define __imp__sub_00269405 __imp__sub_00269B35
#define sub_00293541 sub_00293CB1
#define __imp__sub_00293541 __imp__sub_00293CB1
#define sub_0029366D sub_00293DDD
#define __imp__sub_0029366D __imp__sub_00293DDD
#define sub_002BEDE0 sub_002BF540
#define __imp__sub_002BEDE0 __imp__sub_002BF540
#define sub_00015420 sub_00015410
#define __imp__sub_00015420 __imp__sub_00015410
#define sub_00044090 sub_00044060
#define __imp__sub_00044090 __imp__sub_00044060
#define sub_00137C00 sub_00138490
#define __imp__sub_00137C00 __imp__sub_00138490
#define sub_00148D10 sub_00149400
#define __imp__sub_00148D10 __imp__sub_00149400
#define sub_001B8630 sub_001B8E90
#define __imp__sub_001B8630 __imp__sub_001B8E90
#define sub_00247270 sub_00247900
#define __imp__sub_00247270 __imp__sub_00247900
#define sub_00244080 sub_00244730
#define __imp__sub_00244080 __imp__sub_00244730
#define sub_002440F0 sub_002447A0
#define __imp__sub_002440F0 __imp__sub_002447A0
#define sub_000606F0 sub_00060720
#define __imp__sub_000606F0 __imp__sub_00060720
