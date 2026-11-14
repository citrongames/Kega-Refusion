#!/usr/bin/env python3
"""Original generated Z80 fixtures exercising the real libretro SMS core.
No game bytes are used. CRC forging selects the existing Codemasters board entry.
Run: python3 tests/sms_codemasters_mapper_tests.py --core CORE --output RESULT
"""
import argparse,ctypes as C,hashlib,json,os,struct,tempfile,zlib
P=C.c_void_p;U=C.c_uint;SZ=C.c_size_t
class Game(C.Structure):_fields_=[('path',C.c_char_p),('data',P),('size',SZ),('meta',C.c_char_p)]
class Var(C.Structure):_fields_=[('key',C.c_char_p),('value',C.c_char_p)]
def forge_crc(data,target=0x29822980):
 data=bytearray(data);data[-4:]=bytes(4);base=zlib.crc32(data);basis={}
 for i in range(32):
  q=bytearray(data);q[-4+i//8]^=1<<(i%8);v=zlib.crc32(q)^base;m=1<<i
  while v:
   b=v.bit_length()-1
   if b not in basis:basis[b]=(v,m);break
   x,y=basis[b];v^=x;m^=y
 v=target^base;m=0
 while v:
  x,y=basis[v.bit_length()-1];v^=x;m^=y
 for i in range(32):
  if m>>i&1:data[-4+i//8]^=1<<(i%8)
 assert zlib.crc32(data)==target
 return bytes(data)
class Asm:
 def __init__(self):self.b=bytearray();self.results=[];self.labels={};self.links=[]
 def emit(self,*b):self.b.extend(b)
 def word(self,w):self.b.extend(struct.pack('<H',w))
 def write(self,a,v):self.emit(0x3e,v,0x32);self.word(a)
 def read(self,a,value,label):
  out=0xd000+len(self.results);self.emit(0x3a);self.word(a);self.emit(0x32);self.word(out);self.results.append((label,value))
 def label(self,n):self.labels[n]=0xc100+len(self.b)
 def jump(self,n,op=0xc3):self.emit(op);self.links.append((len(self.b),n));self.word(0)
 def finish(self):
  for off,n in self.links:self.b[off:off+2]=struct.pack('<H',self.labels[n])
  return self.b

def fixture(mode):
 a=Asm()
 for ad,v in [(0x0100,0x40),(0x4100,0x41),(0x8100,0x40)]:a.read(ad,v,'reset_'+hex(ad))
 a.write(0,3);a.read(0,0x43,'slot0_includes_vectors');a.read(0x0100,0x43,'slot0_first1K');a.write(0,0)
 a.write(0x4000,2);a.read(0x4100,0x42,'slot1_bank2')
 a.write(0x8000,5);a.read(0x8100,0x45,'slot2_bank5');a.read(0xbfff,0x45,'slot2_end');a.read(0x8000,0x45,'register_read_is_ROM')
 for ad in [0xfffc,0xfffd,0xfffe,0xffff]:a.write(ad,7)
 a.read(0x0100,0x40,'SEGA_registers_no_effect_slot0');a.read(0x4100,0x42,'SEGA_registers_no_effect_slot1');a.read(0x8100,0x45,'SEGA_registers_no_effect_slot2')
 a.write(0x4000,0x82);a.read(0x4100,0x42,'RAM_bit_does_not_change_ROM_bank')
 a.write(0xa000,0xa5);a.write(0xbfff,0x5a);a.read(0xa000,0xa5,'RAM_low');a.read(0xbfff,0x5a,'RAM_high')
 a.write(0x8000,6);a.read(0x8100,0x46,'bank2_changes_under_RAM');a.read(0xa000,0xa5,'RAM_survives_bank2');a.read(0xbfff,0x5a,'RAM_high_survives_bank2')
 a.write(0x4000,2);a.read(0xa000,0x46,'RAM_disable_reveals_current_ROM');a.read(0xbfff,0x46,'RAM_disable_reveals_ROM_end')
 a.write(0x4000,0x82);a.read(0xa000,0xa5,'RAM_reenable_preserves_data')
 if mode=='mirrors':
  a.write(0x4000,2)
  for ad,bank in [(1,3),(0x3fff,4)]:a.write(ad,bank);a.read(0x0100,0x40+bank,'mirror_'+hex(ad))
  a.write(0,0)
  for ad,bank in [(0x4001,3),(0x7fff,4)]:a.write(ad,bank);a.read(0x4100,0x40+bank,'mirror_'+hex(ad))
  a.write(0x8000,2)
  for ad,bank in [(0x8001,6),(0x9fff,7),(0xa000,8),(0xbfff,9)]:a.write(ad,bank);a.read(0x8100,0x40+bank,'mirror_'+hex(ad))
  a.write(0x4000,0x82);a.write(0x9fff,10);a.read(0x8100,0x4a,'mirror_lower8K_with_RAM');a.read(0xa000,0xa5,'RAM_priority_over_alias');a.write(0xa000,0xa6);a.read(0x8100,0x4a,'RAM_write_does_not_bank2');a.read(0xa000,0xa6,'RAM_write_still_works')
 a.write(0,3);a.write(0x4000,0x84);a.write(0x8000,7);a.write(0xa000,0xa5);a.write(0xbfff,0x5a);a.write(0xd4ff,0xa7)
 a.label('wait');a.emit(0x3a);a.word(0xd400);a.emit(0xfe,1);a.jump('mutate',0xca);a.emit(0xfe,3);a.jump('inspect',0xca);a.jump('wait')
 a.label('mutate');a.write(0xa000,0x3c);a.write(0xbfff,0xc3);a.write(0,1);a.write(0x4000,2);a.write(0x8000,9);a.write(0xd401,1);a.write(0xd400,0);a.jump('wait')
 a.label('inspect')
 for k,ad in enumerate([0x0100,0x4100,0x8100,0xa000,0xbfff]):a.emit(0x3a);a.word(ad);a.emit(0x32);a.word(0xd410+k)
 a.write(0xd401,3);a.write(0xd400,0);a.jump('wait')
 code=a.finish();rom=bytearray().join(bytes([0x40+n])*0x4000 for n in range(16));boot=bytes.fromhex('f331f0df2100021100c101')+struct.pack('<H',len(code))+bytes.fromhex('edb0c300c1');rom[:len(boot)]=boot;rom[0x200:0x200+len(code)]=code;rom[0x7ff0:0x7ff8]=b'TMR SEGA';rom[0x7fff]=0x4c
 return forge_crc(rom),a.results
class Core:
 def __init__(self,path,rompath,data):
  self.lib=C.CDLL(path);self.keep=[];opts={'genesis_plus_gx_system_hw':'auto','genesis_plus_gx_region_detect':'auto','genesis_plus_gx_bios':'disabled','genesis_plus_gx_overscan':'disabled'}
  ENV=C.CFUNCTYPE(C.c_bool,U,P)
  @ENV
  def env(cmd,ptr):
   cmd&=0xffff
   if cmd in [9,30,31]:v=b'/tmp';self.keep.append(v);C.cast(ptr,C.POINTER(C.c_char_p))[0]=v;return True
   if cmd==10:return True
   if cmd==15:
    v=C.cast(ptr,C.POINTER(Var)).contents;k=v.key.decode();s=opts.get(k)
    if s is None:return False
    b=s.encode();self.keep.append(b);v.value=b;return True
   if cmd==16:
    vs=C.cast(ptr,C.POINTER(Var));i=0
    while vs[i].key:
     k=vs[i].key.decode();desc=vs[i].value.decode();opts.setdefault(k,desc.split('; ',1)[-1].split('|')[0]);i+=1
    return True
   if cmd==17:C.cast(ptr,C.POINTER(C.c_bool))[0]=False;return True
   if cmd==52:C.cast(ptr,C.POINTER(U))[0]=0;return True
   if cmd in [3,11,18,35,37,44,45,53,54,55,67,68,69]:return True
   return False
  callbacks=[env,C.CFUNCTYPE(None,P,U,U,SZ)(lambda *_:None),C.CFUNCTYPE(None,C.c_int16,C.c_int16)(lambda *_:None),C.CFUNCTYPE(SZ,P,SZ)(lambda _,n:n),C.CFUNCTYPE(None)(lambda:None),C.CFUNCTYPE(C.c_int16,U,U,U,U)(lambda *_:0)]
  for name,fn in zip(['environment','video_refresh','audio_sample','audio_sample_batch','input_poll','input_state'],callbacks):getattr(self.lib,'retro_set_'+name)(fn)
  self.keep+=callbacks;self.lib.retro_get_memory_data.argtypes=[U];self.lib.retro_get_memory_data.restype=P;self.lib.retro_get_memory_size.argtypes=[U];self.lib.retro_get_memory_size.restype=SZ;self.lib.retro_serialize_size.restype=SZ;self.lib.retro_serialize.argtypes=[P,SZ];self.lib.retro_unserialize.argtypes=[P,SZ];self.lib.retro_load_game.argtypes=[C.POINTER(Game)];self.lib.retro_load_game.restype=C.c_bool
  self.lib.retro_init();self.raw=C.create_string_buffer(data);self.path=os.fsencode(rompath);g=Game(self.path,C.cast(self.raw,P),len(data),None);assert self.lib.retro_load_game(C.byref(g)),'fixture load failed'
 def frame(self,n=1):
  for _ in range(n):self.lib.retro_run()
 def ram(self):return (C.c_ubyte*0x2000).from_address(self.lib.retro_get_memory_data(2))
 def battery(self):return (C.c_ubyte*0x10000).from_address(self.lib.retro_get_memory_data(0))
 def command(self,n):self.ram()[0x1400]=n;self.frame();assert self.ram()[0x1401]==n,'command did not execute'
 def close(self):self.lib.retro_unload_game();self.lib.retro_deinit()
def run(corepath,mode,directory):
 data,expected=fixture(mode);path=os.path.join(directory,mode+'.sms');open(path,'wb').write(data);c=Core(corepath,path,data);c.frame(2);ram=c.ram();assert ram[0x14ff]==0xa7,'Z80 fixture did not finish';checks=[{'name':label,'expected':v,'observed':ram[0x1000+i],'pass':ram[0x1000+i]==v} for i,(label,v) in enumerate(expected)];extra=[]
 if mode=='canonical':
  saved_battery=bytes(c.battery());size=c.lib.retro_serialize_size();state=C.create_string_buffer(size);assert c.lib.retro_serialize(state,size);c.command(1);assert c.lib.retro_unserialize(state,size);c.command(3);actual=bytes(c.ram()[0x1410:0x1415]);extra.append({'name':'native_state_restores_three_banks_RAM_enable_battery_is_separate','expected':'4344473cc3','observed':actual.hex(),'pass':actual==bytes.fromhex('4344473cc3')});C.memmove(c.lib.retro_get_memory_data(0),saved_battery,len(saved_battery));c.command(3);combined=bytes(c.ram()[0x1410:0x1415]);extra.append({'name':'state_plus_battery_roundtrip','expected':'434447a55a','observed':combined.hex(),'pass':combined==bytes.fromhex('434447a55a')})
  save_before=bytes(c.battery());size_running=c.lib.retro_get_memory_size(0);c.lib.retro_reset();preserved=bytes(c.battery())==save_before;extra.append({'name':'soft_reset_preserves_battery','pass':preserved});c.frame(2);again=c.ram();extra.append({'name':'soft_reset_default_banks_0_1_0','pass':bytes(again[0x1000:0x1003])==bytes([0x40,0x41,0x40])});extra.append({'name':'battery_export_includes_actual_mapped_RAM_offset_2000_3FFF','pass':size_running>=0x4000 and save_before[0x2000]==0xa5 and save_before[0x3fff]==0x5a,'exported_size':size_running})
  c.close();c=Core(corepath,path,data);C.memmove(c.lib.retro_get_memory_data(0),save_before,len(save_before));extra.append({'name':'battery_reload_public_memory_roundtrip','pass':bytes(c.battery())==save_before});c.frame(2)
 result={'mode':mode,'fixture_crc32':f'{zlib.crc32(data):08X}','fixture_sha256':hashlib.sha256(data).hexdigest(),'checks':checks+extra};result['pass']=all(x['pass'] for x in result['checks']);c.close();return result
def compatibility(corepath,directory):
 results=[]
 codies,_=fixture('canonical');ordinary=bytearray(codies);ordinary[-1]^=1
 md=bytearray(0x10000);struct.pack_into('>II',md,0,0xffff00,0x200);md[0x100:0x110]=b'SEGA GENESIS    ';md[0x180:0x18e]=b'GM 00000000-00';md[0x1f0:0x1f3]=b'JUE'
 for label,data,suffix in [('codemasters',codies,'.sms'),('ordinary_sms',bytes(ordinary),'.sms'),('ordinary_md_after_sms',bytes(md),'.md')]:
  path=os.path.join(directory,label+suffix);open(path,'wb').write(data);c=Core(corepath,path,data)
  for name in ['retro_kegarefusion_recording_abi','retro_kegarefusion_network_state_abi']:getattr(c.lib,name).restype=C.c_char_p
  recording=c.lib.retro_kegarefusion_recording_abi().decode();net=c.lib.retro_kegarefusion_network_state_abi();network=None if net is None else net.decode();results.append({'label':label,'recording_abi':recording,'network_abi':network});c.close()
 passed=results[0]['recording_abi']=='kegarefusion-recording-v3-peripherals-v1-sms-codemasters-v1' and results[1]['recording_abi']==results[2]['recording_abi']=='kegarefusion-recording-v3-peripherals-v1' and results[0]['network_abi']=='kegarefusion-network-state-v3-peripherals-v1-gpx-f27655b-x64le-32x-wire-v1-sms-codemasters-v1-sms-pause-v1' and results[1]['network_abi']=='kegarefusion-network-state-v3-peripherals-v1-gpx-f27655b-x64le-32x-wire-v1-sms-pause-v1' and results[2]['network_abi']=='kegarefusion-network-state-v3-peripherals-v1-gpx-f27655b-x64le-32x-wire-v1'
 return {'pass':passed,'cases':results}

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('--core',required=True);p.add_argument('--output',required=True);a=p.parse_args()
 with tempfile.TemporaryDirectory(prefix='sms-codies-') as td:
  results=[run(a.core,m,td) for m in ['canonical','mirrors']];abi=compatibility(a.core,td)
 out={'core':os.path.realpath(a.core),'core_sha256':hashlib.sha256(open(a.core,'rb').read()).hexdigest(),'results':results,'compatibility':abi};open(a.output,'w').write(json.dumps(out,indent=2)+'\n');print(json.dumps({'pass':(all(r['pass'] for r in results) and abi['pass']),'failed':[(r['mode'],x['name'],x.get('observed')) for r in results for x in r['checks'] if not x['pass']]}))
 raise SystemExit(0 if (all(r['pass'] for r in results) and abi['pass']) else 1)
