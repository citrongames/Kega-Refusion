#include "ggpo_types.h"
#include "network/udp_proto.h"
#include "backends/p2p.h"
#include "bitvector.h"
#include "kega_ggpo_transport.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>
#include <deque>
#include <memory>

struct Capture {
    std::vector<uint8_t> packet;
    static void send(void *user, uint32_t, uint16_t, const void *data, int size) {
        auto *self = static_cast<Capture *>(user);
        self->packet.assign(static_cast<const uint8_t *>(data),
                            static_cast<const uint8_t *>(data) + size);
    }
    static int receive(void *, uint32_t *, uint16_t *, void *, int) { return 0; }
};

struct Sink : Udp::Callbacks {
    void OnMsg(sockaddr_in &, UdpMsg *, int) override {}
};

class TestProtocol : public UdpProtocol {
public:
    void enqueue(const GameInput &input) { _pending_output.push(input); }
    void sendPending() { SendPendingOutput(); }
    bool decodeInput(UdpMsg *message, int size) { return OnInput(message, size); }
    int lastReceivedFrame() const { return _last_received_input.frame; }
    void setRemoteMagic(uint16_t value) { _remote_magic_number = value; }
    void qualityReply() {
        UdpMsg message(UdpMsg::QualityReply);
        message.u.quality_reply.pong = GGPOPlatform::GetCurrentTimeMS();
        assert(OnQualityReply(&message, message.PacketSize()));
    }
};

static int completeFrames(const UdpMsg *message) {
    int offset = 0;
    int count = 0;
    const int total = message->u.input.num_bits;
    auto *bits = const_cast<uint8_t *>(message->u.input.bits);
    while (offset < total) {
        for (;;) {
            assert(offset < total);
            if (!BitVector_ReadBit(bits, &offset))
                break;
            assert(offset + 1 + BITVECTOR_NIBBLE_SIZE <= total);
            BitVector_ReadBit(bits, &offset);
            const int button = BitVector_ReadNibblet(bits, &offset);
            assert(button < message->u.input.input_size * 8);
        }
        ++count;
    }
    assert(offset == total);
    return count;
}


