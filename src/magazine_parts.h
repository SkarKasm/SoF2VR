/* Packed detachable magazines and model-based action contacts. */
#ifndef SOF2_MAGAZINE_PARTS_H
#define SOF2_MAGAZINE_PARTS_H
typedef struct {int id;const char *model,*prefix;float socket[3],bolt[3];int count,surfaces[16];} VRReloadParts;
static const VRReloadParts vrReloadParts[]={
 {2,"models/weapons/ussocom/ussocom.glm","models/sof2vr_reload/w2",{23.6103067f,-4.4358704f,-9.6466163f},{23.0601562f,-4.0000000f,-5.3300000f},4,{12,13,14,15}},
 {3,"models/weapons/m1911a1/m1911a1.glm","models/sof2vr_reload/w3",{23.6619116f,-4.4741713f,-9.1711797f},{23.7671875f,-4.0625000f,-4.1737500f},5,{3,14,15,16,17}},
 {4,"models/weapons/silver_talon/silver_talon.glm","models/sof2vr_reload/w4",{24.0858453f,-4.0887119f,-8.7956263f},{23.6976563f,-3.6328125f,-4.0956250f},4,{12,13,14,15}},
 {5,"models/weapons/usas12/usas12.glm","models/sof2vr_reload/w5",{24.5220056f,-4.6953661f,-13.8644540f},{37.6457979f,-2.5870547f,-3.9296478f},1,{3}},
 {7,"models/weapons/microuzi/microuzi.glm","models/sof2vr_reload/w7",{26.3457792f,-4.8582651f,-12.8470443f},{31.0699791f,-3.9422319f,-3.7789163f},5,{3,6,10,14,19}},
 {8,"models/weapons/m3a1/m3a1.glm","models/sof2vr_reload/w8",{29.5596983f,-9.6803183f,-14.9431524f},{27.8215611f,-0.4432645f,-11.8019254f},2,{7,8}},
 {10,"models/weapons/ak74/ak74.glm","models/sof2vr_reload/w10",{22.5013397f,-4.3559726f,-11.8994581f},{23.0566867f,-5.0926736f,-5.0727255f},2,{6,7}},
 {11,"models/weapons/oicw/oicw.glm","models/sof2vr_reload/w11",{37.7818835f,-6.1076672f,-13.9818255f},{32.2246391f,-6.1583251f,-1.1100837f},2,{9,10}},
 {12,"models/weapons/msg90a1/msg90a1.glm","models/sof2vr_reload/w12",{29.0329684f,-4.9741025f,-10.9060964f},{23.8392813f,-5.9426815f,-5.3374729f},1,{4}},
 {35,"models/weapons/mp5/mp5.glm","models/sof2vr_reload/w35",{30.2159854f,-3.3812486f,-14.1502901f},{36.1665746f,-2.3033481f,-3.5608426f},2,{2,3}},
 {36,"models/weapons/sig551/sig551.glm","models/sof2vr_reload/w36",{21.9069443f,-4.3018552f,-9.3280692f},{23.4289181f,-5.6578012f,-2.8092038f},2,{0,1}},
 {9,"models/weapons/m4/m4.glm","models/sof2vr_reload/m4",{24.9362320f,-4.9606327f,-11.6674210f},{23.0666414f,-5.3774190f,-7.1666718f},2,{4,5}},
};
static inline const VRReloadParts *ReloadParts(int id){for(unsigned i=0;i<sizeof(vrReloadParts)/sizeof(vrReloadParts[0]);++i)if(vrReloadParts[i].id==id)return &vrReloadParts[i];return 0;}
#endif
