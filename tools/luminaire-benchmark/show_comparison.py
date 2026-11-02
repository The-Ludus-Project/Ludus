#!/usr/bin/env python3
"""Create shared-exposure PNG previews and error maps from raw room renders."""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def font(size):
    candidates=[Path('/System/Library/Fonts/Supplemental/Arial.ttf'),
                Path('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf')]
    for path in candidates:
        if path.is_file():
            return ImageFont.truetype(str(path),size)
    return ImageFont.load_default()


def display(image,exposure):
    value=np.maximum(image,0)*exposure
    value=value/(1+value)
    value=np.where(value <= .0031308,12.92*value,1.055*np.power(value,1/2.4)-.055)
    return Image.fromarray(np.uint8(np.clip(value,0,1)*255+.5))


def error_image(image,reference,peak):
    error=np.sqrt(np.mean((image-reference)**2,axis=2))/peak
    scale=np.clip((np.log10(error+1e-6)+6)/6,0,1)
    stops=np.array([0,.25,.5,.75,1])
    colors=np.array([[8,9,18],[49,20,84],[147,38,104],[239,110,41],[255,248,196]])
    rgb=np.stack([np.interp(scale,stops,colors[:,i]) for i in range(3)],axis=2)
    return Image.fromarray(np.uint8(rgb))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('results',type=Path)
    args=parser.parse_args()
    root=args.results
    report=json.loads((root/'report.json').read_text())
    validation=json.loads((root/'cpu-reference-check.json').read_text())
    if not validation['has_passed']:
        raise ValueError('CPU reference validation must pass before creating comparison images.')
    reference=np.load(root/'reference.npy')
    exposure=.8/max(float(np.quantile(reference.max(axis=2),.99)),1e-12)
    peak=float(reference.max())
    # The middle trial is selected by run order, independently of image quality.
    trial=1 if len(report['results']) >= 4 else 0
    images={method:np.load(root/f'{method}-{trial}.npz')['image'] for method in ['slice','portable-purelight']}
    records={method:next(row for row in report['results'] if row['method']==method and row['seed']==1001+trial) for method in images}
    display(reference,exposure).save(root/'reference.png')
    for method,image in images.items():
        display(image,exposure).save(root/f'{method}.png')
        error_image(image,reference,peak).save(root/f'{method}-error.png')
    side=384
    margin=24
    width=3*side+4*margin
    canvas=Image.new('RGB',(width,990),(17,20,27))
    draw=ImageDraw.Draw(canvas)
    draw.text((margin,18),'Controlled room | equal rendering time | Apple M5 / Metal',font=font(24),fill=(238,241,247))
    draw.text((margin,52),f'{len(reference)} x {len(reference)} native pixels; shared exposure; direct lighting; no denoising',font=font(17),fill=(177,185,200))
    panels=[('Reference',display(reference,exposure),None),('Slice sampling',display(images['slice'],exposure),records['slice']),
            ('Portable PureLight',display(images['portable-purelight'],exposure),records['portable-purelight'])]
    for column,(title,im,record) in enumerate(panels):
        x=margin+column*(side+margin)
        draw.text((x,90),title,font=font(23),fill=(238,241,247))
        if record:
            label=f'{record["wall_seconds"]:.3f} s | {record["spp_mean"]:.2f} spp'
        else:
            label=f'2 x {report["reference_samples_per_half"]:,} spp'
        draw.text((x,121),label,font=font(17),fill=(177,185,200))
        canvas.paste(im.resize((side,side),Image.Resampling.NEAREST),(x,150))
        if record:
            draw.text((x,547),f'Linear NRMSE: {record["normalized_rmse"]*100:.3f}%',font=font(19),fill=(238,241,247))
            canvas.paste(error_image(images[record['method']],reference,peak).resize((side,side),Image.Resampling.NEAREST),(x,582))
        else:
            draw.text((x,547),'Error maps: same logarithmic scale',font=font(18),fill=(238,241,247))
            draw.text((x,581),'RMS RGB error / reference peak',font=font(17),fill=(177,185,200))
            gradient=np.repeat(np.linspace(0,1,256)[None],24,axis=0)
            colors=np.array([[8,9,18],[49,20,84],[147,38,104],[239,110,41],[255,248,196]])
            rgb=np.stack([np.interp(gradient,np.linspace(0,1,5),colors[:,i]) for i in range(3)],axis=2)
            canvas.paste(Image.fromarray(rgb.astype(np.uint8)).resize((side-16,32)),(x,620))
            draw.text((x,665),'10^-6',font=font(16),fill=(177,185,200))
            draw.text((x+(side-16)//2-20,665),'10^-3',font=font(16),fill=(177,185,200))
            draw.text((x+side-25,665),'1',font=font(16),fill=(177,185,200))
            draw.text((x,711),'Portable model: int8 weights, FP32 inference',font=font(16),fill=(177,185,200))
            draw.text((x,742),'1,000 CPU training steps / eight receivers',font=font(16),fill=(177,185,200))
            draw.text((x,789),'This is the headless compute harness.',font=font(17),fill=(238,241,247))
            draw.text((x,822),'Editor/BVH timings and official CUDA',font=font(17),fill=(177,185,200))
            draw.text((x,848),'PureLight performance are not measured.',font=font(17),fill=(177,185,200))
    canvas.save(root/'comparison.png')
    summary={}
    for method in images:
        rows=[row for row in report['results'] if row['method']==method]
        summary[method]={key:dict(median=float(np.median([row[key] for row in rows])),
                                 minimum=float(np.min([row[key] for row in rows])),
                                 maximum=float(np.max([row[key] for row in rows])))
                         for key in ['wall_seconds','gpu_seconds','spp_mean','samples_per_wall_second','mse','normalized_rmse']}
    summary['reference_estimated_normalized_rmse']=report['reference_estimated_normalized_rmse']
    summary['display_exposure']=exposure
    summary['display_trial']=trial
    (root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
    print(json.dumps(summary,indent=2))


if __name__=='__main__':
    main()
