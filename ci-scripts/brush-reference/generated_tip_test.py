"""Pure source-fixture/readback contract, not generator parity."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from generated_tip_fixtures import prepare
spec=importlib.util.spec_from_file_location('generated_tip_probe',Path(__file__).parent/'gzbrush_reference/generated_tip_probe.py')
probe=importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)
class GeneratedTipContract(unittest.TestCase):
    def test_distinct_masks_and_exact_readback(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);(root/'output').mkdir();(root/'paintoppresets').mkdir()
            prepare(root,root,lambda *args:b'original metadata test')
            cases=json.loads((root/'output/fixtures.json').read_text())['cases']
            self.assertEqual(len(cases),32)
            self.assertEqual({c['generatedTipProbe']['id'] for c in cases},{'default','gauss','soft'})
            for case in cases:
                definition=case['overrides']['brush_definition']
                result=probe.controls(case,definition)
                self.assertEqual(result['effectiveMask']['id'],case['generatedTipProbe']['id'])
                for key in ('id','type','hfade','vfade','diameter'):
                    old=case['generatedTipProbe'][key]
                    with self.assertRaises(ValueError):
                        probe.controls(case,definition.replace(key+'="'+old+'"',key+'="999"'))
                if case['generatedTipProbe']['id']=='soft':
                    with self.assertRaises(ValueError):
                        probe.controls(case,definition.replace('softness_curve="','softness_curve="0,0.8;'))
if __name__=='__main__':unittest.main()
