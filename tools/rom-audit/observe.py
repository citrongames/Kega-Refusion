#!/usr/bin/env python3
"""One native libretro core/ROM per process; no ROM, save, settings or BIOS writes.
Handler-dispatched 68K and Z80-bank cartridge/expansion accesses are observed.
Direct ROM/RAM fetches are intentionally untouched. Generic A1 TIME dispatch can forward to a
known cart handler, whose name is included. See hardware.observation_scope.
"""
import argparse,ctypes as C,hashlib,json,os,subprocess,sys,time,zlib
from pathlib import Path
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
NATIVE_SOURCE=Path(__file__).resolve().with_name("observer_native.c")
HERE=ROOT/"build/verification/rom-audit"
P=C.c_void_p; U=C.c_uint; SZ=C.c_size_t
class Var(C.Structure): _fields_=[('key',C.c_char_p),('value',C.c_char_p)]
class Game(C.Structure): _fields_=[('path',C.c_char_p),('data',P),('size',SZ),('meta',C.c_char_p)]
class Geom(C.Structure): _fields_=[('base_width',U),('base_height',U),('max_width',U),('max_height',U),('aspect_ratio',C.c_float)]
class Timing(C.Structure): _fields_=[('fps',C.c_double),('sample_rate',C.c_double)]
class AV(C.Structure): _fields_=[('geometry',Geom),('timing',Timing)]
class Info(C.Structure): _fields_=[('library_name',C.c_char_p),('library_version',C.c_char_p),('valid_extensions',C.c_char_p),('need_fullpath',C.c_bool),('block_extract',C.c_bool)]
class Row(C.Structure): _fields_=[('calls',C.c_uint64),('handler',SZ)]+[(x,U) for x in ['address','first_value','last_value','min_value','max_value','first_pc','last_pc','first_frame','last_frame','category','kind']]
class Event(C.Structure): _fields_=[(x,U) for x in ['frame','pc','address','value','category','kind']]+[('handler',SZ)]
CAT=['cartridge','expansion_unmapped','time','a0_exceptional','a1_exceptional','standard_control','z80_cartridge','z80_expansion_unmapped','z80_time','z80_a0_exceptional','z80_a1_exceptional','z80_standard_control']
KIND=['read8','read16','write8','write16']
def sha_file(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def build_native(source):
    source=Path(source).resolve(); HERE.mkdir(parents=True,exist_ok=True); target=HERE/('observer_native-'+hashlib.sha256(str(source).encode()).hexdigest()[:8]+'.so')
    newest=max(NATIVE_SOURCE.stat().st_mtime,(source/'core/m68k/m68k.h').stat().st_mtime,(source/'core/cart_hw/md_cart.h').stat().st_mtime)
    if not target.exists() or target.stat().st_mtime<newest:
        # Serialize compile by source path across worker processes.
        import fcntl
        with open(str(target)+'.lock','w') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            if not target.exists() or target.stat().st_mtime<newest:
                inc=[str(source/d) for d in ['core','core/z80','core/m68k','core/cart_hw','core/cd_hw','core/input_hw','core/sound','core/ntsc','libretro','libretro/libretro-common/include']]
                inc += [str(d) for d in (source/'core').rglob('*') if d.is_dir()]
                cmd=['gcc','-O2','-shared','-fPIC','-DLSB_FIRST','-D__LIBRETRO__','-DM68K_OVERCLOCK_SHIFT=20','-DZ80_OVERCLOCK_SHIFT=20','-DINLINE=static inline']
                cmd += ['-I'+str(source/'core'),'-I'+str(source/'libretro')]+['-I'+d for d in inc]
                temp=str(target)+'.tmp'; subprocess.run(cmd+[str(NATIVE_SOURCE),'-o',temp],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE);os.replace(temp,target)
    return target

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rom',required=True);ap.add_argument('--output',required=True,help='Per-ROM JSON file; PNGs share its stem')
    ap.add_argument('--core',default=str(ROOT/'build/core32x-linux/core/genesis_plus_gx_libretro.so'))
    ap.add_argument('--source',default=str(ROOT/'build/deps/genesis-plus-gx-32x-linux'))
    ap.add_argument('--frames',type=int,default=1200);ap.add_argument('--no-hooks',action='store_true',help='Control run for video/audio comparison')
    ap.add_argument('--auto-checksum',action='store_true');ap.add_argument('--no-input',action='store_true');ap.add_argument('--no-png',action='store_true');ap.add_argument('--png-width',type=int,default=320)
    ap.add_argument('--checkpoints',default='',help='Comma-separated one-based frame numbers; default 120,300,480,600,900,final')
    ap.add_argument('--buttons',action='append',default=[],help='Additional FRAME:COUNT:MASK, zero-based libretro mask')
    ap.add_argument('--option',action='append',default=[],help='Core variable KEY=VALUE override')
    ap.add_argument('--bios-dir',default=str(HERE/'bios'),help='Read-only shared BIOS symlink directory; never populated per ROM')
    a=ap.parse_args();start=time.monotonic()
    if not 1<=a.frames<=36000: ap.error('--frames must be 1..36000')
    rom=Path(a.rom).resolve();output=Path(a.output).resolve();output.parent.mkdir(parents=True,exist_ok=True)
    raw=rom.read_bytes();digest=hashlib.sha256(raw).hexdigest(); corepath=Path(a.core).resolve(); corehash=sha_file(corepath)
    nativepath=build_native(a.source);n=C.CDLL(str(nativepath));lib=C.CDLL(str(corepath))
    original_raw=raw; checksum_guard=None
    if a.auto_checksum and len(raw)>=512 and len(raw)%2==0 and raw[0x100:0x104]==b'SEGA':
        calculated=sum(int.from_bytes(raw[i:i+2],'big') for i in range(512,len(raw),2))&65535
        guard=lib.retro_kegarefusion_preserve_header_checksum
        guard.argtypes=[P,SZ,U];guard.restype=C.c_int
        sourcebuf=C.create_string_buffer(raw);checksum_guard=bool(guard(sourcebuf,len(raw),calculated))
        if not checksum_guard:raw=raw[:0x18e]+calculated.to_bytes(2,'big')+raw[0x190:]

    symbols={}
    for line in subprocess.check_output(['nm','-an',str(corepath)],text=True).splitlines():
        parts=line.split()
        if len(parts)==3:
            try: symbols[parts[2]]=int(parts[0],16)
            except ValueError: pass
    if any(k not in symbols for k in ['m68k','ext','Z80','zstate','retro_init']): raise RuntimeError('Core must retain local symbols for exact native observation')
    base=C.cast(lib.retro_init,P).value-symbols['retro_init']; rev={v:k for k,v in symbols.items()}
    def name(ptr):
        if not ptr:return None
        off=ptr-base
        return {'name':rev.get(off,'unknown'),'offset':f'0x{off:x}'}
    for k in ['obs_time_r','obs_time_w','obs_regs_r','obs_regs_w']: getattr(n,k).restype=SZ
    n.obs_init.argtypes=[SZ,SZ,SZ,SZ,SZ];n.obs_init(base+symbols['m68k'],base+symbols['ext'],base+symbols['Z80'],base+symbols['zstate'],base+symbols['zbank_memory_map'])
    n.obs_rows.restype=C.POINTER(Row);n.obs_events.restype=C.POINTER(Event);n.obs_dropped.restype=C.c_uint64;n.obs_total.restype=C.c_uint64
    n.obs_known.argtypes=[SZ]*4;n.obs_known(*[base+symbols.get(k,0) for k in ['m68k_read_bus_8','m68k_read_bus_16','m68k_unused_8_w','m68k_unused_16_w']])
    n.obs_limit_handler.argtypes=[SZ,U];n.obs_limit_ptr.restype=SZ;n.obs_limit_dropped.restype=C.c_uint64
    for k,cap in [('sram_read_byte',128),('sram_read_word',128),('sram_write_byte',128),('sram_write_word',128),('m68k_read_bus_8',256),('m68k_read_bus_16',256),('m68k_unused_8_w',256),('m68k_unused_16_w',256),('zbank_unused_r',256),('zbank_unused_w',256)]:
        if k in symbols:n.obs_limit_handler(base+symbols[k],cap)
    opts={}; overrides=dict(x.split('=',1) for x in a.option)
    # Stable defaults are reported after the core registers its complete v0 variable table.
    overrides={'genesis_plus_gx_system_hw':'auto','genesis_plus_gx_region_detect':'auto','genesis_plus_gx_bios':'disabled','genesis_plus_gx_overscan':'disabled','genesis_plus_gx_addr_error':'enabled','genesis_plus_gx_force_dtack':'enabled',**overrides}
    owned_strings=[];pixfmt=0;frame=0;buttons=0;video_calls=0;dupes=0;video_samples=[];last_pixels=None;last_shape=None;audio_count=0;audio_nonzero=0;audio_peak=0;audio_blocks=0;audio_nonzero_blocks=0;audio_hash=hashlib.sha256();video_roll=hashlib.sha256();checkpoints=[];video_errors=[]
    checkset={int(x) for x in a.checkpoints.split(',') if x} if a.checkpoints else {120,300,480,600,900,a.frames}
    checkset={x for x in checkset if 1<=x<=a.frames}|{a.frames}
    pngset={min(120,a.frames),min(600,a.frames),a.frames}
    intervals=[] if a.no_input else [(120,12,8),(300,12,8),(480,12,8),(600,180,128),(660,12,256),(720,12,1),(780,12,2),(840,120,128),(900,12,256),(960,12,1),(1020,12,2)]
    for x in a.buttons:
        st,ct,mask=x.split(':');intervals.append((int(st),int(ct),int(mask,0)))
    sysdir=os.fsencode(Path(a.bios_dir).resolve()); savedir=os.fsencode(HERE/'no-user-saves');owned_strings.extend([sysdir,savedir])
    ENV=C.CFUNCTYPE(C.c_bool,U,P)
    @ENV
    def env(cmd,data):
        nonlocal pixfmt
        cmd &= 0xFFFF
        if cmd in (9,30,31): C.cast(data,C.POINTER(C.c_char_p))[0]=sysdir if cmd!=31 else savedir;return True
        if cmd==3:C.cast(data,C.POINTER(C.c_bool))[0]=True;return True
        if cmd==10:pixfmt=C.cast(data,C.POINTER(C.c_int))[0];return pixfmt in (0,1,2)
        if cmd==15:
            v=C.cast(data,C.POINTER(Var)).contents;k=v.key.decode();val=overrides.get(k,opts.get(k))
            if val is None:return False
            b=val.encode();owned_strings.append(b);v.value=b;opts[k]=val;return True
        if cmd==16:
            vs=C.cast(data,C.POINTER(Var));i=0
            while vs[i].key:
                k=vs[i].key.decode();desc=(vs[i].value or b'').decode();opts[k]=overrides.get(k,desc.split('; ',1)[-1].split('|')[0]);i+=1
            return True
        if cmd==17:C.cast(data,C.POINTER(C.c_bool))[0]=False;return True
        if cmd==52:C.cast(data,C.POINTER(U))[0]=0;return True
        if cmd==39:C.cast(data,C.POINTER(U))[0]=0;return True
        if cmd==51:return True
        if cmd in (11,18,35,37,44,45,53,54,55,67,68,69):return True
        if cmd==49:C.cast(data,C.POINTER(C.c_bool))[0]=False;return True
        return False
    def image_rgb(data,w,h,pitch):
        ar=np.frombuffer(data,dtype=np.uint8).reshape(h,pitch)
        if pixfmt==1:return ar[:,:w*4].reshape(h,w,4)[:,:,:3][:,:,::-1].copy()
        px=ar[:,:w*2].copy().view(np.uint16).reshape(h,w).astype(np.uint32)
        if pixfmt==2:return np.stack([((px>>11)&31)*255//31,((px>>5)&63)*255//63,(px&31)*255//31],axis=2).astype(np.uint8)
        return np.stack([((px>>10)&31)*255//31,((px>>5)&31)*255//31,(px&31)*255//31],axis=2).astype(np.uint8)
    VIDEO=C.CFUNCTYPE(None,P,U,U,SZ)
    @VIDEO
    def video(data,w,h,pitch):
        nonlocal video_calls,dupes,last_pixels,last_shape
        video_calls+=1
        if not data:dupes+=1;return
        f=frame+1
        if f not in checkset and f not in pngset and f%30!=0:return
        try:
            rawpixels=C.string_at(data,h*pitch);rgb=image_rgb(rawpixels,w,h,pitch);hh=hashlib.sha256(rgb.tobytes()).hexdigest();video_roll.update(f'{f}:{w}:{h}:{hh}\n'.encode())
            item={'frame':f,'width':w,'height':h,'sha256_rgb':hh,'unique_colors':int(np.unique(rgb.reshape(-1,3),axis=0).shape[0]) if f in checkset else None,'mean_rgb':[round(float(x),3) for x in rgb.mean(axis=(0,1))] if f in checkset else None}
            video_samples.append((f,hh));last_pixels=rgb;last_shape=(w,h)
            if f in pngset and not a.no_png:
                im=Image.fromarray(rgb);mw=max(1,a.png_width)
                if w>mw:im=im.resize((mw,max(1,round(h*mw/w))),Image.Resampling.NEAREST)
                path=output.with_suffix('');path=path.parent/(path.name+f'.f{f:05d}.png');im.save(path);item['png']=str(path)
            if f in checkset:checkpoints.append(item)
        except Exception as ex:video_errors.append(str(ex))
    AUDIO=C.CFUNCTYPE(None,C.c_int16,C.c_int16)
    BATCH=C.CFUNCTYPE(SZ,C.POINTER(C.c_int16),SZ)
    @AUDIO
    def aud(l,r):
        nonlocal audio_count,audio_nonzero,audio_peak,audio_blocks,audio_nonzero_blocks
        audio_count+=2;audio_nonzero+=int(l!=0)+int(r!=0);audio_peak=max(audio_peak,abs(l),abs(r));audio_hash.update(bytes((l&255,(l>>8)&255,r&255,(r>>8)&255)))
    @BATCH
    def batch(ptr,count):
        nonlocal audio_count,audio_nonzero,audio_peak,audio_blocks,audio_nonzero_blocks
        audio_blocks+=1;audio_count+=int(count)*2
        data=C.string_at(ptr,int(count)*4);ar=np.frombuffer(data,dtype=np.int16);nz=int(np.count_nonzero(ar));audio_nonzero+=nz;audio_nonzero_blocks+=int(nz>0)
        if len(ar):audio_peak=max(audio_peak,int(ar.max()),-int(ar.min()))
        audio_hash.update(data);return count
    POLL=C.CFUNCTYPE(None);STATE=C.CFUNCTYPE(C.c_int16,U,U,U,U)
    @POLL
    def poll():pass
    @STATE
    def state(port,device,index,id):
        if port!=0 or device!=1:return 0
        if id==256:return buttons if buttons<32768 else buttons-65536
        return (buttons>>id)&1 if id<16 else 0
    for fn,cb in [('environment',env),('video_refresh',video),('audio_sample',aud),('audio_sample_batch',batch),('input_poll',poll),('input_state',state)]:
        setter=getattr(lib,'retro_set_'+fn);setter.argtypes=[type(cb)];setter(cb)
    lib.retro_get_system_info.argtypes=[C.POINTER(Info)];info=Info();lib.retro_get_system_info(C.byref(info));lib.retro_init()
    lib.retro_load_game.argtypes=[C.POINTER(Game)];lib.retro_load_game.restype=C.c_bool
    # A private memory copy prevents any core byte swapping from changing the user file.
    gamebuf=C.create_string_buffer(raw);g=Game(os.fsencode(rom),C.cast(gamebuf,P),len(raw),None)
    if not lib.retro_load_game(C.byref(g)):raise RuntimeError('retro_load_game rejected ROM')
    lib.retro_set_controller_port_device.argtypes=[U,U];lib.retro_set_controller_port_device(0,1);lib.retro_set_controller_port_device(1,0)
    av=AV();lib.retro_get_system_av_info(C.byref(av))
    initial_handlers={k:name(getattr(n,'obs_'+k)()) for k in ['time_r','time_w','regs_r','regs_w']}
    # refresh initializes handler snapshots even without interception, via install/remove.
    n.obs_install()
    if a.no_hooks:n.obs_remove()
    initial_handlers={k:name(getattr(n,'obs_'+k)()) for k in ['time_r','time_w','regs_r','regs_w']}
    cpu_samples=[];audio_stages=[]
    for frame in range(a.frames):
        buttons=0
        for at,count,mask in intervals:
            if at<=frame<at+count:buttons|=mask
        n.obs_frame(frame+1);lib.retro_run()
        if frame+1 in checkset:
            cpu_samples.append({'frame':frame+1,'m68k_pc':f'0x{n.obs_pc():06x}','z80_pc':f'0x{n.obs_zpc():04x}','m68k_stopped':n.obs_stopped(),'zstate':n.obs_zstate()})
            audio_stages.append({'frame':frame+1,'scalar_samples':audio_count,'nonzero_scalar_samples':audio_nonzero,'peak':audio_peak})
    rows=[]
    for i in range(n.obs_count()):
        rr=n.obs_rows()[i];r={field:getattr(rr,field) for field,_ in Row._fields_};r['category']=CAT[r['category']];r['kind']=KIND[r['kind']];r['handler']=name(r['handler']);r['handler_behavior']='open_bus' if r['handler'] and r['handler']['name'] in ('m68k_read_bus_8','m68k_read_bus_16','zbank_unused_r') else 'ignored_write' if r['handler'] and r['handler']['name'] in ('m68k_unused_8_w','m68k_unused_16_w','zbank_unused_w') else 'native_mapped_handler';r['address']=f'0x{r["address"]:06x}'
        r['first_pc']=f'0x{r["first_pc"]:06x}';r['last_pc']=f'0x{r["last_pc"]:06x}';rows.append(r)
    rows.sort(key=lambda x:(x['category'],x['address'],x['kind']))
    events=[]
    for i in range(n.obs_event_count()):
        e=n.obs_events()[i];events.append({'frame':e.frame,'pc':f'0x{e.pc:06x}','address':f'0x{e.address:06x}','value':e.value,'category':CAT[e.category],'kind':KIND[e.kind],'handler':name(e.handler)})
    totals={c:{k:int(n.obs_total(ci,ki)) for ki,k in enumerate(KIND)} for ci,c in enumerate(CAT)}
    final_handlers={k:name(getattr(n,'obs_'+k)()) for k in ['time_r','time_w','regs_r','regs_w']}
    result={'schema':'kega-exhaustive-observer-v1.1','status':'completed','rom':{'path':str(rom),'size':len(raw),'sha256':digest,'crc32':f'{zlib.crc32(original_raw)&0xffffffff:08x}'},'core':{'path':str(corepath),'sha256':corehash,'library_name':info.library_name.decode(),'library_version':info.library_version.decode(),'source':str(Path(a.source).resolve()),'source_md_cart_sha256':sha_file(Path(a.source)/'core/cart_hw/md_cart.c'),'source_abi_header_sha256':{str(rel):sha_file(Path(a.source)/rel) for rel in ['core/m68k/m68k.h','core/cart_hw/md_cart.h','core/z80/z80.h','core/membnk.h','core/genesis.h']},'symbol_provenance':'Exact ELF .symtab values from nm -an; load base derived from exported retro_init; observer struct sizes and offsets compiled from reported source headers.','native_observer_sha256':sha_file(nativepath),'abi':{'m68k_size':n.obs_m68k_size(),'m68k_pc_offset':n.obs_pc_offset(),'memory_map_entry_size':n.obs_map_size(),'cart_size':n.obs_cart_size(),'cart_hw_offset':n.obs_cart_hw_offset()}},'settings':{'frames':a.frames,'core_options':opts,'port1':'pad','port2':'disconnected','input_intervals':[{'at_frame_zero_based':x,'count':y,'libretro_mask':z} for x,y,z in intervals],'bios_directory':str(Path(a.bios_dir).resolve()),'user_saves_loaded':False,'hooks_enabled':not a.no_hooks,'checksums_modified':raw!=original_raw,'auto_checksum_requested':a.auto_checksum,'checksum_guard_preserved':checksum_guard,'loaded_sha256':hashlib.sha256(raw).hexdigest(),'loaded_crc32':f'{zlib.crc32(raw)&0xffffffff:08x}'},'timing':{'fps':av.timing.fps,'sample_rate':av.timing.sample_rate,'elapsed_seconds':round(time.monotonic()-start,3)},'video':{'callback_count':video_calls,'duplicate_callbacks':dupes,'pixel_format':pixfmt,'sample_interval':30,'sample_count':len(video_samples),'unique_sampled_hashes':len(set(h for _,h in video_samples)),'sampled_sequence_sha256':video_roll.hexdigest(),'checkpoints':checkpoints,'errors':video_errors},'audio':{'scalar_samples':audio_count,'nonzero_scalar_samples':audio_nonzero,'nonzero_blocks':audio_nonzero_blocks,'blocks':audio_blocks,'peak_pcm16':audio_peak,'sha256_pcm16le':audio_hash.hexdigest(),'checkpoints':audio_stages},'cpu_checkpoints':cpu_samples,'hardware':{'observation_scope':'Forwarded non-null 68K memory-map handlers below C00000. Direct ROM/RAM accesses are not intercepted. Non-null Z80 bank handlers below C00000 are observed in z80_* categories. Standard IO/Z80 RAM accesses counted separately; A0 unavailable-bus or non-RAM accesses retained. A1 TIME dispatcher calls count once; named active native TIME handler distinguishes known forwarding. Handler maps refreshed every frame and after writes; all map hooks temporarily restored around cartridge/TIME writes so mapper pointer tests and map copies see native handlers. This is observed execution coverage, not proof of gameplay correctness.','initial_handlers':initial_handlers,'final_handlers':final_handlers,'mapper_flags':{'realtec':n.obs_realtec(),'bankshift':n.obs_bankshift(),'special':n.obs_special(),'core_romsize':n.obs_romsize()},'totals':totals,'aggregates':rows,'first_unique_accesses':events,'aggregate_limit':4096,'aggregate_overflow_calls':int(n.obs_dropped()),'noisy_handler_limits':[{'handler':name(n.obs_limit_ptr(i)),'row_cap':n.obs_limit_cap(i),'stored_rows':n.obs_limit_rows(i),'unrecorded_calls':int(n.obs_limit_dropped(i))} for i in range(n.obs_limits_count())]}}
    n.obs_remove();lib.retro_unload_game();lib.retro_deinit()
    # No fwrite/save-memory serialization. Only audit results are persisted.
    temp=output.with_suffix(output.suffix+'.tmp');temp.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');os.replace(temp,output)
    print(json.dumps({'status':'completed','rom_sha256':digest,'output':str(output),'frames':a.frames,'elapsed_seconds':result['timing']['elapsed_seconds'],'hardware_rows':len(rows),'video_unique':result['video']['unique_sampled_hashes'],'audio_nonzero':audio_nonzero}))
if __name__=='__main__':
    try:main()
    except Exception as ex:
        print(json.dumps({'status':'failed','error':str(ex),'type':type(ex).__name__}),file=sys.stderr);raise
