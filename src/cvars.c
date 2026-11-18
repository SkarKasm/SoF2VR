#include "cvars.h"
#include "proxy_dll.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_CVARS 64

static cvar_t g_cvars[MAX_CVARS] = {0};
static int g_numCvars = 0;

/* CVAR definitions */
cvar_t vr_worldscale = {"vr_worldscale", "39.37", 39.37f, false};
cvar_t vr_ipr = {"vr_ipr", "0.064", 0.064f, false};
cvar_t vr_snapturn_angle = {"vr_snapturn_angle", "45", 45.0f, false};
cvar_t vr_comfort_vignette = {"vr_comfort_vignette", "0", 0.0f, false};
cvar_t vr_comfort_vignette_intensity = {"vr_comfort_vignette_intensity", "0.5", 0.5f, false};
cvar_t vr_weapon_stabilize = {"vr_weapon_stabilize", "0", 0.0f, false};
cvar_t vr_manual_reload = {"vr_manual_reload", "1", 1.0f, false};
cvar_t vr_enable_particles = {"vr_enable_particles", "1", 1.0f, false};
cvar_t vr_debug_draw = {"vr_debug_draw", "0", 0.0f, false};

void Cvar_Init(void) {
    cvar_t* cvars[] = {
        &vr_worldscale,
        &vr_ipr,
        &vr_snapturn_angle,
        &vr_comfort_vignette,
        &vr_comfort_vignette_intensity,
        &vr_weapon_stabilize,
        &vr_manual_reload,
        &vr_enable_particles,
        &vr_debug_draw,
        NULL
    };
    
    for (int i = 0; cvars[i] && g_numCvars < MAX_CVARS; i++) {
        g_cvars[g_numCvars] = *cvars[i];
        g_cvars[g_numCvars].value = atof(g_cvars[g_numCvars].string);
        g_numCvars++;
    }
    
    LOG_INFO("CVars initialized: %d variables", g_numCvars);
}

void Cvar_Shutdown(void) {
    g_numCvars = 0;
}

cvar_t* Cvar_FindVar(const char* name) {
    for (int i = 0; i < g_numCvars; i++) {
        if (strcmp(g_cvars[i].name, name) == 0) {
            return &g_cvars[i];
        }
    }
    return NULL;
}

float Cvar_VariableValue(const char* name) {
    cvar_t* var = Cvar_FindVar(name);
    return var ? var->value : 0.0f;
}

const char* Cvar_VariableString(const char* name) {
    cvar_t* var = Cvar_FindVar(name);
    return var ? var->string : "";
}

void Cvar_Set(const char* name, const char* value) {
    cvar_t* var = Cvar_FindVar(name);
    if (!var) return;
    
    var->string = value;
    var->value = atof(value);
    var->modified = true;
    
    LOG_INFO("Cvar %s = %s", name, value);
}

void Cvar_SetValue(const char* name, float value) {
    cvar_t* var = Cvar_FindVar(name);
    if (!var) return;
    
    char buffer[32];
    sprintf(buffer, "%f", value);
    var->string = buffer;
    var->value = value;
    var->modified = true;
}

void Cvar_Reset(const char* name) {
    cvar_t* var = Cvar_FindVar(name);
    if (!var) return;
    
    var->value = atof(var->string);
    var->modified = false;
}

void Cvar_WriteConfig(const char* filename) {
    FILE* f = fopen(filename, "w");
    if (!f) return;
    
    fprintf(f, "// SoF2 VR Configuration\n");
    fprintf(f, "// Generated automatically\n\n");
    
    for (int i = 0; i < g_numCvars; i++) {
        if (g_cvars[i].modified) {
            fprintf(f, "seta %s \"%s\"\n", g_cvars[i].name, g_cvars[i].string);
        }
    }
    
    fclose(f);
}