/*
===========================================================================
SoF2 VR — Console Variables
File: cvars.h
Description: VR configuration variables.
===========================================================================
*/

#ifndef __CVARS_H__
#define __CVARS_H__

#include <stdbool.h>

typedef struct {
    const char* name;
    const char* string;
    float value;
    bool modified;
} cvar_t;

/* CVAR flags */
#define CVAR_ARCHIVE    0x0001
#define CVAR_USERINFO   0x0002
#define CVAR_SERVERINFO 0x0004
#define CVAR_SYSTEMINFO 0x0008
#define CVAR_INIT       0x0010
#define CVAR_LATCH      0x0020
#define CVAR_ROM        0x0040
#define CVAR_CHEAT      0x0080

/* External CVAR declarations */
extern cvar_t vr_worldscale;
extern cvar_t vr_ipr;
extern cvar_t vr_snapturn_angle;
extern cvar_t vr_comfort_vignette;
extern cvar_t vr_comfort_vignette_intensity;
extern cvar_t vr_weapon_stabilize;
extern cvar_t vr_manual_reload;
extern cvar_t vr_enable_particles;
extern cvar_t vr_debug_draw;

/* ---- API ---- */
void Cvar_Init(void);
void Cvar_Shutdown(void);
cvar_t* Cvar_FindVar(const char* name);
float Cvar_VariableValue(const char* name);
const char* Cvar_VariableString(const char* name);
void Cvar_Set(const char* name, const char* value);
void Cvar_SetValue(const char* name, float value);
void Cvar_Reset(const char* name);
void Cvar_WriteConfig(const char* filename);

#endif /* __CVARS_H__ */