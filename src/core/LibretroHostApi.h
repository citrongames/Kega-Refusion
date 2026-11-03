#pragma once
#include "LibretroHost.h"
struct LibretroHost::Api {
#define API(name) decltype(&retro_##name) name = nullptr
    API(init); API(deinit); API(api_version); API(get_system_info); API(get_system_av_info);
    API(set_environment); API(set_video_refresh); API(set_audio_sample); API(set_audio_sample_batch);
    API(set_input_poll); API(set_input_state); API(set_controller_port_device);
    API(load_game); API(unload_game); API(run); API(reset); API(serialize_size); API(serialize);
    API(unserialize); API(get_memory_data); API(get_memory_size);
    API(cheat_reset); API(cheat_set);
#undef API
    unsigned char* (*link_io)() = nullptr;
    void (*link_connect)(unsigned char*, void(*)(void*,unsigned), void*) = nullptr;
    void (*link_deadline)(unsigned) = nullptr;
    unsigned (*link_latch)() = nullptr;
    void (*link_restore_latch)(unsigned) = nullptr;
    size_t (*link_audio_size)() = nullptr;
    bool (*link_audio_save)(void*,size_t) = nullptr;
    bool (*link_audio_load)(const void*,size_t) = nullptr;
    void (*manage_bram)(bool) = nullptr;
    bool (*get_bram)(unsigned, const void**, size_t*, const char**) = nullptr;
    const char* (*state_abi)() = nullptr;
    const char* (*recording_abi)() = nullptr;
    const char* (*network_state_abi)() = nullptr;
    unsigned (*features)() = nullptr;
    unsigned (*system_hw)() = nullptr;
    void (*debug_masks)(uint32_t,uint64_t) = nullptr;
    void (*debug_backdrop)(uint32_t) = nullptr;
    bool (*multitap)(unsigned, unsigned, unsigned) = nullptr;
    bool (*configure_controllers)(unsigned,unsigned,unsigned,uint32_t) = nullptr;
    uint32_t (*input_map)() = nullptr;
    bool (*jcart)() = nullptr;
    void (*debug_gain)(unsigned,unsigned) = nullptr;
    void (*debug_capabilities)(uint32_t*,uint64_t*) = nullptr;
    bool (*native_video_size)(unsigned*,unsigned*) = nullptr;
    bool (*input_polled)() = nullptr;
    bool (*get_cd_header)(void*, size_t) = nullptr;
    void (*invalidate_code)() = nullptr;
    int (*preserve_header_checksum)(const std::uint8_t*, size_t, unsigned) = nullptr;
    using PapriumMusicCallback = std::uint64_t (*)(void*, unsigned, std::uint64_t,
                                                    std::int16_t*, std::size_t);
    void (*set_paprium_music)(PapriumMusicCallback, void*) = nullptr;
    int (*paprium_active)() = nullptr;
    int (*paprium_cd_active)() = nullptr;
};