class DelayBackend : public Peer2PeerBackend {
public:
    explicit DelayBackend(GGPOSessionCallbacks* callbacks)
        : Peer2PeerBackend(callbacks,"delay-test",0,2,1,nullptr,0) {}
    bool ready() const { return !_synchronizing; }
    bool confirmed(int queue,int frame,GameInput* input) { return _sync.GetConfirmedInput(queue,frame,input); }
    void acknowledge() {
        for(int i=0;i<_num_players;++i)if(_endpoints[i].IsInitialized())_endpoints[i].SendInputAck();
    }
};
static DelayBackend* activeDelayBackend;
static bool delayBegin(const char*) { return true; }
static bool delaySave(unsigned char** data,int* length,int* checksum,int) {
    *data=new unsigned char[1]{};*length=1;*checksum=0;return true;
}
static bool delayLoad(unsigned char*,int) {return true;}
static void delayFree(void* data) {delete[] static_cast<unsigned char*>(data);}
static bool delayLog(char*,unsigned char*,int) {return true;}
static bool delayEvent(GGPOEvent*) {return true;}
static bool delayAdvance(int) {
    char inputs[2]{};int disconnected=0;
    assert(activeDelayBackend->SyncInput(inputs,sizeof(inputs),&disconnected)==GGPO_OK);
    return activeDelayBackend->IncrementFrame()==GGPO_OK;
}
struct DelayWire {
    struct Packet {uint32_t source;std::vector<uint8_t> bytes;};
    std::deque<Packet> inbox[2];
    struct End {DelayWire* wire;int side;} endpoint[2]{{this,0},{this,1}};
    static void send(void* opaque,uint32_t,uint16_t,const void* data,int length) {
        auto& end=*static_cast<End*>(opaque);
        auto& queue=end.wire->inbox[1-end.side];assert(queue.size()<128);
        const auto* bytes=static_cast<const uint8_t*>(data);
        queue.push_back({uint32_t(0x0a010001u+end.side),std::vector<uint8_t>(bytes,bytes+length)});
    }
    static int receive(void* opaque,uint32_t* source,uint16_t* port,void* data,int capacity) {
        auto& end=*static_cast<End*>(opaque);auto& queue=end.wire->inbox[end.side];
        if(queue.empty())return 0;
        auto packet=std::move(queue.front());queue.pop_front();
        assert(packet.bytes.size()<=size_t(capacity));
        *source=packet.source;*port=1;std::memcpy(data,packet.bytes.data(),packet.bytes.size());
        return int(packet.bytes.size());
    }
};
static void delayTransmission(int localQueue,int initialDelay) {
    DelayWire wire;
    GGPOSessionCallbacks callbacks{};
    callbacks.begin_game=delayBegin;callbacks.save_game_state=delaySave;
    callbacks.load_game_state=delayLoad;callbacks.free_buffer=delayFree;
    callbacks.log_game_state=delayLog;callbacks.on_event=delayEvent;callbacks.advance_frame=delayAdvance;
    std::unique_ptr<DelayBackend> backend[2];
    for(int side=0;side<2;++side) {
        KegaGgpoTransport hooks{&wire.endpoint[side],DelayWire::send,DelayWire::receive};
        kega_ggpo_set_transport(&hooks);
        backend[side]=std::make_unique<DelayBackend>(&callbacks);
        for(int queue=0;queue<2;++queue) {
            GGPOPlayer player{};player.size=sizeof(player);player.player_num=queue+1;
            player.type=queue==(side?1-localQueue:localQueue)?GGPO_PLAYERTYPE_LOCAL:GGPO_PLAYERTYPE_REMOTE;
            if(player.type==GGPO_PLAYERTYPE_REMOTE) {
                std::strcpy(player.u.remote.ip_address,side?"10.1.0.1":"10.1.0.2");
                player.u.remote.port=1;
            }
            GGPOPlayerHandle handle=-1;
            assert(backend[side]->AddPlayer(&player,&handle)==GGPO_OK && handle==queue+1);
        }
    }
    kega_ggpo_set_transport(nullptr);
    const auto pump=[&] {
        for(int round=0;round<16;++round)for(auto& b:backend) {
            activeDelayBackend=b.get();assert(b->DoPoll(0)==GGPO_OK);
        }
    };
    pump();assert(backend[0]->ready() && backend[1]->ready());
    assert(backend[0]->SetFrameDelay(localQueue+1,initialDelay)==GGPO_OK);
    for(int frame=0;frame<12;++frame) {
        if(frame==4) {
            struct GGPONetworkStats stats{};
            assert(backend[0]->GetNetworkStats(&stats,2-localQueue)==GGPO_OK);
            assert(stats.network.send_queue_len==1); // ACK retains the newest frame as the next delta base.
            assert(backend[0]->SetFrameDelay(localQueue+1,initialDelay+1)==GGPO_OK);
        }
        if(frame==8)assert(backend[0]->SetFrameDelay(localQueue+1,initialDelay)==GGPO_OK);
        for(int side=0;side<2;++side) {
            const int queue=side?1-localQueue:localQueue;
            char value=char((side?0x40:0x10)+(frame==8?7:frame));
            activeDelayBackend=backend[side].get();
            assert(backend[side]->AddLocalInput(queue+1,&value,1)==GGPO_OK);
        }
        pump();
        for(auto& b:backend)b->acknowledge();
        pump();
        for(auto& b:backend) {
            activeDelayBackend=b.get();char inputs[2]{};int disconnected=0;
            assert(b->SyncInput(inputs,sizeof(inputs),&disconnected)==GGPO_OK && !disconnected);
            assert(b->IncrementFrame()==GGPO_OK);
        }
        pump();
    }
    for(int frame=0;frame<12+initialDelay;++frame) {
        GameInput sent,received;
        assert(backend[0]->confirmed(localQueue,frame,&sent));
        assert(backend[1]->confirmed(localQueue,frame,&received));
        assert(sent.equal(received));
        if(frame<initialDelay)assert(received.bits[0]==0); // Startup uses zero padding.
        if(frame==4+initialDelay)assert(received.bits[0]==0x13); // Grow duplicates previous input.
        if(frame==9+initialDelay)assert(received.bits[0]==0x19); // Shrink drops capture, never a wire frame.
    }
}

