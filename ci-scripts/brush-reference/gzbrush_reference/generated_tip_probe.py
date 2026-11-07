"""Validate public serialized mask identity and emit bounded original pixels."""
import base64
import json
import math
import zlib
from xml.etree.ElementTree import fromstring


def controls(case, definition):
    root=fromstring(definition)
    masks=root.findall('MaskGenerator')
    if root.attrib.get('type')!='auto_brush' or len(masks)!=1:
        raise ValueError('Generated tip source structure changed')
    actual=masks[0].attrib
    for key,value in case['generatedTipProbe'].items():
        if key in ('id','type'):
            valid=actual.get(key)==value
        elif key=='softness_curve':
            def points(text): return [tuple(map(float,p.split(','))) for p in text.strip(';').split(';')]
            valid=points(actual.get(key,''))==points(value)
        else:
            observed=float(actual.get(key,'nan'))
            valid=math.isfinite(observed) and abs(observed-float(value))<=1e-6
        if not valid: raise ValueError('Source generated tip lost '+key)
    return {'effectiveMask':dict(actual),'brushDefinition':definition}


def emit(case, evidence, raw, width):
    rect=[976,976,96,96]
    crop=b''.join(raw[(y*width+rect[0])*4:(y*width+rect[0]+rect[2])*4]
                  for y in range(rect[1],rect[1]+rect[3]))
    if len(crop)!=96*96*4: raise ValueError('Incomplete generated tip pixel readback')
    encoded=base64.b64encode(zlib.compress(crop)).decode('ascii')
    print('::notice::'+json.dumps({'preset':case['preset'],'capture':'generated-tip-controls',**evidence,'rect':rect},separators=(',',':')))
    for offset in range(0,len(encoded),1800):
        print('::notice::'+json.dumps({'preset':case['preset'],'capture':'generated-tip-bgra','rect':rect,
              'encoding':'zlib-base64-bgra8','offset':offset,'total':len(encoded),'data':encoded[offset:offset+1800]},separators=(',',':')))
