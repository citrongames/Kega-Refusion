#!/usr/bin/env python3
"""Generated original SMS region fixtures, one native core per child process.
No copyrighted ROM data is used. CRC forging selects a known PAL metadata key.
Run: python3 tests/sms_region_tests.py --core CORE --output RESULT.json
"""
import argparse,ctypes as C,hashlib,json,os,runpy,struct,subprocess,sys,tempfile
from pathlib import Path
forge_crc=runpy.run_path(str(Path(__file__).with_name('sms_codemasters_mapper_tests.py')))['forge_crc']
P=C.c_void_p;U=C.c_uint;SZ=C.c_size_t
class Game(C.Structure):_fields_=[('path',C.c_char_p),('data',P),('size',SZ),('meta',C.c_char_p)]
class Var(C.Structure):_fields_=[('key',C.c_char_p),('value',C.c_char_p)]
class Geometry(C.Structure):_fields_=[('w',U),('h',U),('maxw',U),('maxh',U),('aspect',C.c_float)]
class Timing(C.Structure):_fields_=[('fps',C.c_double),('rate',C.c_double)]
class AV(C.Structure):_fields_=[('geometry',Geometry),('timing',Timing)]
def fixture(size=0x80000):
 raw=bytearray(size);raw[:10]=bytes.fromhex('f331f0df2100c03418fd');raw[0x7ff0:0x7ff8]=b'TMR SEGA';raw[0x7fff]=0x41
 return forge_crc(raw,0xc8718d40)
class Core:
 def __init__(self,path,data,filename='original.sms',options=None):
  self.lib=C.CDLL(path);self.keep=[];opts={'genesis_plus_gx_system_hw':'auto','genesis_plus_gx_region_detect':'auto','genesis_plus_gx_vdp_mode':'auto','genesis_plus_gx_bios':'disabled',**(options or {})}
  ENV=C.CFUNCTYPE(C.c_bool,U,P)
  @ENV
  def env(cmd,ptr):
   cmd&=65535
   if cmd in (9,30,31):v=b'/tmp';self.keep.append(v);C.cast(ptr,C.POINTER(C.c_char_p))[0]=v;return True
   if cmd==15:
    v=C.cast(ptr,C.POINTER(Var)).contents;val=opts.get(v.key.decode())
    if val is None:return False
    b=val.encode();self.keep.append(b);v.value=b;return True
   if cmd==16:
    vs=C.cast(ptr,C.POINTER(Var));i=0
    while vs[i].key:k=vs[i].key.decode();opts.setdefault(k,vs[i].value.decode().split('; ',1)[-1].split('|')[0]);i+=1
    return True
   if cmd==17:C.cast(ptr,C.POINTER(C.c_bool))[0]=False;return True
   if cmd==52:C.cast(ptr,C.POINTER(U))[0]=0;return True
   if cmd in (3,10,11,18,35,37,44,45,53,54,55,67,68,69):return True
   return False
  callbacks=[env,C.CFUNCTYPE(None,P,U,U,SZ)(lambda *_:None),C.CFUNCTYPE(None,C.c_int16,C.c_int16)(lambda *_:None),C.CFUNCTYPE(SZ,P,SZ)(lambda _,n:n),C.CFUNCTYPE(None)(lambda:None),C.CFUNCTYPE(C.c_int16,U,U,U,U)(lambda *_:0)]
  for name,cb in zip(['environment','video_refresh','audio_sample','audio_sample_batch','input_poll','input_state'],callbacks):getattr(self.lib,'retro_set_'+name)(cb)
  self.keep+=callbacks;self.lib.retro_load_game.argtypes=[C.POINTER(Game)];self.lib.retro_load_game.restype=C.c_bool;self.lib.retro_serialize_size.restype=SZ;self.lib.retro_serialize.argtypes=[P,SZ];self.lib.retro_serialize.restype=C.c_bool;self.lib.retro_unserialize.argtypes=[P,SZ];self.lib.retro_unserialize.restype=C.c_bool
  self.lib.retro_get_system_av_info.argtypes=[C.POINTER(AV)];self.lib.retro_kegarefusion_system_hw.restype=U
  self.lib.retro_init();self.raw=C.create_string_buffer(data);self.path=os.fsencode(filename);g=Game(self.path,C.cast(self.raw,P),len(data),None);assert self.lib.retro_load_game(C.byref(g))
  syms={}
  for line in subprocess.check_output(['nm','-an',path],text=True).splitlines():
   words=line.split()
   if len(words)==3:
    try:syms[words[2]]=int(words[0],16)
    except ValueError:pass
  self.base=C.cast(self.lib.retro_init,P).value-syms['retro_init'];self.syms=syms
 def frame(self):self.lib.retro_run()
 def stats(self):
  av=AV();self.lib.retro_get_system_av_info(C.byref(av));return {'hardware':int(self.lib.retro_kegarefusion_system_hw()),'region_code':int(C.c_ubyte.from_address(self.base+self.syms['region_code']).value),'vdp_pal':int(C.c_ubyte.from_address(self.base+self.syms['vdp_pal']).value),'fps':av.timing.fps,'sample_rate':av.timing.rate}
 def save(self):
  size=self.lib.retro_serialize_size();buf=C.create_string_buffer(size);assert self.lib.retro_serialize(buf,size);return buf.raw
 def restore(self,data):buf=C.create_string_buffer(data);return bool(self.lib.retro_unserialize(buf,len(data)))
 def close(self):self.lib.retro_unload_game();self.lib.retro_deinit()
