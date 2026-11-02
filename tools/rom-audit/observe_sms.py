#!/usr/bin/env python3
"""One SMS ROM/native libretro core per process, read-only source content.
Observes actual forwarded Z80 memory writes and memory-control port writes,
current mapper/FCR/bank maps, native SMS context and effective hardware/region.
Default controls press SMS button1/button2, with no MD Start/Pause pulse.
"""
import argparse,ctypes as C,hashlib,json,os,subprocess,sys,time,zlib
from pathlib import Path
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[2]
NATIVE_SOURCE=Path(__file__).resolve().with_name("sms_observer_native.c")
HERE=ROOT/"build/verification/sms-audit/native"
P=C.c_void_p; U=C.c_uint; SZ=C.c_size_t
class Var(C.Structure): _fields_=[('key',C.c_char_p),('value',C.c_char_p)]
class Game(C.Structure): _fields_=[('path',C.c_char_p),('data',P),('size',SZ),('meta',C.c_char_p)]
class Geom(C.Structure): _fields_=[('base_width',U),('base_height',U),('max_width',U),('max_height',U),('aspect_ratio',C.c_float)]
class Timing(C.Structure): _fields_=[('fps',C.c_double),('sample_rate',C.c_double)]
class AV(C.Structure): _fields_=[('geometry',Geom),('timing',Timing)]
class Info(C.Structure): _fields_=[('library_name',C.c_char_p),('library_version',C.c_char_p),('valid_extensions',C.c_char_p),('need_fullpath',C.c_bool),('block_extract',C.c_bool)]
class Row(C.Structure): _fields_=[('calls',C.c_uint64),('changed',C.c_uint64),('values',C.c_uint64*4),('handler',SZ)]+[(x,U) for x in ['address','kind','first_value','last_value','first_pc','last_pc','first_frame','last_frame','first_before_fcr','last_after_fcr','priority']]
class Event(C.Structure): _fields_=[('handler',SZ)]+[(x,U) for x in ['frame','pc','address','value','kind','before_fcr','after_fcr']]+[('before_map',SZ*6),('after_map',SZ*6)]
def sha_file(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def build_native(source):
    import re,fcntl
    source=Path(source).resolve();HERE.mkdir(parents=True,exist_ok=True)
    sms=(source/'core/cart_hw/sms_cart.c').read_bytes().decode('latin1')
    hw=re.search(r'typedef struct\s*\{\s*uint8 fcr\[4\];[^}]*\}\s*romhw_t;',sms)
    slot=re.search(r'static struct\s*\{\s*uint8 \*rom;[^}]*\}\s*slot;',sms)
    if not hw or not slot:raise RuntimeError('SMS local type declarations changed; inspect source before observing')
    slot_type=slot.group(0).replace('static struct','typedef struct',1).replace('} slot;','} sms_audit_slot_t;')
    exact_types=hw.group(0)+'\n'+slot_type+'\n'
    fingerprint=hashlib.sha256((str(source)+exact_types+NATIVE_SOURCE.read_text()).encode()).hexdigest()[:16]
    target=HERE/('sms_observer-'+fingerprint+'.so');header=HERE/'sms_observer_types.h'
    with open(str(target)+'.lock','w') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if not target.exists():
            header.write_text(exact_types)
            inc=[source/'core',source/'libretro',source/'libretro/libretro-common/include',HERE]+[p for p in (source/'core').rglob('*') if p.is_dir()]
            command=['gcc','-O2','-shared','-fPIC','-DLSB_FIRST','-D__LIBRETRO__','-DM68K_OVERCLOCK_SHIFT=20','-DZ80_OVERCLOCK_SHIFT=20','-DINLINE=static inline']+['-I'+str(i) for i in inc]
            temp=str(target)+'.tmp'
            built=subprocess.run(command+[str(NATIVE_SOURCE),'-o',temp],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            if built.returncode:raise RuntimeError(built.stderr.decode()[:6000])
            os.replace(temp,target)
    mapper_names={int(value,16):name for name,value in re.findall(r'#define\s+(MAPPER_\w+)\s+\((0x[0-9A-Fa-f]+)\)',sms)}
    return target,mapper_names,hashlib.sha256(exact_types.encode()).hexdigest()

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--rom',required=True);ap.add_argument('--output',required=True,help='Per-ROM JSON file; PNGs share its stem')
    ap.add_argument('--core',default=str(ROOT/'build/linux/core/genesis_plus_gx_libretro.so'))
    ap.add_argument('--source',default=str(ROOT/'build/deps/genesis-plus-gx-32x-linux'))
    ap.add_argument('--frames',type=int,default=1200);ap.add_argument('--no-hooks',action='store_true',help='Control run for video/audio comparison')
    ap.add_argument('--no-input',action='store_true');ap.add_argument('--no-png',action='store_true');ap.add_argument('--png-width',type=int,default=320)
    ap.add_argument('--checkpoints',default='',help='Comma-separated one-based frame numbers; default 120,300,480,600,900,final')
    ap.add_argument('--buttons',action='append',default=[],help='Additional FRAME:COUNT:MASK, zero-based libretro mask')
    ap.add_argument('--option',action='append',default=[],help='Core variable KEY=VALUE override')
    ap.add_argument('--bios-dir',default=str(HERE/'bios'),help='Read-only shared BIOS symlink directory; never populated per ROM')
    a=ap.parse_args();start=time.monotonic()
    if not 1<=a.frames<=36000: ap.error('--frames must be 1..36000')
    rom=Path(a.rom).resolve();output=Path(a.output).resolve();output.parent.mkdir(parents=True,exist_ok=True)
    raw=rom.read_bytes();digest=hashlib.sha256(raw).hexdigest(); corepath=Path(a.core).resolve(); corehash=sha_file(corepath)
    nativepath,mapper_names,types_hash=build_native(a.source);n=C.CDLL(str(nativepath));lib=C.CDLL(str(corepath))
    symbols={}
    for line in subprocess.check_output(['nm','-an',str(corepath)],text=True).splitlines():
        parts=line.split()
        if len(parts)==3:
            try: symbols[parts[2]]=int(parts[0],16)
            except ValueError: pass
    if any(k not in symbols for k in ['cart_rom','bios_rom','slot','Z80','z80_writemem','z80_writeport','z80_readmap','z80_writemap','ext','system_hw','region_code','vdp_pal','retro_init']): raise RuntimeError('Core must retain local symbols for exact native observation')
    base=C.cast(lib.retro_init,P).value-symbols['retro_init']; rev={v:k for k,v in symbols.items()}
    def name(ptr):
        if not ptr:return None
        off=ptr-base
        return {'name':rev.get(off,'unknown'),'offset':f'0x{off:x}'}
    pointer_functions=['sms_readmap','sms_writemap','sms_write_handler','sms_port_handler','sms_slot_rom','sms_rom_base']
    for key in pointer_functions:getattr(n,key).restype=SZ
    n.sms_init.argtypes=[SZ]*9
    n.sms_init(*[base+symbols[k] for k in ['cart_rom','slot','bios_rom','Z80','z80_writemem','z80_writeport','z80_readmap','z80_writemap','ext']])
    n.sms_rows.restype=C.POINTER(Row);n.sms_events.restype=C.POINTER(Event);n.sms_total.restype=C.c_uint64;n.sms_port_count.restype=C.c_uint64;n.sms_port_values.restype=C.c_uint64
    rombase=n.sms_rom_base();romcap=n.sms_rom_capacity()
    memory_regions=[('cart_buffer',rombase,romcap),('work_ram',base+symbols['work_ram'],0x10000),('z80_ram',base+symbols['zram'],0x2000),('sram',base+symbols['sram']+n.sms_sram_offset(),0x10000)]
    def mapped(ptr):
        if not ptr:return {'storage':'null'}
        for label,at,size in memory_regions:
            if at<=ptr<at+size:
                off=ptr-at;result={'storage':label,'offset':f'0x{off:x}'}
                if label=='cart_buffer':result['inside_loaded_rom']=off<len(raw)
                return result
        return {'storage':'unknown','core_relative_address':f'0x{ptr-base:x}'}
    def bytes4(packed):return [(packed>>(8*i))&255 for i in range(4)]
    context=C.CFUNCTYPE(C.c_int,P)(base+symbols['sms_cart_context_save']);contextbuf=C.create_string_buffer(16384)
    region_fn=C.CFUNCTYPE(C.c_int)(base+symbols['sms_cart_region_detect'])
    def native_snapshot(f):
        size=context(contextbuf)
        if not 0<=size<=len(contextbuf):raise RuntimeError('Unexpected SMS context size')
        payload=contextbuf.raw[:size]
        hw=C.c_ubyte.from_address(base+symbols['system_hw']).value;region=C.c_ubyte.from_address(base+symbols['region_code']).value;pal=C.c_ubyte.from_address(base+symbols['vdp_pal']).value
        return {'frame':f,'z80_pc':f'0x{n.sms_zpc():04x}','z80_halted':bool(n.sms_halted()),'z80_cycles':n.sms_zcycles(),'system_hw':hw,'system_name':{1:'SG1000',2:'SG1000II',3:'SG1000II_RAM_EXT',16:'MarkIII',32:'MasterSystem',33:'MasterSystemII',64:'GameGear',65:'GameGear_SMS',128:'MegaDrive',129:'PowerBaseConverter'}.get(hw,'unknown'),'region_code':region,'region_name':{0:'Japan_NTSC',64:'Japan_PAL',128:'USA_NTSC',192:'Europe_PAL'}.get(region,'unknown'),'vdp_pal':bool(pal),'fm_ym2413':{'configuration_bits':C.c_ubyte.from_address(base+symbols['config']+n.sms_fm_config_offset()).value,'enabled':bool(C.c_ubyte.from_address(base+symbols['config']+n.sms_fm_config_offset()).value&1),'automatic':bool(C.c_ubyte.from_address(base+symbols['config']+n.sms_fm_config_offset()).value&2)},'cartridge_mapper':{'id':n.sms_mapper(),'name':mapper_names.get(n.sms_mapper(),'unknown'),'pages':n.sms_pages(),'fcr':[n.sms_fcr(i) for i in range(4)]},'active_slot':{'mapper_id':n.sms_slot_mapper(),'mapper_name':mapper_names.get(n.sms_slot_mapper(),'unknown'),'pages':n.sms_slot_pages(),'fcr':[n.sms_slot_fcr(i) for i in range(4)],'rom':mapped(n.sms_slot_rom())},'bios_mapper':{'id':n.sms_bios_mapper(),'pages':n.sms_bios_pages()},'write_handler':name(n.sms_write_handler()),'port_write_handler':name(n.sms_port_handler()),'readmap_1k':[mapped(n.sms_readmap(i)) for i in range(64)],'writemap_1k':[mapped(n.sms_writemap(i)) for i in range(64)],'native_mapper_context':{'bytes':size,'fcr_hex':payload[:4].hex(),'sha256':hashlib.sha256(payload).hexdigest()}}
    opts={}; overrides=dict(x.split('=',1) for x in a.option)
    # Stable defaults are reported after the core registers its complete v0 variable table.
    overrides={'genesis_plus_gx_system_hw':'auto','genesis_plus_gx_region_detect':'auto','genesis_plus_gx_bios':'disabled','genesis_plus_gx_overscan':'disabled','genesis_plus_gx_addr_error':'enabled','genesis_plus_gx_force_dtack':'enabled',**overrides}
    owned_strings=[];pixfmt=0;frame=0;buttons=0;video_calls=0;dupes=0;video_samples=[];last_pixels=None;last_shape=None;audio_count=0;audio_nonzero=0;audio_peak=0;audio_blocks=0;audio_nonzero_blocks=0;audio_hash=hashlib.sha256();video_roll=hashlib.sha256();checkpoints=[];video_errors=[]
    checkset={int(x) for x in a.checkpoints.split(',') if x} if a.checkpoints else {120,300,480,600,900,a.frames}
    checkset={x for x in checkset if 1<=x<=a.frames}|{a.frames}
    pngset={min(120,a.frames),min(600,a.frames),a.frames}
    intervals=[] if a.no_input else [(120,12,1),(300,12,256),(480,12,1),(600,180,128),(660,12,256),(720,12,1),(840,120,128),(900,12,256),(960,12,1)]
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
    n.sms_frame(0);initial_fps=av.timing.fps;initial=native_snapshot(0);detected_region_hint=region_fn()
    if initial['system_hw'] not in (1,2,3,16,32,33,64,65,129):raise RuntimeError('Loaded hardware is not an SMS-family system: '+str(initial['system_hw']))
    if not a.no_hooks:n.sms_install()
    cpu_samples=[];audio_stages=[]
    for frame in range(a.frames):
        buttons=0
        for at,count,mask in intervals:
            if at<=frame<at+count:buttons|=mask
        n.sms_frame(frame+1);lib.retro_run()
        if frame+1 in checkset:
            cpu_samples.append(native_snapshot(frame+1))
            audio_stages.append({'frame':frame+1,'scalar_samples':audio_count,'nonzero_scalar_samples':audio_nonzero,'peak':audio_peak})
    rows=[]
    for i in range(n.sms_count()):
        rr=n.sms_rows()[i]
        row={field:getattr(rr,field) for field,_ in Row._fields_ if field not in ('values','handler')}
        row['handler']=name(rr.handler);row['values_seen']=[j for j in range(256) if rr.values[j//64]&(1<<(j%64))];row['address']=f'0x{rr.address:04x}';row['kind']='memory_write' if rr.kind==0 else 'port_write';row['first_pc']=f'0x{rr.first_pc:04x}';row['last_pc']=f'0x{rr.last_pc:04x}';row['first_before_fcr']=bytes4(rr.first_before_fcr);row['last_after_fcr']=bytes4(rr.last_after_fcr);rows.append(row)
    rows.sort(key=lambda r:(r['kind'],r['address']))
    events=[]
    for i in range(n.sms_event_count()):
        e=n.sms_events()[i];events.append({'frame':e.frame,'z80_pc':f'0x{e.pc:04x}','address':f'0x{e.address:04x}','value':e.value,'kind':'memory_write' if e.kind==0 else 'port_write','handler':name(e.handler),'before_fcr':bytes4(e.before_fcr),'after_fcr':bytes4(e.after_fcr),'before_8k_bank_bases':[mapped(p) for p in e.before_map],'after_8k_bank_bases':[mapped(p) for p in e.after_map]})
    final=native_snapshot(a.frames);lib.retro_get_system_av_info(C.byref(av));lib.retro_get_region.restype=U
    result={'schema':'kega-sms-observer-v1','status':'completed','rom':{'path':str(rom),'size':len(raw),'sha256':digest,'crc32':f'{zlib.crc32(raw)&0xffffffff:08x}'},'core':{'path':str(corepath),'sha256':corehash,'library_name':info.library_name.decode(),'library_version':info.library_version.decode(),'current_source_tree':str(Path(a.source).resolve()),'current_sms_cart_c_sha256':sha_file(Path(a.source)/'core/cart_hw/sms_cart.c'),'exact_extracted_local_types_sha256':types_hash,'native_observer_sha256':sha_file(nativepath),'symbol_provenance':'Exact local ELF .symtab via nm -an and exported retro_init load base. Z80 and SRAM types compiled from core headers; local romhw_t and slot declarations extracted verbatim from the reported current sms_cart.c. ELF hash identifies actual binary; current source tree is not independently a compiled-source attestation.','abi':{'romhw_size':n.sms_romhw_size(),'slot_size':n.sms_slot_size(),'romhw_fcr_offset':n.sms_fcr_offset(),'romhw_mapper_offset':n.sms_mapper_offset(),'romhw_pages_offset':n.sms_pages_offset(),'z80_size':n.sms_z80_size()}},'settings':{'frames':a.frames,'core_options':opts,'port1':'pad','port2':'disconnected','input_intervals':[{'at_frame_zero_based':x,'count':y,'libretro_mask':z} for x,y,z in intervals],'sms_button_masks':{'button1_B':1,'button2_A':256,'right':128,'pause_Start':8},'bios_directory':str(Path(a.bios_dir).resolve()),'user_saves_loaded':False,'hooks_enabled':not a.no_hooks,'rom_bytes_modified':False},'timing':{'initial_fps':initial_fps,'fps':av.timing.fps,'sample_rate':av.timing.sample_rate,'libretro_region':'PAL' if lib.retro_get_region() else 'NTSC','sms_cart_region_detect_hint':detected_region_hint,'elapsed_seconds':round(time.monotonic()-start,3)},'video':{'callback_count':video_calls,'duplicate_callbacks':dupes,'pixel_format':pixfmt,'sample_interval':30,'sample_count':len(video_samples),'unique_sampled_hashes':len(set(h for _,h in video_samples)),'sampled_sequence_sha256':video_roll.hexdigest(),'checkpoints':checkpoints,'errors':video_errors},'audio':{'scalar_samples':audio_count,'nonzero_scalar_samples':audio_nonzero,'nonzero_blocks':audio_nonzero_blocks,'blocks':audio_blocks,'peak_pcm16':audio_peak,'sha256_pcm16le':audio_hash.hexdigest(),'checkpoints':audio_stages},'initial_hardware':initial,'hardware_checkpoints':cpu_samples,'final_hardware':final,'hardware':{'observation_scope':'Forwards actual SMS Z80 write-memory and write-port callbacks unchanged, restoring native callback identity during each call. All writes belowC000 and standard mapper candidates are retained up to bounds; ordinary RAM writes counted. Actual FCR/bank changes are priority rows. Ports3E/3F observed; direct memory reads remain untouched. FCR contexts are native read-only snapshots, not full state restore tests. Bounded execution is not a playability verdict.','totals':{key:int(n.sms_total(i)) for i,key in enumerate(['memory_writes','ordinary_ram_and_high_writes','cartridge_range_writes','mapper_candidate_or_change_calls','bank_or_fcr_changed_calls','aggregate_overflow_calls','nonpriority_unrecorded_calls','all_port_writes'])},'port_write_histogram':[{'port_low_byte':f'0x{p:02x}','calls':int(n.sms_port_count(p)),'values_seen':[v for v in range(256) if n.sms_port_values(p,v//64)&(1<<(v%64))]} for p in range(256) if n.sms_port_count(p)],'aggregates':rows,'first_unique_accesses':events,'aggregate_limit':2048,'nonpriority_row_limit':256}}
    n.sms_remove();lib.retro_unload_game();lib.retro_deinit()
    # No fwrite/save-memory serialization. Only audit results are persisted.
    temp=output.with_suffix(output.suffix+'.tmp');temp.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n');os.replace(temp,output)
    print(json.dumps({'status':'completed','rom_sha256':digest,'output':str(output),'frames':a.frames,'elapsed_seconds':result['timing']['elapsed_seconds'],'hardware_rows':len(rows),'video_unique':result['video']['unique_sampled_hashes'],'audio_nonzero':audio_nonzero}))
if __name__=='__main__':
    try:main()
    except Exception as ex:
        print(json.dumps({'status':'failed','error':str(ex),'type':type(ex).__name__}),file=sys.stderr);raise
