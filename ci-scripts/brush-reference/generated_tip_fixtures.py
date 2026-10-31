"""Original distinct Default/Gaussian/curve-Soft source observations."""
import json
from xml.etree.ElementTree import Element, SubElement, tostring

def prepare(root, resources, writer):
    plans=[]
    for generator in ('default','gauss'):
        for shape in ('circle','rect'):
            for diameter in (16,64):
                for fade in (.25,.75):
                    plans.append((generator,shape,diameter,fade,fade,None))
    for shape in ('circle','rect'):
        for diameter in (16,64):
            for curve in ('0,0;1,1;', '0,0;0.25,0.75;1,1;', '0,1;0.5,0.2;1,0;'):
                plans.append(('soft',shape,diameter,.5,.5,curve))
    for generator in ('gauss','soft'):
        for shape in ('circle','rect'):
            plans.append((generator,shape,64,.25,.75,'0,0;1,1;' if generator=='soft' else None))
    cases=[]
    for generator,shape,diameter,hfade,vfade,curve in plans:
        name='GZ Original generated tips '+str(len(cases)).zfill(3)
        brush=Element('Brush',type='auto_brush',BrushVersion='2',spacing='1',angle='0',randomness='0',density='1',useAutoSpacing='0')
        attributes=dict(id=generator,type=shape,diameter=str(diameter),ratio='1',hfade=str(hfade),vfade=str(vfade),antialiasEdges='1',spikes='2')
        if curve is not None: attributes['softness_curve']=curve
        SubElement(brush,'MaskGenerator',attributes)
        overrides={'brush_definition':tostring(brush,encoding='unicode'),'Spacing/Isotropic':'false'}
        (resources/'paintoppresets'/(name+'.kpp')).write_bytes(writer(name,'dry',False,overrides,diameter,1,1))
        cases.append({'preset':name,'variant':'baseline','diameter':diameter,'spacing':1,'pressure':[1,1],'opacity':1,'flow':1,
            'foregroundRGBA8':[173,91,47,255],'start':[1024,1024],'end':[1025,1024],
            'background':'transparent','overrides':overrides,'delivery':'Node.paintLine','canvasCenter':[1024,1024],
            'generatedTipProbe':attributes,'changeExpectation':'Distinct source mask acquisition; no source generator admission'})
    (root/'output/fixtures.json').write_text(json.dumps({'version':'5.3.4.1','group':'generatedtips','presets':[c['preset'] for c in cases],
        'cases':cases,'license':'CC0-1.0','width':2048,'height':2048,'input':'Public Node.paintLine original generated-tip controls'},indent=2))
