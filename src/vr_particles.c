#include "vr_particles.h"
#include "proxy_dll.h"
#include <GL/gl.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

static VRParticle g_particles[MAX_VR_PARTICLES] = {0};
static bool g_particlesInitialized = false;

void VRParticles_Init(void) {
    if (g_particlesInitialized) return;
    
    srand((unsigned int)time(NULL));
    g_particlesInitialized = true;
    LOG_INFO("VR Particles initialized");
}

void VRParticles_Shutdown(void) {
    g_particlesInitialized = false;
    LOG_INFO("VR Particles shutdown");
}

void VRParticles_Update(float dt) {
    if (!g_particlesInitialized) return;
    
    for (int i = 0; i < MAX_VR_PARTICLES; i++) {
        if (g_particles[i].active) {
            g_particles[i].life -= dt;
            g_particles[i].pos[0] += g_particles[i].vel[0] * dt;
            g_particles[i].pos[1] += g_particles[i].vel[1] * dt;
            g_particles[i].pos[2] += g_particles[i].vel[2] * dt;
            
            // Gravity for blood
            g_particles[i].vel[1] -= 9.81f * dt * 0.5f;
            
            // Fade out
            float lifeRatio = g_particles[i].life / g_particles[i].maxLife;
            g_particles[i].color[3] = lifeRatio;
            
            if (g_particles[i].life <= 0.0f) {
                g_particles[i].active = false;
            }
        }
    }
}

void VRParticles_Render(const float* viewMatrix, const float* projMatrix) {
    if (!g_particlesInitialized) return;
    
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadMatrixf(projMatrix);
    
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadMatrixf(viewMatrix);
    
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    
    glBegin(GL_QUADS);
    for (int i = 0; i < MAX_VR_PARTICLES; i++) {
        if (!g_particles[i].active) continue;
        
        float size = g_particles[i].size;
        float* pos = g_particles[i].pos;
        float* color = g_particles[i].color;
        
        glColor4f(color[0], color[1], color[2], color[3]);
        
        // Billboard quad facing camera
        float right[3] = {viewMatrix[0], viewMatrix[4], viewMatrix[8]};
        float up[3] = {viewMatrix[1], viewMatrix[5], viewMatrix[9]};
        
        float halfSize = size * 0.5f;
        float v[4][3] = {
            {pos[0] - right[0]*halfSize - up[0]*halfSize, pos[1] - right[1]*halfSize - up[1]*halfSize, pos[2] - right[2]*halfSize - up[2]*halfSize},
            {pos[0] + right[0]*halfSize - up[0]*halfSize, pos[1] + right[1]*halfSize - up[1]*halfSize, pos[2] + right[2]*halfSize - up[2]*halfSize},
            {pos[0] + right[0]*halfSize + up[0]*halfSize, pos[1] + right[1]*halfSize + up[1]*halfSize, pos[2] + right[2]*halfSize + up[2]*halfSize},
            {pos[0] - right[0]*halfSize + up[0]*halfSize, pos[1] - right[1]*halfSize + up[1]*halfSize, pos[2] - right[2]*halfSize + up[2]*halfSize}
        };
        
        glVertex3fv(v[0]);
        glVertex3fv(v[1]);
        glVertex3fv(v[2]);
        glVertex3fv(v[3]);
    }
    glEnd();
    
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_LIGHTING);
    
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

void VRParticles_SpawnBlood(const float* position, const float* normal, int count) {
    if (!g_particlesInitialized) return;
    if (count > 20) count = 20;
    
    for (int i = 0; i < count; i++) {
        int slot = -1;
        for (int j = 0; j < MAX_VR_PARTICLES; j++) {
            if (!g_particles[j].active) {
                slot = j;
                break;
            }
        }
        if (slot == -1) break;
        
        VRParticle* p = &g_particles[slot];
        p->active = true;
        p->pos[0] = position[0];
        p->pos[1] = position[1];
        p->pos[2] = position[2];
        
        float u = (float)rand() / RAND_MAX;
        float v = (float)rand() / RAND_MAX;
        float theta = 2.0f * 3.14159f * u;
        float phi = acosf(2.0f * v - 1.0f) * 0.5f;
        
        float speed = 2.0f + (float)rand() / RAND_MAX * 3.0f;
        p->vel[0] = normal[0] * speed + sinf(phi) * cosf(theta) * speed * 0.5f;
        p->vel[1] = normal[1] * speed + cosf(phi) * speed * 0.5f;
        p->vel[2] = normal[2] * speed + sinf(phi) * sinf(theta) * speed * 0.5f;
        
        p->color[0] = 0.6f + (float)rand() / RAND_MAX * 0.2f;
        p->color[1] = 0.0f;
        p->color[2] = 0.0f;
        p->color[3] = 1.0f;
        
        p->size = 0.02f + (float)rand() / RAND_MAX * 0.03f;
        p->life = 1.0f + (float)rand() / RAND_MAX * 2.0f;
        p->maxLife = p->life;
    }
}

void VRParticles_SpawnMuzzleFlash(const float* position, const float* direction) {
    if (!g_particlesInitialized) return;
    
    int slot = -1;
    for (int j = 0; j < MAX_VR_PARTICLES; j++) {
        if (!g_particles[j].active) {
            slot = j;
            break;
        }
    }
    if (slot == -1) return;
    
    VRParticle* p = &g_particles[slot];
    p->active = true;
    p->pos[0] = position[0];
    p->pos[1] = position[1];
    p->pos[2] = position[2];
    p->vel[0] = 0;
    p->vel[1] = 0;
    p->vel[2] = 0;
    
    p->color[0] = 1.0f;
    p->color[1] = 0.9f;
    p->color[2] = 0.3f;
    p->color[3] = 1.0f;
    
    p->size = 0.15f;
    p->life = 0.05f;
    p->maxLife = 0.05f;
}