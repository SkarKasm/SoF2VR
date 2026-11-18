"""Clip the native glove at its wrist and close the cut with native leather UVs.

Clipping interpolates the posed skin, rather than dropping crossing triangles.
The cap is generated in bind space so it follows every authored finger pose.
"""
import math

# Palm calibration uses the original slice. The mesh continues to the native
# cuff, just short of the stock sleeve seam and long forearm filler triangle.
WRIST_PLANE = .5
CUFF_PLANE = 2.65

def close_wrist(points, normals, uv, triangles, binds, plane=CUFF_PLANE):
    outp=[];outn=[];outuv=[];outt=[];edges=[]
    def lerp(a,b,t):return [x+(y-x)*t for x,y in zip(a,b)]
    def vertex(v):
        i=len(outp);outp.append(v[0]);outn.append(v[1]);outuv.append(v[2]);return i
    for tri in triangles:
        poly=[(points[i],normals[i],uv[i],binds[i]) for i in tri];cut=[];clipped=[]
        for a,b in zip(poly,poly[1:]+poly[:1]):
            ina=a[3][1]<=plane;inb=b[3][1]<=plane
            if ina:clipped.append(a)
            if ina!=inb:
                t=(plane-a[3][1])/(b[3][1]-a[3][1]);v=tuple(lerp(x,y,t) for x,y in zip(a,b));clipped.append(v);cut.append(v)
        if len(cut)==2:edges.append(cut)
        if len(clipped)>=3:
            ids=[vertex(v) for v in clipped]
            outt.extend((ids[0],ids[j],ids[j+1]) for j in range(1,len(ids)-1))
    # Weld material seams by bind coordinates. The outer cut contour defines
    # the wrist cover; internal duplicate edges must not create extra caps.
    key=lambda v:tuple(round(x,4) for x in v[3])
    nodes={};adj={}
    for a,b in edges:
        ka,kb=key(a),key(b)
        if ka==kb:continue
        nodes[ka]=a;nodes[kb]=b;adj.setdefault(ka,set()).add(kb);adj.setdefault(kb,set()).add(ka)
    seen=set();caps=0
    for start in adj:
        if start in seen:continue
        todo=[start];component=[]
        while todo:
            k=todo.pop()
            if k in seen:continue
            seen.add(k);component.append(k);todo.extend(adj[k]-seen)
        if len(component)<3:continue
        cx=sum(k[0] for k in component)/len(component);cz=sum(k[2] for k in component)/len(component)
        ring=sorted(component,key=lambda k:math.atan2(k[2]-cz,k[0]-cx))
        centre=[sum(nodes[k][0][i] for k in ring)/len(ring) for i in range(3)]
        # Use the original leather at the wrist seam, with a compact UV patch.
        # This patch lies entirely inside the native leather glove's back,
        # away from the exposed-skin and sleeve islands in hands.jpg.
        tex=[.13,.20]
        normal=[0.,0.,0.]
        for a,b in zip(ring,ring[1:]+ring[:1]):
            u=[nodes[a][0][i]-centre[i] for i in range(3)];v=[nodes[b][0][i]-centre[i] for i in range(3)]
            cross=[u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
            normal=[x+y for x,y in zip(normal,cross)]
        length=math.sqrt(sum(x*x for x in normal))
        if length<1e-8:normal=[0,0,1]
        else:normal=[-x/length for x in normal] # Native GLM triangles wind clockwise.
        ci=vertex((centre,normal,tex));ids=[]
        for k in ring:
            p=nodes[k][0];t=[tex[0]+(k[0]-cx)*.015,tex[1]+(k[2]-cz)*.015]
            ids.append(vertex((p,normal,t)))
        outt.extend((ci,ids[j],ids[(j+1)%len(ids)]) for j in range(len(ids)));caps+=1
    assert caps, 'Native glove wrist plane produced no closing contour'
    return outp,outn,outuv,outt
