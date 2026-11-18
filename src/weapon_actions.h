#ifndef SOF2_VR_WEAPON_ACTIONS_H
#define SOF2_VR_WEAPON_ACTIONS_H
/* Presentation strokes, in metres. Distinct open-bolt mechanisms must not
 * discard imaginary chambered cartridges when their bolts are cocked. */
static inline int VR_OpenBolt(int id){return id==7 || id==8;}
static inline float VR_ActionStroke(int id){
    switch(id){
    case 2:return .045f;case 3:return .042f;case 4:return .050f;
    case 7:return .080f;case 8:return .105f;
    case 9:return .090f;case 10:return .095f;case 36:return .085f;
    case 12:return .090f;case 35:return .085f;
    default:return .070f;
    }
}
#endif
