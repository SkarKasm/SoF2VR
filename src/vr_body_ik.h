#ifndef SOF2_VR_BODY_IK_H
#define SOF2_VR_BODY_IK_H

/* Metres, in a common coordinate frame. A solver never moves a tracked hand.
 * end is the reachable arm endpoint; target remains the requested wrist. */
typedef struct {
    float shoulder[3],elbow[3],end[3],target[3],pole[3];
    float reachError;
    int valid,limited;
} VRArmIK;

int VR_IKSolveArm(VRArmIK *out,const float shoulder[3],const float wrist[3],
                  const float elbowHint[3],float upper,float forearm,
                  const VRArmIK *previous);
/* Rigid rest-to-posed segment transform, row-major 3x4. No bone stretching. */
int VR_IKSegmentMatrix(const float restStart[3],const float restEnd[3],
                       const float start[3],const float end[3],float matrix[3][4]);
/* Axis 0 follows the limb, axis 1 the glove's projected cuff width.
 * Degenerate width directions fall back to the body frame. */
int VR_IKSegmentBasis(const float direction[3],const float width[3],
                      const float fallbackWidth[3],float axis[3][3]);
#endif