def child(core,case):
 data=fixture(case.get('size',0x80000));temporary=tempfile.TemporaryDirectory(prefix='sms-region-');rompath=Path(temporary.name)/case.get('filename','original.sms');rompath.write_bytes(data);c=Core(core,data,str(rompath),case.get('options'));c.frame();actual=c.stats();result={'case':case['name'],'fixture_crc32':'C8718D40','fixture_size':len(data),'fixture_sha256':hashlib.sha256(data).hexdigest(),'actual':actual,'expected':case['expected']};result['pass']=all(actual[k]==v for k,v in case['expected'].items())
 if case.get('state_probe'):
  c.close();c=Core(core,data,str(rompath),options={'genesis_plus_gx_region_detect':'ntsc-u'});c.frame();ntsc=c.stats();saved=c.save();c.close();c=Core(core,data,str(rompath));c.frame();before=c.stats();accepted=c.restore(saved);after=c.stats();result['native_state_probe']={'saved_under':ntsc,'loaded_under':before,'state_accepted':accepted,'after_restore':after,'contains_console_cadence':after['vdp_pal']==ntsc['vdp_pal'],'finding':'Native GPX payload accepts the state but retains current console region/PAL clock. Movie replay needs its saved frameRate to pin the original cadence.'};assert accepted and ntsc['vdp_pal']==0 and after['vdp_pal']==before['vdp_pal']==1
 c.close();temporary.cleanup();return result
if __name__=='__main__':
 ap=argparse.ArgumentParser();ap.add_argument('--core',required=True);ap.add_argument('--output');ap.add_argument('--case');a=ap.parse_args()
 if a.case:print(json.dumps(child(a.core,json.loads(a.case))));raise SystemExit(0)
 cases=[{'name':'pal_auto','expected':{'hardware':33,'region_code':192,'vdp_pal':1},'state_probe':True},{'name':'pal_sms','options':{'genesis_plus_gx_system_hw':'master system'},'expected':{'hardware':32,'region_code':192,'vdp_pal':1}},{'name':'pal_sms2','options':{'genesis_plus_gx_system_hw':'master system II'},'expected':{'hardware':33,'region_code':192,'vdp_pal':1}},{'name':'manual_ntsc_wins','options':{'genesis_plus_gx_region_detect':'ntsc-u'},'expected':{'region_code':128,'vdp_pal':0}},{'name':'renamed_same_bytes','filename':'Anything At All.sms','expected':{'hardware':33,'region_code':192,'vdp_pal':1}},{'name':'manual_japan_wins','options':{'genesis_plus_gx_region_detect':'ntsc-j'},'expected':{'region_code':0,'vdp_pal':0}},{'name':'markIII_excluded','options':{'genesis_plus_gx_system_hw':'mark-III'},'expected':{'hardware':16,'region_code':128,'vdp_pal':0}},{'name':'gamegear_excluded','options':{'genesis_plus_gx_system_hw':'game gear'},'expected':{'hardware':65,'vdp_pal':0}},{'name':'same_crc_wrong_size_excluded','size':0x40000,'expected':{'hardware':33,'region_code':128,'vdp_pal':0}}]
 results=[]
 for case in cases:
  run=subprocess.run([sys.executable,__file__,'--core',a.core,'--case',json.dumps(case)],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  if run.returncode:raise RuntimeError(run.stderr)
  results.append(json.loads(run.stdout))
 # Rename only changes the path, not the content fingerprint or native mode.
 assert results[0]['fixture_sha256']==results[4]['fixture_sha256'] and results[0]['actual']==results[4]['actual']
 result={'core':str(Path(a.core).resolve()),'core_sha256':hashlib.sha256(Path(a.core).read_bytes()).hexdigest(),'fixture_scope':'Original generated loop, no copyrighted ROM bytes; exact CRC+size selects known PAL catalog rule. One core instance per child process.','pass':all(r['pass'] for r in results),'results':results}
 Path(a.output).write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({'pass':result['pass'],'failed':[r['case'] for r in results if not r['pass']]}));raise SystemExit(0 if result['pass'] else 1)
