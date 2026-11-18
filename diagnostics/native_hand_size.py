"""Keep both native gloves at their matching skeletal scale.

Their corresponding phalanx lengths match within approximately 0.5 percent.
Different rigid-palm skin bounds are not evidence of a different hand scale;
using that width ratio shortened the entire support hand and limited reach.
"""
LEFT_SCALE=1.0

def evaluated_size(e,side):
 if side!='l':return e
 from build_weapon_grips import palm_points
 from native_pose import read
 p=palm_points(read('models/weapons/lhand/lhand.glm'),e[0]);s=LEFT_SCALE
 world=[[[r[k]*s for k in range(3)]+[p[i]+s*(r[3]-p[i])] for i,r in enumerate(m)] for m in e[0]]
 return world,e[1],e[2]

def points_size(points,palm,side):
 s=LEFT_SCALE if side=='l' else 1.
 return [[palm[k]+s*(p[k]-palm[k]) for k in range(3)] for p in points]