int main() {
    static_assert(GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8 <
                  (1 << BITVECTOR_NIBBLE_SIZE), "button index encoding");
    static_assert(GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8 *
                      (BITVECTOR_NIBBLE_SIZE + 2) + 1 <= MAX_COMPRESSED_BITS,
                  "one full 32-player frame must fit");
    static_assert(sizeof(UdpMsg) <= 8192, "transport datagram limit");

    delayTransmission(0,0);delayTransmission(1,0);
    delayTransmission(0,2);delayTransmission(1,2);

    Capture capture;
    KegaGgpoTransport hooks{&capture, &Capture::send, &Capture::receive};
    kega_ggpo_set_transport(&hooks);
    Poll poll;
    Sink sink;
    Udp udp;
    udp.Init(1, &poll, &sink); // Custom mode must not bind a native socket.
    UdpMsg::connect_status status[UDP_MSG_MAX_PLAYERS]{};
    TestProtocol protocol;
    char address[] = "10.1.0.2";
    protocol.Init(&udp, poll, 0, address, 1, status);

    // Before any reply, RTT is initialized but explicitly invalid. Polling
    // never produces a new measurement, even when consecutive RTT values match.
    struct GGPONetworkStats quality{};
    protocol.GetNetworkStats(&quality);
    assert(quality.network.ping == 0 && quality.network.kbps_sent == 0);
    assert(quality.network.quality_samples == 0 && quality.network.quality_age_ms == -1);
    protocol.qualityReply();
    protocol.GetNetworkStats(&quality);
    assert(quality.network.quality_samples == 1 && quality.network.quality_age_ms >= 0);
    protocol.GetNetworkStats(&quality);
    assert(quality.network.quality_samples == 1);
    protocol.qualityReply();
    protocol.GetNetworkStats(&quality);
    assert(quality.network.quality_samples == 2 && quality.network.quality_age_ms >= 0);
    assert(capture.packet.empty()); // Statistics add no outgoing probes.

    // Two maximum-size all-bit-flip frames cannot fit one packet. The sender
    // must stop after the first complete frame and leave the rest pending.
    uint8_t on[GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS];
    uint8_t off[sizeof(on)]{};
    std::memset(on, 0xff, sizeof(on));
    GameInput first, second;
    first.init(0, reinterpret_cast<char *>(on), sizeof(on));
    second.init(1, reinterpret_cast<char *>(off), sizeof(off));
    protocol.enqueue(first);
    protocol.enqueue(second);
    protocol.sendPending();
    assert(!capture.packet.empty());
    auto *message = reinterpret_cast<const UdpMsg *>(capture.packet.data());
    assert(message->hdr.type == UdpMsg::Input);
    assert(message->u.input.num_bits <= MAX_COMPRESSED_BITS);
    assert(completeFrames(message) == 1);

    // Reject a decoded button outside the advertised input size.
    UdpMsg invalid(UdpMsg::Input);
    std::memset(&invalid.u, 0, sizeof(invalid.u));
    invalid.u.input.start_frame = 0;
    invalid.u.input.input_size = 7;
    int offset = 0;
    BitVector_SetBit(invalid.u.input.bits, &offset);
    BitVector_SetBit(invalid.u.input.bits, &offset);
    BitVector_WriteNibblet(invalid.u.input.bits, 100, &offset);
    BitVector_ClearBit(invalid.u.input.bits, &offset);
    invalid.u.input.num_bits = offset;
    invalid.hdr.magic = 1234;
    invalid.hdr.sequence_number = 0;
    protocol.setRemoteMagic(1234);
    protocol.OnMsg(&invalid, invalid.PacketSize());
    assert(protocol.lastReceivedFrame() == -1);
    assert(!protocol.decodeInput(&invalid, invalid.PacketSize()));
    kega_ggpo_set_transport(nullptr);
}