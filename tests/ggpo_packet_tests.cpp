#include "ggpo_types.h"
#include "network/udp_proto.h"
#include "bitvector.h"
#include "kega_ggpo_transport.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

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

int main() {
    static_assert(GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8 <
                  (1 << BITVECTOR_NIBBLE_SIZE), "button index encoding");
    static_assert(GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS * 8 *
                      (BITVECTOR_NIBBLE_SIZE + 2) + 1 <= MAX_COMPRESSED_BITS,
                  "one full 32-player frame must fit");
    static_assert(sizeof(UdpMsg) <= 8192, "transport datagram limit");

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