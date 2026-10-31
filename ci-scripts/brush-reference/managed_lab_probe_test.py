"""Pure CI contract checks, not source conversion or renderer qualification."""
import copy
import unittest
import importlib.util
from pathlib import Path
spec=importlib.util.spec_from_file_location("managed_lab_probe",Path(__file__).parent / "gzbrush_reference/managed_lab_probe.py")
probe=importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

class FakeApp:
    def colorDepths(self,model): return ["U8","U16","F32"]
    def profiles(self,model,depth): return [probe.PROFILE] if model == "RGBA" else ["Original mock Lab"]
    def version(self): return "5.3.4.1 (mock contract only)"

class FakeColor:
    def __init__(self,model,depth,profile):
        self.model,self.depth,self.profile=model,depth,profile or "Original mock Lab"
        self.values=[0,0,0,1]
    def colorModel(self): return self.model
    def colorDepth(self): return self.depth
    def colorProfile(self): return self.profile
    def setComponents(self,values): self.values=values
    def components(self): return self.values
    def componentsOrdered(self): return self.values[:3][::-1]+self.values[3:] if self.model=="RGBA" else self.values
    def toXML(self): return "<original-mock/>"
    def toQString(self): return "Original mock channels"
    def setColorSpace(self,model,depth,profile):
        self.model,self.depth,self.profile=model,depth,profile
        return True

class PublicLabContract(unittest.TestCase):
    def test_complete_and_reject_changed_identity(self):
        report=probe.capture(FakeApp(),FakeColor)
        self.assertEqual(len(report['records']),76)
        for mutation in ('missing','depth','input','nan','runtime'):
            bad=copy.deepcopy(report)
            if mutation=='missing': bad['records'].pop()
            if mutation=='depth': bad['records'][0]['lab']['depth']='U8'
            if mutation=='input': bad['records'][0]['inputRGB'][0]=.1
            if mutation=='nan': bad['records'][0]['lab']['ordered'][0]=float('nan')
            if mutation=='runtime': bad['runtimeVersion']=''
            with self.assertRaises(ValueError): probe.validate(bad)

    def test_optional_depth_is_explicit_without_blocking_observations(self):
        class IntegerOnly(FakeApp):
            def colorDepths(self,model): return ["U8","U16"]
        report=probe.capture(IntegerOnly(),FakeColor)
        self.assertEqual(len(report['records']),38)
        self.assertEqual(report['researchStatus'],'available')
        self.assertEqual(report['labProfiles']['F32'],
                         {'supported':False,'reason':'depth not registered'})
        class NoLab(FakeApp):
            def colorDepths(self,model): return [] if model=='LABA' else ['U8','U16']
        unsupported=probe.capture(NoLab(),FakeColor)
        self.assertEqual(unsupported['records'],[])
        self.assertEqual(unsupported['researchStatus'],'unsupported-U16')

    def test_channel_order_is_observed(self):
        class WrongOrder(FakeColor):
            def componentsOrdered(self): return self.values
        with self.assertRaises(ValueError): probe.capture(FakeApp(),WrongOrder)

if __name__=='__main__': unittest.main()
