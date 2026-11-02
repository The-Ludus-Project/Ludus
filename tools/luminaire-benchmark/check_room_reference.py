#!/usr/bin/env python3
"""Check selected rendered pixels with independent CPU area quadrature and shadows."""
import argparse
import json
from pathlib import Path

import numpy as np

from benchmark import CENTER, HALF_EXTENT, Panel
from render_comparison import load_triangles


def trace(triangles, origin, direction, ignored=None, maximum=None):
    origin = np.broadcast_to(origin, direction.shape)
    count = len(direction)
    distance = np.full(count, 10.24) if maximum is None else np.array(maximum, copy=True)
    owners = np.full(count, -1, dtype=int)
    normals = np.zeros((count, 3))
    for index, triangle in enumerate(triangles.reshape(-1, 4, 4)):
        first, second, third, normal = triangle[:, :3]
        edge_first = second-first
        edge_second = third-first
        cross_first = np.cross(direction, edge_second)
        determinant = np.sum(cross_first*edge_first,axis=1)
        inverse = np.divide(1., determinant, out=np.zeros(count), where=determinant != 0)
        offset = origin-first
        u = np.sum(offset*cross_first,axis=1)*inverse
        cross_second = np.cross(offset, edge_first)
        v = np.sum(direction*cross_second,axis=1)*inverse
        t = np.sum(cross_second*edge_second,axis=1) * inverse
        valid = (determinant != 0) & (u >= 0) & (v >= 0) & (u+v <= 1) & (t > .0001) & (t < distance)
        if ignored is not None:
            valid &= index != ignored
        distance[valid] = t[valid]
        owners[valid] = index
        normals[valid] = normal
    return distance, owners, normals


def primary(triangles, extent):
    x,y = np.meshgrid(np.arange(extent),np.arange(extent))
    screen = (np.c_[x.ravel(),y.ravel()]+.5)/extent*2-1
    forward=np.array([0.,-.1,3.2]);forward/=np.linalg.norm(forward)
    right=np.cross([0.,1,0],forward);right/=np.linalg.norm(right)
    up=np.cross(forward,right)
    direction=forward+np.tan(np.pi/8)*(screen[:,:1]*right-screen[:,1:]*up)
    direction/=np.linalg.norm(direction,axis=1)[:,None]
    origin=np.array([0.,0,-3.2])
    distance,owner,normal=trace(triangles,origin,direction)
    return origin+distance[:,None]*direction,normal,owner


def quadrature(panel, triangles, point, normal, owner, order):
    nodes,weights=np.polynomial.legendre.leggauss(order)
    partitions=[set(),set()]
    # This controlled room has horizontal rectangular occluders. Split at their
    # exact projected silhouette so quadrature never integrates across a shadow
    # discontinuity. Visibility is still checked independently with triangles.
    mesh=triangles.reshape(-1,4,4)[:,:3,:3]
    shadow_origin=point+normal*.0001
    heights=np.unique(mesh[:,:,1][np.all(mesh[:,:,1]==mesh[:,:1,1],axis=1)])
    for height in heights:
        if shadow_origin[1] < height < CENTER[1]:
            vertices=mesh[np.all(mesh[:,:,1]==height,axis=1)].reshape(-1,3)
            fraction=(height-shadow_origin[1])/(CENTER[1]-point[1])
            for axis,world_axis in enumerate([0,2]):
                for boundary in [vertices[:,world_axis].min(),vertices[:,world_axis].max()]:
                    projected=(point[world_axis]+(boundary-shadow_origin[world_axis])/fraction)/HALF_EXTENT
                    partitions[axis].add(float(projected))
    result=np.zeros(3)
    for lower in panel.lower:
        edges=[sorted([lower[axis],lower[axis]+2/3]+[v for v in partitions[axis] if lower[axis] < v < lower[axis]+2/3])
               for axis in range(2)]
        for left,right in zip(edges[0][:-1],edges[0][1:]):
            for bottom,top in zip(edges[1][:-1],edges[1][1:]):
                x,y=np.meshgrid((nodes+1)*(right-left)/2+left,(nodes+1)*(top-bottom)/2+bottom)
                candidates=np.c_[x.ravel(),y.ravel()]
                products=(weights[:,None]*weights).ravel()*(right-left)*(top-bottom)/4
                radiance,jacobian,directions=panel.evaluate(point,candidates)
                physical=radiance*(jacobian*np.maximum(np.sum(directions*normal,axis=1),0)/np.pi)[:,None]
                world=CENTER+HALF_EXTENT*np.c_[candidates[:,0],np.zeros(len(candidates)),candidates[:,1]]
                delta=world-point
                distance=np.linalg.norm(delta,axis=1)
                _,occluder,_=trace(triangles,shadow_origin,delta/distance[:,None],owner,distance-.0001)
                physical[occluder >= 0]=0
                result+=np.sum(physical*products[:,None],axis=0)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--room',type=Path,required=True)
    parser.add_argument('--results',type=Path,required=True)
    args=parser.parse_args()
    reference=np.load(args.results/'reference.npy')
    extent=len(reference)
    triangles=load_triangles(args.room).astype(np.float64)
    positions,normals,owners=primary(triangles,extent)
    energy=reference.sum(axis=2).ravel()
    candidates=np.flatnonzero((owners >= 0) & (energy > energy.max()*.01))
    chosen=candidates[np.linspace(0,len(candidates)-1,8,dtype=int)]
    panel=Panel()
    rows=[]
    for pixel in chosen:
        first=quadrature(panel,triangles,positions[pixel],normals[pixel],owners[pixel],128)
        second=quadrature(panel,triangles,positions[pixel],normals[pixel],owners[pixel],256)
        mc=reference.reshape(-1,3)[pixel]
        rows.append(dict(pixel=[int(pixel%extent),int(pixel//extent)],quadrature=second.tolist(),
            quadrature_relative_change=float(np.linalg.norm(first-second)/max(np.linalg.norm(second),1e-30)),
            reference=mc.tolist(),relative_error=float(np.linalg.norm(mc-second)/max(np.linalg.norm(second),1e-30))))
    has_passed=all(row['quadrature_relative_change'] <= .001 and row['relative_error'] <= .03 for row in rows)
    report=dict(scope='eight selected lit pixels; independent CPU geometry and emitter-area quadrature',
                has_passed=has_passed,results=rows)
    (args.results/'cpu-reference-check.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))
    if not has_passed:
        raise ValueError('Reference check needs further investigation; evidence preserved.')


if __name__=='__main__':
    main()
