/*
===========================================================================
SoF2 VR — VR Particle System (Optional)
File: vr_particles.h
Description: Simple particle system for blood splatter, muzzle flashes in VR.
===========================================================================
*/

#ifndef __VR_PARTICLES_H__
#define __VR_PARTICLES_H__

#include <stdbool.h>

typedef struct {
    float pos[3];
    float vel[3];
    float color[4];
    float size;
    float life;
    float maxLife;
    bool active;
} VRParticle;

#define MAX_VR_PARTICLES 1024

/* ---- Subsystem API ---- */
void VRParticles_Init(void);
void VRParticles_Shutdown(void);
void VRParticles_Update(float dt);
void VRParticles_Render(const float* viewMatrix, const float* projMatrix);
void VRParticles_SpawnBlood(const float* position, const float* normal, int count);
void VRParticles_SpawnMuzzleFlash(const float* position, const float* direction);

#endif /* __VR_PARTICLES_H__ */