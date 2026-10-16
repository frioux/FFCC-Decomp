// Headless FFCC GBA client harness using Dolphin's bundled libmGBA.
//
// This loads a retail ffcc_cli.bin as a GBA multiboot image and exposes the
// four GameCube-side Joybus operations.  It is intentionally independent of
// Dolphin's GameCube emulation so malformed parent transcripts can be replayed
// deterministically.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio.h>
}

namespace
{
constexpr std::uint32_t EWRAM_BASE = 0x02000000;
constexpr std::uint32_t IWRAM_BASE = 0x03000000;
constexpr std::uint32_t G_LINK_STARTED_JP = 0x030029A4;
constexpr std::uint32_t G_RECONNECT_PENDING_JP = 0x03002994;
constexpr std::uint32_t G_WAS_CONNECTED_JP = 0x030029A0;
constexpr std::uint32_t G_SCREEN_JP = 0x03003090;
constexpr std::uint32_t G_MODE_JP = 0x03003094;
constexpr std::uint32_t G_SAVED_SCREEN_JP = 0x030032E8;
constexpr std::uint32_t G_LINK_ESTABLISHED_JP = 0x030032F0;
constexpr std::uint32_t G_SCREEN_PHASE_JP = 0x030032F4;
constexpr std::uint32_t G_MENU_HAS_INPUT_JP = 0x03002ACC;
constexpr std::uint32_t G_DATA_FLAGS_JP = 0x03002AEC;
constexpr std::uint32_t G_SCREEN_INIT_DONE_JP = 0x030030AC;
constexpr std::uint32_t G_WINDOWS_JP = 0x030030B0;
constexpr std::uint32_t S_LETTER_TOP_JP = 0x0300279C;
constexpr std::uint32_t S_LETTER_ROW_JP = 0x0300279D;
constexpr std::uint32_t S_LETTER_SEL_JP = 0x030027A0;
constexpr std::uint32_t S_LETTER_STATE_INIT_WESTERN = 0x030027DA;
constexpr std::uint32_t S_LETTER_TOP_WESTERN = 0x030027DC;
constexpr std::uint32_t S_LETTER_ROW_WESTERN = 0x030027DD;
constexpr std::uint32_t S_LETTER_SEL_WESTERN = 0x030027E0;
constexpr std::uint32_t S_LINK_CONNECTED_JP = 0x03000018;
constexpr std::uint32_t S_TX_QUEUE_JP = 0x03000038;
constexpr std::uint32_t S_TX_COUNT_JP = 0x03000138;
constexpr std::uint32_t S_LINK_CONNECTED_WESTERN = 0x03000070;
constexpr std::uint32_t S_TX_QUEUE_WESTERN = 0x03000090;
constexpr std::uint32_t S_TX_COUNT_WESTERN = 0x03000190;
constexpr std::uint32_t LINK_WRITE_JP = 0x020014C0;
constexpr std::uint32_t LINK_WRITE_EN = 0x02002698;
constexpr std::uint32_t LINK_TX_POP_EN = 0x02002714;
constexpr std::uint32_t LINK_SEND_CMAKE_NAME_JP = 0x02001FAC;
constexpr std::uint32_t LINK_SEND_LETTER_REPLY_JP = 0x02001E30;
constexpr std::uint32_t LINK_SEND_CMAKE_NAME_WESTERN = 0x02003190;
constexpr std::uint32_t LINK_SEND_LETTER_REPLY_WESTERN = 0x02003014;
constexpr std::uint32_t LINK_SEND_EVENT_JP = 0x02001E00;
constexpr std::uint32_t LINK_SEND_ITEM_OP_JP = 0x02001F38;
constexpr std::uint32_t LINK_SEND_EQUIP_SLOT_JP = 0x020020F8;
constexpr std::uint32_t LINK_SEND_CMD_SLOT_JP = 0x02002128;
constexpr std::uint32_t RADAR_DRAW_MAP_JP = 0x02003F94;
constexpr std::uint32_t RADAR_DRAW_MAP_EN = 0x0200197C;
constexpr std::uint32_t TEXT_INIT_JP = 0x02003478;
constexpr std::uint32_t TEXT_SET_FILL_JP = 0x020034D4;
constexpr std::uint32_t TEXT_CLEAR_JP = 0x02003534;
constexpr std::uint32_t TEXT_PRINT_JP = 0x020035A4;
constexpr std::uint32_t TEXT_SET_X_JP = 0x02003AF8;
constexpr std::uint32_t SMITH_MODE_UPDATE_JP = 0x02005858;
constexpr std::uint32_t SMITH_TOP_SETUP_JP = 0x0201722C;
constexpr std::uint32_t SMITH_IS_RECIPE_KNOWN_JP = 0x020190A8;
constexpr std::uint32_t SMITH_FORGE_SETUP_JP = 0x020178F0;
constexpr std::uint32_t SMITH_FORGE_PRINT_ROW_JP = 0x02017FAC;
constexpr std::uint32_t LINK_RESET_JP = 0x020010E4;
constexpr std::uint32_t S_BASE_POS_JP = 0x030008C4;
constexpr std::uint32_t S_MAP_HEADER_JP = 0x03001174;
constexpr std::uint32_t S_MAP_INFO_JP = 0x03001178;
constexpr std::uint32_t S_MAP_ROW_LEN_JP = 0x0300117C;
constexpr std::uint32_t S_MAP_DATA_JP = 0x03001180;
constexpr std::uint32_t S_BASE_POS_EN = 0x03002754;
constexpr std::uint32_t S_MAP_HEADER_EN = 0x03000054;
constexpr std::uint32_t S_MAP_INFO_EN = 0x03000058;
constexpr std::uint32_t S_MAP_ROW_LEN_EN = 0x0300005C;
constexpr std::uint32_t S_MAP_DATA_EN = 0x03000060;
constexpr std::uint32_t S_TX_QUEUE_EN = 0x03000090;
constexpr std::uint32_t S_TX_COUNT_EN = 0x03000190;
constexpr std::uint32_t LIST_BUF_JP = 0x02032800;
constexpr std::uint32_t LIST_BUF_EN = 0x02038800;
constexpr std::uint32_t LIST_BUF_PAL = 0x0203A800;
constexpr std::uint32_t DETAIL_BUF_JP = 0x02035800;
constexpr std::uint32_t DETAIL_BUF_EN = 0x0203B800;
constexpr std::uint32_t DETAIL_BUF_PAL = 0x0203D800;
constexpr std::uint32_t LETTER_PRINT_NEXT_LINE_JP = 0x0200EA88;
constexpr std::uint32_t LETTER_PRINT_NEXT_LINE_EN = 0x020137D4;
constexpr std::uint32_t LETTER_PRINT_NEXT_LINE_PAL = 0x0201396C;
constexpr std::uint32_t REG_JOY_TRANS = 0x04000154;
constexpr std::uint32_t REG_JOYSTAT = 0x04000158;

mLogger s_quiet_logger = {
    [](mLogger*, int, mLogLevel, const char*, va_list) {}, nullptr};

std::uint32_t ParseU32(const std::string& text)
{
    std::size_t consumed = 0;
    const auto value = std::stoull(text, &consumed, 0);
    if (consumed != text.size() || value > 0xFFFFFFFFULL)
        throw std::runtime_error("invalid 32-bit integer: " + text);
    return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> ParseHexBytes(const std::string& text)
{
    std::string digits;
    digits.reserve(text.size());
    for (const char c : text)
    {
        if (c == ' ' || c == '\t' || c == '_' || c == ':')
            continue;
        digits.push_back(c);
    }
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
        digits.erase(0, 2);
    if (digits.empty() || (digits.size() & 1) != 0)
        throw std::runtime_error("hex byte string must contain an even number of digits");

    std::vector<std::uint8_t> result;
    result.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2)
    {
        const auto byte_text = digits.substr(i, 2);
        std::size_t consumed = 0;
        const auto value = std::stoul(byte_text, &consumed, 16);
        if (consumed != 2)
            throw std::runtime_error("invalid hex byte: " + byte_text);
        result.push_back(static_cast<std::uint8_t>(value));
    }
    return result;
}

std::vector<std::uint8_t> ReadFileBytes(const std::string& path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error("could not open payload file: " + path);

    const auto end = input.tellg();
    if (end < 0)
        throw std::runtime_error("could not determine payload size: " + path);
    input.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(end));
    if (!data.empty() &&
        !input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(end)))
        throw std::runtime_error("could not read payload file: " + path);
    return data;
}

std::uint16_t Crc16(const std::vector<std::uint8_t>& data)
{
    std::uint16_t crc = 0xFFFF;
    for (const auto byte : data)
    {
        crc ^= static_cast<std::uint16_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return static_cast<std::uint16_t>(~crc);
}

std::uint32_t PackWord(std::uint8_t a, std::uint8_t b = 0, std::uint8_t c = 0,
                       std::uint8_t d = 0)
{
    return static_cast<std::uint32_t>(a) |
           (static_cast<std::uint32_t>(b) << 8) |
           (static_cast<std::uint32_t>(c) << 16) |
           (static_cast<std::uint32_t>(d) << 24);
}

std::string Hex(std::uint32_t value, int width = 8)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setfill('0') << std::setw(width) << value;
    return out.str();
}

class Harness
{
public:
    explicit Harness(const char* image_path)
    {
        mLogSetDefaultLogger(&s_quiet_logger);
        m_core = mCoreCreate(mPLATFORM_GBA);
        if (!m_core || !m_core->init(m_core))
            throw std::runtime_error("mGBA core initialization failed");

        mCoreInitConfig(m_core, "ffcc-joybus-harness");
        mCoreConfigSetIntValue(&m_core->config, "useBios", 0);
        mCoreConfigSetIntValue(&m_core->config, "skipBios", 1);
        mCoreConfigSetValue(&m_core->config, "idleOptimization", "remove");

        if (!mCoreLoadFile(m_core, image_path))
            throw std::runtime_error(std::string("could not load multiboot image: ") + image_path);

        GBASIOJOYCreate(&m_joybus);
        auto* gba = static_cast<GBA*>(m_core->board);
        GBASIOSetDriver(&gba->sio, &m_joybus, SIO_JOYBUS);

        m_video.resize(GBA_VIDEO_HORIZONTAL_PIXELS * GBA_VIDEO_VERTICAL_PIXELS);
        m_core->setVideoBuffer(m_core, m_video.data(), GBA_VIDEO_HORIZONTAL_PIXELS);
        m_core->reset(m_core);
    }

    ~Harness()
    {
        if (m_core)
        {
            mCoreConfigDeinit(&m_core->config);
            m_core->deinit(m_core);
        }
    }

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    void ResetCore()
    {
        m_core->reset(m_core);
    }

    void RunFrames(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
            m_core->runFrame(m_core);
    }

    void SetKeys(std::uint32_t keys)
    {
        m_core->setKeys(m_core, keys);
    }

    std::vector<std::uint8_t> SaveState() const
    {
        std::vector<std::uint8_t> state(m_core->stateSize(m_core));
        if (!m_core->saveState(m_core, state.data()))
            throw std::runtime_error("could not save mGBA state");
        return state;
    }

    void LoadState(const std::vector<std::uint8_t>& state)
    {
        if (!m_core->loadState(m_core, state.data()))
            throw std::runtime_error("could not restore mGBA state");
    }

    std::vector<std::uint8_t> Command(GBASIOJOYCommand command, std::uint32_t word = 0)
    {
        std::array<std::uint8_t, 6> data{};
        data[0] = static_cast<std::uint8_t>(word);
        data[1] = static_cast<std::uint8_t>(word >> 8);
        data[2] = static_cast<std::uint8_t>(word >> 16);
        data[3] = static_cast<std::uint8_t>(word >> 24);
        const int size = GBASIOJOYSendCommand(&m_joybus, command, data.data());
        RunFrames(1);
        return {data.begin(), data.begin() + size};
    }

    std::uint8_t Poll()
    {
        const auto response = Command(JOY_POLL);
        if (response.size() != 3)
            throw std::runtime_error("short JOY_POLL response");
        return response[2];
    }

    std::uint32_t Read()
    {
        const auto response = Command(JOY_TRANS);
        if (response.size() != 5)
            throw std::runtime_error("short JOY_TRANS response");
        return static_cast<std::uint32_t>(response[0]) |
               (static_cast<std::uint32_t>(response[1]) << 8) |
               (static_cast<std::uint32_t>(response[2]) << 16) |
               (static_cast<std::uint32_t>(response[3]) << 24);
    }

    void Write(std::uint32_t word)
    {
        Command(JOY_RECV, word);
    }

    void ResetJoybus()
    {
        Command(JOY_RESET);
    }

    void SendMessage(const std::vector<std::uint8_t>& data)
    {
        if (data.empty() || data.size() > 0xFFFF)
            throw std::runtime_error("message payload size must be 1..65535 bytes");

        const auto continuation_count = (data.size() - 1 + 2) / 3;
        const auto total_words = continuation_count + 2;
        if (total_words > 0xFF)
            throw std::runtime_error("message needs more than 255 Joybus words");

        const auto crc = Crc16(data);
        Write(PackWord(0x05, static_cast<std::uint8_t>(total_words),
                       static_cast<std::uint8_t>(crc), static_cast<std::uint8_t>(crc >> 8)));
        Write(PackWord(0x45, static_cast<std::uint8_t>(data.size()),
                       static_cast<std::uint8_t>(data.size() >> 8), data[0]));

        std::size_t pos = 1;
        for (std::size_t i = 0; i < continuation_count; ++i)
        {
            const auto b = pos < data.size() ? data[pos++] : 0;
            const auto c = pos < data.size() ? data[pos++] : 0;
            const auto d = pos < data.size() ? data[pos++] : 0;
            Write(PackWord(0x85, b, c, d));
        }
        std::cout << "send-message bytes=" << data.size() << " words=" << total_words
                  << " crc=" << Hex(crc, 4) << '\n';
    }

    void SendBulk(std::uint8_t type, const std::vector<std::uint8_t>& data)
    {
        constexpr std::size_t max_chunk_size = 0x2FD;
        if (type > 15)
            throw std::runtime_error("bulk type must be 0..15");
        if (data.empty() || data.size() > 0xFFFF)
            throw std::runtime_error("bulk payload size must be 1..65535 bytes");

        const auto block_count = (data.size() + max_chunk_size - 1) / max_chunk_size;
        const auto total_crc = Crc16(data);
        Write(PackWord(0x0B, type, static_cast<std::uint8_t>(block_count),
                       static_cast<std::uint8_t>(block_count >> 8)));
        Write(PackWord(0x0B, type, static_cast<std::uint8_t>(data.size()),
                       static_cast<std::uint8_t>(data.size() >> 8)));
        Write(PackWord(0x0B, type, static_cast<std::uint8_t>(total_crc),
                       static_cast<std::uint8_t>(total_crc >> 8)));

        for (std::size_t block = 0; block < block_count; ++block)
        {
            const auto begin = block * max_chunk_size;
            const auto remaining = data.size() - begin;
            const auto chunk_size = remaining < max_chunk_size ? remaining : max_chunk_size;
            const auto word_count = (chunk_size + 2) / 3;
            const std::vector<std::uint8_t> chunk(data.begin() + begin,
                                                  data.begin() + begin + chunk_size);
            const auto chunk_crc = Crc16(chunk);

            Write(PackWord(0x4B, static_cast<std::uint8_t>(word_count),
                           static_cast<std::uint8_t>(chunk_size),
                           static_cast<std::uint8_t>(chunk_size >> 8)));
            Write(PackWord(0x4B, static_cast<std::uint8_t>(block),
                           static_cast<std::uint8_t>(chunk_crc),
                           static_cast<std::uint8_t>(chunk_crc >> 8)));

            for (std::size_t pos = 0; pos < chunk_size; pos += 3)
            {
                const auto a = data[begin + pos];
                const auto b = pos + 1 < chunk_size ? data[begin + pos + 1] : 0;
                const auto c = pos + 2 < chunk_size ? data[begin + pos + 2] : 0;
                Write(PackWord(0x8B, a, b, c));
            }

            bool acknowledged = false;
            for (unsigned attempt = 0; attempt < 256; ++attempt)
            {
                if ((Poll() & 8) == 0)
                {
                    RunFrames(1);
                    continue;
                }
                const auto reply = Read();
                const auto command = static_cast<std::uint8_t>(reply) & 0x3F;
                if (command == 7)
                    throw std::runtime_error("GBA rejected bulk block " + std::to_string(block) +
                                             " with " + Hex(reply));
                if (command == 6 && static_cast<std::uint8_t>(reply >> 8) == type)
                {
                    acknowledged = true;
                    break;
                }
            }
            if (!acknowledged)
                throw std::runtime_error("no matching ACK for bulk block " + std::to_string(block));
        }

        std::cout << "send-bulk type=" << static_cast<unsigned>(type)
                  << " bytes=" << data.size() << " blocks=" << block_count
                  << " crc=" << Hex(total_crc, 4) << '\n';
    }

    void Handshake(std::uint32_t game_code = 0x4A434347, std::uint32_t host_id = 0x12345678,
                   std::uint8_t player = 0)
    {
        // Allow AgbMain to initialize LinkWork, then reproduce JoyBus::InitialCode.
        RunFrames(2);
        ResetJoybus();
        const auto idle = Read();
        Write(game_code);
        const auto context = Read();
        const auto old_host = Read();
        Write(host_id);
        Write(1U | (static_cast<std::uint32_t>(player & 0xF) << 8));

        std::cout << "handshake idle=" << Hex(idle) << " context=" << Hex(context)
                  << " old_host=" << Hex(old_host) << " status=" << Hex(Poll(), 2) << '\n';
    }

    std::uint8_t Read8(std::uint32_t address) const
    {
        return static_cast<std::uint8_t>(m_core->rawRead8(m_core, address, -1));
    }

    std::uint16_t Read16(std::uint32_t address) const
    {
        return static_cast<std::uint16_t>(m_core->rawRead16(m_core, address, -1));
    }

    std::uint32_t Read32(std::uint32_t address) const
    {
        return m_core->rawRead32(m_core, address, -1);
    }

    void Write8(std::uint32_t address, std::uint8_t value)
    {
        m_core->rawWrite8(m_core, address, -1, value);
    }

    void Write16(std::uint32_t address, std::uint16_t value)
    {
        m_core->rawWrite16(m_core, address, -1, value);
    }

    void Write32(std::uint32_t address, std::uint32_t value)
    {
        m_core->rawWrite32(m_core, address, -1, value);
    }

    void QuiesceJoybusRegisters()
    {
        // rawWrite intentionally honors the hardware's write-one/retain rules;
        // a test baseline needs to clear the emulated latch itself.
        auto* gba = static_cast<GBA*>(m_core->board);
        gba->memory.io[(REG_JOY_TRANS & 0x3FF) >> 1] = 0;
        gba->memory.io[((REG_JOY_TRANS & 0x3FF) >> 1) + 1] = 0;
        gba->memory.io[(REG_JOYSTAT & 0x3FF) >> 1] = 0;
    }

    std::uint32_t Register(const char* name) const
    {
        std::uint32_t value = 0;
        if (!m_core->readRegister(m_core, name, &value))
            throw std::runtime_error(std::string("could not read register ") + name);
        return value;
    }

    void SetRegister(const char* name, std::uint32_t value)
    {
        if (!m_core->writeRegister(m_core, name, &value))
            throw std::runtime_error(std::string("could not write register ") + name);
    }

    void CallThumb(std::uint32_t address, std::uint32_t r0, std::uint32_t r1 = 0,
                   bool allow_nonreturn = false)
    {
        constexpr std::uint32_t return_stub = EWRAM_BASE + 0x3FF00;
        Write16(return_stub, 0xE7FE); // b .
        SetRegister("r0", r0);
        SetRegister("r1", r1);
        SetRegister("lr", return_stub | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", address);
        for (int i = 0; i < 1000000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == return_stub || pc == return_stub + 2)
                return;
            m_core->step(m_core);
        }
        const std::string message = "Thumb call did not return: pc=" + Hex(Register("pc")) +
                                    " lr=" + Hex(Register("lr")) +
                                    " cpsr=" + Hex(Register("cpsr"));
        if (!allow_nonreturn)
            throw std::runtime_error(message);
        std::cout << message << '\n';
    }

    void CallThumbUntil(std::uint32_t address, std::uint32_t r0, std::uint32_t r1,
                        std::uint32_t stop, unsigned limit = 1000000)
    {
        SetRegister("r0", r0);
        SetRegister("r1", r1);
        SetRegister("lr", (EWRAM_BASE + 0x3FF00) | 1);
        Write16(EWRAM_BASE + 0x3FF00, 0xE7FE);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", address);
        for (unsigned i = 0; i < limit; ++i)
        {
            if ((Register("pc") & ~1U) == (stop & ~1U))
            {
                std::cout << "call-thumb-until reached=" << Hex(Register("pc"))
                          << " steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "call-thumb-until missed stop=" << Hex(stop)
                  << " pc=" << Hex(Register("pc")) << '\n';
    }

    void ProbeLetterOverflow(const std::string& region, std::uint32_t prefix,
                             const std::vector<std::uint8_t>& payload,
                             unsigned limit = 20000000)
    {
        if (prefix > 64 || payload.size() + prefix < 508)
            throw std::runtime_error("letter overflow needs prefix <= 64 and at least 508 bytes");

        const bool jp = region == "JP";
        const bool en = region == "EN";
        const bool pal = region == "PAL";
        if (!jp && !en && !pal)
            throw std::runtime_error("letter overflow region must be JP, EN, or PAL");

        const auto detail = jp ? DETAIL_BUF_JP : en ? DETAIL_BUF_EN : DETAIL_BUF_PAL;
        const auto function = jp ? LETTER_PRINT_NEXT_LINE_JP :
                              en ? LETTER_PRINT_NEXT_LINE_EN : LETTER_PRINT_NEXT_LINE_PAL;
        const auto epilogue = function + 0x1A2;
        const auto letter_row = jp ? S_LETTER_ROW_JP : S_LETTER_ROW_WESTERN;
        const auto tx_count = jp ? S_TX_COUNT_JP : S_TX_COUNT_WESTERN;
        constexpr std::uint32_t return_stub = EWRAM_BASE + 0x3FF00;

        for (std::uint32_t i = 0; i < prefix; ++i)
            Write8(detail + i, 0x20);
        for (std::size_t i = 0; i < payload.size(); ++i)
            Write8(detail + prefix + static_cast<std::uint32_t>(i), payload[i]);
        Write8(detail + prefix + static_cast<std::uint32_t>(payload.size()), 0);
        Write8(letter_row, 0);

        const auto byte_at = [&](std::size_t offset) -> std::uint8_t {
            if (offset < prefix)
                return 0x20;
            offset -= prefix;
            return offset < payload.size() ? payload[offset] : 0;
        };
        const auto word_at = [&](std::size_t offset) {
            return static_cast<std::uint32_t>(byte_at(offset)) |
                   (static_cast<std::uint32_t>(byte_at(offset + 1)) << 8) |
                   (static_cast<std::uint32_t>(byte_at(offset + 2)) << 16) |
                   (static_cast<std::uint32_t>(byte_at(offset + 3)) << 24);
        };
        const auto expected_pc = word_at(504);
        const auto physical = [](std::uint32_t address) {
            if ((address >> 24) == 0x02)
                return EWRAM_BASE | (address & 0x3FFFF);
            if ((address >> 24) == 0x03)
                return IWRAM_BASE | (address & 0x7FFF);
            return address;
        };

        Write16(return_stub, 0xE7FE);
        SetRegister("r0", 0);
        SetRegister("r1", 0);
        SetRegister("lr", return_stub | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", function);

        const auto start_sp = Register("sp");
        const auto count_before = Read32(tx_count);
        const auto presented_before = Read32(REG_JOY_TRANS);
        bool reached_epilogue = false;
        bool reached_expected = false;
        bool queue_changed = false;
        unsigned steps = 0;
        for (; steps < limit; ++steps)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == epilogue)
                reached_epilogue = true;
            if (physical(pc) == (physical(expected_pc) & ~1U))
                reached_expected = true;
            if (Read32(tx_count) != count_before || Read32(REG_JOY_TRANS) != presented_before)
                queue_changed = true;
            if (pc == return_stub || pc == return_stub + 2 ||
                (steps > 1000 && pc < EWRAM_BASE))
                break;
            m_core->step(m_core);
        }

        std::cout << "probe-letter-overflow region=" << region
                  << " prefix=" << prefix << " bytes=" << payload.size()
                  << " expected-pc=" << Hex(expected_pc)
                  << " physical=" << Hex(physical(expected_pc) | (expected_pc & 1))
                  << " saved-r4=" << Hex(word_at(488))
                  << " epilogue=" << reached_epilogue
                  << " pc-control=" << reached_expected
                  << " queue-change=" << queue_changed
                  << " steps=" << steps
                  << " pc=" << Hex(Register("pc"))
                  << " sp=" << Hex(Register("sp"))
                  << " initial-sp=" << Hex(start_sp)
                  << " tx-count=" << Hex(Read32(tx_count))
                  << " presented=" << Hex(Read32(REG_JOY_TRANS)) << '\n';
    }

    void StepTrace(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            std::cout << "step " << i << " pc=" << Hex(Register("pc"))
                      << " cpsr=" << Hex(Register("cpsr")) << '\n';
            m_core->step(m_core);
        }
        PrintRegisters();
    }

    void PrintState() const
    {
        std::cout << "pc=" << Hex(Register("pc")) << " lr=" << Hex(Register("lr"))
                  << " cpsr=" << Hex(Register("cpsr"))
                  << " linkStarted=" << static_cast<unsigned>(Read8(G_LINK_STARTED_JP))
                  << " linkEstablished=" << static_cast<unsigned>(Read8(G_LINK_ESTABLISHED_JP))
                  << " screen=" << static_cast<int>(static_cast<std::int8_t>(Read8(G_SCREEN_JP)))
                  << " mode=" << static_cast<unsigned>(Read8(G_MODE_JP))
                  << " savedScreen=" << static_cast<int>(static_cast<std::int8_t>(Read8(G_SAVED_SCREEN_JP)))
                  << '\n';
    }

    void PrintRegisters() const
    {
        std::cout << "registers";
        for (unsigned i = 0; i <= 12; ++i)
            std::cout << " r" << i << '=' << Hex(Register(("r" + std::to_string(i)).c_str()));
        std::cout << " sp=" << Hex(Register("sp"))
                  << " lr=" << Hex(Register("lr"))
                  << " pc=" << Hex(Register("pc"))
                  << " cpsr=" << Hex(Register("cpsr")) << '\n';
    }

    void Dump(std::uint32_t address, std::uint32_t size) const
    {
        for (std::uint32_t offset = 0; offset < size; offset += 16)
        {
            std::cout << Hex(address + offset) << ':';
            for (std::uint32_t i = 0; i < 16 && offset + i < size; ++i)
                std::cout << ' ' << Hex(Read8(address + offset + i), 2).substr(2);
            std::cout << '\n';
        }
    }

    void SaveMemory(std::uint32_t address, std::uint32_t size,
                    const std::string& path) const
    {
        std::vector<std::uint8_t> data(size);
        for (std::uint32_t i = 0; i < size; ++i)
            data[i] = Read8(address + i);
        std::ofstream output(path, std::ios::binary);
        if (!output)
            throw std::runtime_error("could not open memory dump: " + path);
        output.write(reinterpret_cast<const char*>(data.data()), data.size());
        if (!output)
            throw std::runtime_error("could not write memory dump: " + path);
        std::cout << "saved-memory address=" << Hex(address)
                  << " size=" << Hex(size) << " path=" << path << '\n';
    }

    std::vector<std::uint32_t> Drain(unsigned limit = 64)
    {
        std::vector<std::uint32_t> words;
        while (words.size() < limit && (Poll() & 8) != 0)
            words.push_back(Read());
        return words;
    }

    void SweepSmith()
    {
        // Suppress the ordinary per-frame LINK_PAD packet so a candidate's
        // writes to the link are distinguishable.  The sweep still runs the
        // real client and screen dispatcher; only the parent/link baseline is
        // made quiescent.
        Write8(S_LINK_CONNECTED_JP, 0);
        Write8(G_WAS_CONNECTED_JP, 0);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();
        const auto baseline = SaveState();
        const std::array<std::uint16_t, 8> key_sets = {
            0, 1, 2, 4, 8, 0x10, 0x40, 0x80,
        };

        unsigned transmitting_cases = 0;
        unsigned noncode_cases = 0;
        for (int phase = 0; phase <= 2; ++phase)
        {
            for (int screen = -128; screen <= 127; ++screen)
            {
                for (const auto keys : key_sets)
                {
                    LoadState(baseline);
                    Write32(G_MODE_JP, 3);
                    Write32(G_SCREEN_JP, static_cast<std::uint32_t>(screen));
                    Write32(G_SCREEN_PHASE_JP, static_cast<std::uint32_t>(phase));
                    Write8(G_LINK_STARTED_JP, 1);
                    Write8(G_LINK_ESTABLISHED_JP, 0);
                    SetKeys(keys);
                    RunFrames(1);

                    const auto pc = Register("pc");
                    if (!((pc >= EWRAM_BASE && pc < EWRAM_BASE + 0x40000) ||
                          (pc >= IWRAM_BASE && pc < IWRAM_BASE + 0x8000) || pc < 0x4000))
                    {
                        ++noncode_cases;
                    }

                    const auto joy_status = Read16(REG_JOYSTAT);
                    const auto tx_count = Read32(S_TX_COUNT_JP);
                    const auto tx_word = Read32(REG_JOY_TRANS);
                    if ((joy_status & 8) != 0 || tx_count != 0)
                    {
                        ++transmitting_cases;
                        std::cout << "tx smith screen=" << screen << " phase=" << phase
                                  << " keys=" << Hex(keys, 4) << " pc=" << Hex(pc)
                                  << " joy_status=" << Hex(joy_status, 4)
                                  << " presented=" << Hex(tx_word)
                                  << " queued=" << tx_count;
                        for (std::uint32_t i = 0; i < tx_count && i < 8; ++i)
                            std::cout << ' ' << Hex(Read32(S_TX_QUEUE_JP + i * 4));
                        std::cout << '\n';
                    }
                }
            }
        }
        std::cout << "sweep-smith transmitting_cases=" << transmitting_cases
                  << " noncode_pc_cases=" << noncode_cases << " total=6144\n";
        LoadState(baseline);
        SetKeys(0);
    }

    void ProbeQueueReset()
    {
        // Model the vulnerable ordering without requiring a frame-perfect GC.
        // "A" + the JP name-entry music-note character is 41 81 F4.  If the
        // header is lost, the first continuation makes ChkCMakeName store F4
        // in m_playerSlot (signed -12).
        constexpr std::uint32_t name = EWRAM_BASE + 0x3FE00;
        Write8(S_LINK_CONNECTED_JP, 0);
        Write8(G_WAS_CONNECTED_JP, 0);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();
        for (int i = 0; i < 16; ++i)
            Write8(name + i, 0);
        Write8(name + 0, 0x41);
        Write8(name + 1, 0x81);
        Write8(name + 2, 0xF4);
        CallThumb(LINK_SEND_CMAKE_NAME_JP, name);

        std::cout << "before-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP);
        for (std::uint32_t i = 0; i < Read32(S_TX_COUNT_JP) && i < 8; ++i)
            std::cout << ' ' << Hex(Read32(S_TX_QUEUE_JP + i * 4));
        std::cout << '\n';

        ResetJoybus();
        std::cout << "after-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';

        const auto idle = Read();
        Write(0x4A434347);
        const auto context = Read();
        const auto old_host = Read();
        Write(0x12345678);
        Write(1);

        std::cout << "reconnect idle=" << Hex(idle) << " context=" << Hex(context)
                  << " old_host=" << Hex(old_host) << " words=";
        for (int i = 0; i < 7; ++i)
            std::cout << (i == 0 ? "" : ",") << Hex(Read());
        std::cout << " queued=" << Read32(S_TX_COUNT_JP) << '\n';
    }

    void ProbeNameUnderpaint()
    {
        // Submit the maximum-length JP name
        //     ああああああた
        // (82 A0 repeated six times, then 82 BD).  In normal play this can be
        // rejected as a duplicate name.  Draining the six-word name message
        // leaves its last continuation in sTxQueue[0].  The subsequent
        // character-creation cancel is a single event word, so it is
        // presented directly and must not replace that backing word.
        constexpr std::uint32_t name = EWRAM_BASE + 0x3FE00;
        Write8(S_LINK_CONNECTED_JP, 1);
        Write8(G_WAS_CONNECTED_JP, 1);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();
        for (int i = 0; i < 16; ++i)
            Write8(name + i, 0);
        for (int i = 0; i < 6; ++i)
        {
            Write8(name + i * 2, 0x82);
            Write8(name + i * 2 + 1, 0xA0);
        }
        Write8(name + 12, 0x82);
        Write8(name + 13, 0xBD);
        CallThumb(LINK_SEND_CMAKE_NAME_JP, name);

        const auto words = Drain(8);
        std::cout << "name-underpaint sent=";
        for (std::size_t i = 0; i < words.size(); ++i)
            std::cout << (i == 0 ? "" : ",") << Hex(words[i]);
        std::cout << " queue0=" << Hex(Read32(S_TX_QUEUE_JP))
                  << " count=" << Read32(S_TX_COUNT_JP) << '\n';

        CallThumb(LINK_SEND_EVENT_JP, 4, 0); // EVT_CMAKE_CANCEL
        std::cout << "name-underpaint after-cancel presented="
                  << Hex(Read32(REG_JOY_TRANS))
                  << " queue0=" << Hex(Read32(S_TX_QUEUE_JP))
                  << " count=" << Read32(S_TX_COUNT_JP) << '\n';
    }

    void ProbeLetterReplyReset()
    {
        // A letter reply is header + middle + final.  Discard the header with
        // one Joybus reset, let the parent consume only the middle word, then
        // reset again before it consumes the final word.  The GC receiver
        // appends that middle word to its persistent receive buffer but never
        // reaches the final-word CRC check.
        Write8(S_LINK_CONNECTED_JP, 0);
        Write8(G_WAS_CONNECTED_JP, 0);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();

        SetRegister("r2", 0);          // item attachment, not gil
        SetRegister("r3", 0x12345678); // attachment value
        CallThumb(LINK_SEND_LETTER_REPLY_JP, 7, 2);

        std::cout << "reply-before-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP);
        for (std::uint32_t i = 0; i < Read32(S_TX_COUNT_JP) && i < 8; ++i)
            std::cout << ' ' << Hex(Read32(S_TX_QUEUE_JP + i * 4));
        std::cout << '\n';

        ResetJoybus();
        const auto idle = Read();
        Write(0x4A434347);
        const auto context = Read();
        const auto old_host = Read();
        Write(0x12345678);
        Write(1);

        const auto stale_host = Read();
        const auto middle = Read();
        std::cout << "reply-middle-only idle=" << Hex(idle)
                  << " context=" << Hex(context) << " old_host=" << Hex(old_host)
                  << " stale_host=" << Hex(stale_host) << " middle=" << Hex(middle)
                  << " final-presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';

        ResetJoybus();
        std::cout << "reply-after-second-reset presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';
    }

    void ProbeWesternWarpPackets()
    {
        // Emit the exact retail EN/PAL GBA words needed by the restricted
        // recv0/G59 route.  BC itself is not keyboard-selectable.  Start at
        // legal BBF3, advance the shared GC counter 70 times with zero name
        // continuations, then use the normal legal length and reply packets.
        constexpr std::uint32_t name = EWRAM_BASE + 0x3FE00;

        auto clear_tx = [&]() {
            Write8(S_LINK_CONNECTED_WESTERN, 0);
            Write32(S_TX_COUNT_WESTERN, 0);
            QuiesceJoybusRegisters();
        };
        auto print_tx = [&](const char* label) {
            std::cout << label << " presented=" << Hex(Read32(REG_JOY_TRANS))
                      << " queued=" << Read32(S_TX_COUNT_WESTERN);
            for (std::uint32_t i = 0; i < Read32(S_TX_COUNT_WESTERN) && i < 8; ++i)
                std::cout << ' ' << Hex(Read32(S_TX_QUEUE_WESTERN + i * 4));
            std::cout << '\n';
        };
        auto send_name = [&](const char* label, const std::vector<std::uint8_t>& bytes) {
            clear_tx();
            for (int i = 0; i < 16; ++i)
                Write8(name + i, 0);
            for (std::size_t i = 0; i < bytes.size() && i < 16; ++i)
                Write8(name + static_cast<std::uint32_t>(i), bytes[i]);
            CallThumb(LINK_SEND_CMAKE_NAME_WESTERN, name);
            print_tx(label);
        };

        // Legal UI name.  After its header is stripped, continuations one
        // and two install channel-1 packet count C041, which targets recv3.
        send_name("western-counter-recv3-legal",
                  {0x41, 0x41, 0x41, 0xC0, 0x41});

        // Fully legal reset-optimal predecessor to recv0's required BC39
        // count. BB and F3 are both present on the western keyboard.
        send_name("western-counter-recv0-walk-start",
                  {0x41, 0x41, 0x41, 0xBB, 0xF3});

        // A one-character name has five all-zero continuations.  Fourteen
        // full submissions move BBF3 -> BC39 while clearing only CMes
        // temporaries and Joybus.m_pathBuf.
        send_name("western-counter-walk-zero-name", {0x41});

        // First continuation is all zero and supplies the high length byte.
        send_name("western-length-high-zero", {0x41});

        // Ac<0xC0>\" supplies 63 C0 22.  With the observed EN allocation this
        // makes recv0 length 0063C022, exactly (G59-2)-recv0.payload.
        send_name("western-length-low-legal", {0x41, 0x63, 0xC0, 0x22});

        clear_tx();
        SetRegister("r2", 1); // gil attachment
        SetRegister("r3", 14);
        CallThumb(LINK_SEND_LETTER_REPLY_WESTERN, 7, 0);
        print_tx("western-14-gil-reply");

        // Cleanup-safe western Smith-to-Radar bridge.  Port 0 can install
        // C02D in channel 3 while leaving that record inactive.  Channel 3's
        // first continuation then writes BF2C into recv3.length[2..3].
        send_name("western-clean-counter-c02d", {0x41, 0xC0, 0x2D});
        send_name("western-recv3-to-channel1-count", {0x41, 0xBF, 0x2C});

        // With the BF2C receive offset, the two continuation words place
        // DE36 in channel 1's CMake counter.  DE36 addresses
        // Joybus.m_fileBaseB+1 in every retail region.
        clear_tx();
        SetRegister("r2", 1);
        SetRegister("r3", 56886);
        CallThumb(LINK_SEND_LETTER_REPLY_WESTERN, 7, 0);
        print_tx("western-56886-gil-counter-reply");

        // Observed Tipa letterBuffer[1] + 0x37 = 816FCE77.  Once DE36 is
        // installed, this first continuation replaces fileBaseB's low three
        // bytes with 6F CE 77 while preserving its 0x81 high byte.
        send_name("western-fileb-smith-source-observed", {0x41, 0x6F, 0xCE, 0x77});
    }

    void ProbeSmithLetterIndex(std::uint8_t target)
    {
        // Model the state produced by carrying a fully drawn letter-list screen
        // into JP smith screen 9, after another LIST_BUF producer has replaced
        // the old list.  Screen 9's init slot aliases LetterList_Main.
        for (std::uint32_t address = LIST_BUF_JP; address < LIST_BUF_JP + 0x3000; address += 4)
            Write32(address, 0);

        // A smith list with one recipe stored in inventory slot 1 starts with
        // bytes 01 01 00 00.  Letter code misreads the whole word as a large
        // positive count, while its row pointers still fold through mirrored
        // EWRAM.  Fill the record with the exact stable item-534 (Firaga +2)
        // fields at a 90% tailor: price 0, its six attack-row halfwords, and
        // four zero smith results.  This tests the practical wrong-craft
        // payload rather than a header followed by fabricated zero records.
        Write32(LIST_BUF_JP, 0x00000101);
        const std::uint16_t firaga_record[] = {
            0, 0, 500, 30, 0xFFFF, 8, 40, 30, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        for (std::size_t i = 0; i < std::size(firaga_record); ++i)
            Write16(LIST_BUF_JP + 4 + static_cast<std::uint32_t>(i * 2), firaga_record[i]);

        Write32(G_MODE_JP, 3);             // MODE_SMITH
        Write32(G_SCREEN_JP, 9);
        Write32(G_SCREEN_PHASE_JP, 0);     // call screen-9 init alias
        Write32(G_SCREEN_INIT_DONE_JP, 1);
        Write8(G_MENU_HAS_INPUT_JP, 1);
        Write8(G_LINK_STARTED_JP, 1);
        Write8(G_WAS_CONNECTED_JP, 1);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write8(G_LINK_ESTABLISHED_JP, 0);
        Write16(G_DATA_FLAGS_JP, Read16(G_DATA_FLAGS_JP) | 8); // stale letter-list ready

        // Preserve the already-open letter window.  Keeping sLetterRow at 8
        // avoids redrawing the initial rows before input is accepted.
        Write8(G_WINDOWS_JP + 0, 1);       // active
        Write8(G_WINDOWS_JP + 2, 0);       // cursor
        Write8(G_WINDOWS_JP + 5, 2);       // bg
        Write16(G_WINDOWS_JP + 0x0E, 8);   // rows
        Write16(G_WINDOWS_JP + 0x10, 0);   // x
        Write16(G_WINDOWS_JP + 0x12, 0);   // y
        Write16(G_WINDOWS_JP + 0x14, 30);  // width
        Write16(G_WINDOWS_JP + 0x16, 18);  // height
        Write8(S_LETTER_TOP_JP, 0);
        Write8(S_LETTER_ROW_JP, 8);
        Write16(S_LETTER_SEL_JP, 0xFFFF);

        auto pulse = [this](std::uint32_t keys) {
            SetKeys(keys);
            Command(JOY_TRANS);
            SetKeys(0);
            Command(JOY_TRANS);
        };

        for (unsigned i = 0; i < target; ++i)
            pulse(0x80); // DPAD_DOWN
        pulse(0x01);     // A_BUTTON

        std::cout << "probe-smith-letter-index target=" << static_cast<unsigned>(target)
                  << " top=" << static_cast<int>(static_cast<std::int8_t>(Read8(S_LETTER_TOP_JP)))
                  << " cursor=" << static_cast<unsigned>(Read8(G_WINDOWS_JP + 2))
                  << " selected=" << Hex(Read16(S_LETTER_SEL_JP), 4) << '\n';
        const auto words = Drain();
        std::cout << "probe-smith-letter-index queued:";
        for (const auto word : words)
            std::cout << ' ' << Hex(word);
        std::cout << '\n';
    }

    void ProbeWesternSmithLetterIndex(std::uint8_t target, bool pal)
    {
        const std::uint32_t list_buf = pal ? LIST_BUF_PAL : LIST_BUF_EN;
        for (std::uint32_t address = list_buf; address < list_buf + 0x3000; address += 4)
            Write32(address, 0);

        // The smallest useful retail smith list: item 534 (Firaga +2) in
        // inventory slot 1.  PAL/EN screen 9 is the complete LetterList
        // function triple, so unlike JP it must survive LetterList_Init before
        // accepting input.  Item 534's smith results are all zero, so GC can
        // build this record without following any OOB material pointers.
        Write32(list_buf, 0x00000101);
        const std::uint16_t firaga_record[] = {
            0, 0, 500, 30, 0xFFFF, 8, 40, 30, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        };
        for (std::size_t i = 0; i < std::size(firaga_record); ++i)
            Write16(list_buf + 4 + static_cast<std::uint32_t>(i * 2), firaga_record[i]);

        Write32(G_MODE_JP, 3);
        Write32(G_SCREEN_JP, 9);
        Write32(G_SCREEN_PHASE_JP, 0);
        Write32(G_SCREEN_INIT_DONE_JP, 0);
        Write8(G_MENU_HAS_INPUT_JP, 1);
        Write8(G_LINK_STARTED_JP, 1);
        Write8(G_WAS_CONNECTED_JP, 1);
        Write8(G_RECONNECT_PENDING_JP, 0);
        Write8(G_LINK_ESTABLISHED_JP, 0);
        Write16(G_DATA_FLAGS_JP, Read16(G_DATA_FLAGS_JP) | 8);
        Write8(S_LETTER_STATE_INIT_WESTERN, 0);
        Write8(S_LETTER_TOP_WESTERN, 0);
        Write8(S_LETTER_ROW_WESTERN, 0);
        Write16(S_LETTER_SEL_WESTERN, 0xFFFF);

        // Let init draw and open the list.  Joybus transfers are sufficient to
        // advance the retail main loop one frame apiece in this harness.
        for (unsigned i = 0; i < 90; ++i)
            Command(JOY_TRANS);

        auto pulse = [this](std::uint32_t keys) {
            SetKeys(keys);
            Command(JOY_TRANS);
            SetKeys(0);
            Command(JOY_TRANS);
        };
        for (unsigned i = 0; i < target; ++i)
            pulse(0x80);
        pulse(0x01);

        std::cout << "probe-western-smith-letter-index region=" << (pal ? "PAL" : "EN")
                  << " target=" << static_cast<unsigned>(target)
                  << " phase=" << Read32(G_SCREEN_PHASE_JP)
                  << " top=" << static_cast<unsigned>(Read8(S_LETTER_TOP_WESTERN))
                  << " cursor=" << static_cast<unsigned>(Read8(G_WINDOWS_JP + 2))
                  << " selected=" << Hex(Read16(S_LETTER_SEL_WESTERN), 4) << '\n';
        const auto words = Drain();
        std::cout << "probe-western-smith-letter-index queued:";
        for (const auto word : words)
            std::cout << ' ' << Hex(word);
        std::cout << '\n';
    }

    void ProbeRadarStack(bool western = false)
    {
        // Radar_DrawMap's line[256] starts 0x250 bytes below the saved LR.
        // One 296-halfword RLE run reaches it; two following literal tokens
        // replace LR with a Thumb pointer to an EWRAM sentinel.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t normal_return = EWRAM_BASE + 0x3FF00;
        // Both return-address halfwords must have bit 15 clear or the row
        // decoder treats them as additional RLE tokens.
        constexpr std::uint32_t hijack_target = EWRAM_BASE + 0x30000;
        const std::uint32_t radar_draw_map = western ? RADAR_DRAW_MAP_EN : RADAR_DRAW_MAP_JP;
        const std::uint32_t base_pos = western ? S_BASE_POS_EN : S_BASE_POS_JP;
        const std::uint32_t map_header = western ? S_MAP_HEADER_EN : S_MAP_HEADER_JP;
        const std::uint32_t map_info = western ? S_MAP_INFO_EN : S_MAP_INFO_JP;
        const std::uint32_t map_row_len = western ? S_MAP_ROW_LEN_EN : S_MAP_ROW_LEN_JP;
        const std::uint32_t map_data = western ? S_MAP_DATA_EN : S_MAP_DATA_JP;

        for (std::uint32_t i = 0; i < 0x200; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(base_pos + 0, 0);
        Write16(base_pos + 2, 0);
        Write16(map + 0x14, 0x100); // width
        Write16(map + 0x16, 8); // one decoded row
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(map_header, map);
        Write32(map_info, info);
        Write32(map_row_len, rows);
        Write32(map_data, data);
        Write8(rows, 3);
        Write16(data + 0, 0x8128); // RLE: 296 copies of tile 0
        Write16(data + 2, static_cast<std::uint16_t>((hijack_target | 1) & 0xFFFF));
        Write16(data + 4, static_cast<std::uint16_t>((hijack_target | 1) >> 16));
        Write16(normal_return, 0xE7FE); // b .
        Write16(hijack_target, 0xE7FE); // b .

        const auto entry_sp = Register("sp");
        SetRegister("lr", normal_return | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", radar_draw_map);
        for (int i = 0; i < 200000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == hijack_target || pc == hijack_target + 2)
            {
                std::cout << "probe-radar-stack region=" << (western ? "western" : "JP")
                          << " hijacked pc=" << Hex(Register("pc"))
                          << " sp=" << Hex(Register("sp"))
                          << " entry-sp=" << Hex(entry_sp)
                          << " r0=" << Hex(Register("r0"))
                          << " r1=" << Hex(Register("r1"))
                          << " r2=" << Hex(Register("r2"))
                          << " r3=" << Hex(Register("r3"))
                          << " r4=" << Hex(Register("r4"))
                          << " r5=" << Hex(Register("r5"))
                          << " r6=" << Hex(Register("r6"))
                          << " r7=" << Hex(Register("r7"))
                          << " steps=" << i << '\n';
                return;
            }
            if (pc == normal_return || pc == normal_return + 2)
            {
                std::cout << "probe-radar-stack returned normally pc=" << Hex(Register("pc"))
                          << " steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-stack stopped pc=" << Hex(Register("pc"))
                  << " lr=" << Hex(Register("lr")) << " cpsr=" << Hex(Register("cpsr"))
                  << " entry_sp=" << Hex(entry_sp) << '\n';
        Dump(entry_sp - 0x80, 0x90);
    }

    void ProbeRadarStream(bool western, const std::vector<std::uint16_t>& tokens,
                          int map_trampoline = 0, std::uint8_t branch_slot = 0,
                          int step_limit = 200000, bool stop_at_target = false,
                          std::uint8_t first_name_byte = 0x43,
                          const std::vector<std::uint8_t>& arm_payload = {})
    {
        if (tokens.empty() || tokens.size() > 255)
            throw std::runtime_error("probe-radar-stream needs 1..255 halfword tokens");

        const std::uint32_t map = map_trampoline >= 5 ? EWRAM_BASE + 0x38000
                                                       : EWRAM_BASE + 0x2E000;
        const std::uint32_t info = map + 0x100;
        const std::uint32_t rows = info + 8;
        const std::uint32_t data = map_trampoline >= 5 ? map + 0x200 : info + 0x20;
        constexpr std::uint32_t normal_return = EWRAM_BASE + 0x3FF00;
        const std::uint32_t radar_draw_map = western ? RADAR_DRAW_MAP_EN : RADAR_DRAW_MAP_JP;
        const std::uint32_t base_pos = western ? S_BASE_POS_EN : S_BASE_POS_JP;
        const std::uint32_t map_header = western ? S_MAP_HEADER_EN : S_MAP_HEADER_JP;
        const std::uint32_t map_info = western ? S_MAP_INFO_EN : S_MAP_INFO_JP;
        const std::uint32_t map_row_len = western ? S_MAP_ROW_LEN_EN : S_MAP_ROW_LEN_JP;
        const std::uint32_t map_data = western ? S_MAP_DATA_EN : S_MAP_DATA_JP;
        const std::uint32_t tx_queue = western ? S_TX_QUEUE_WESTERN : S_TX_QUEUE_JP;
        const std::uint32_t tx_count = western ? S_TX_COUNT_WESTERN : S_TX_COUNT_JP;
        std::uint16_t saved_pc_low = 0;
        std::uint16_t saved_pc_high = 0;
        std::uint32_t output_line = 0;
        for (const auto token : tokens)
        {
            const auto count = (token & 0x8000) ? (token & 0x3FF) : 1;
            const auto value = (token & 0x8000) ? static_cast<std::uint16_t>(0x1000 | ((token >> 10) & 3)) : token;
            if (output_line <= 296 && 296 < output_line + count)
                saved_pc_low = value;
            if (output_line <= 297 && 297 < output_line + count)
                saved_pc_high = value;
            output_line += count;
        }
        const std::uint32_t expected_target = saved_pc_low | (static_cast<std::uint32_t>(saved_pc_high) << 16);

        const std::uint32_t row_data = map_trampoline == 2 ? map + 0x3000
            : map_trampoline == 5 ? data + 58
            : map_trampoline == 6 ? data + 460
            : map_trampoline >= 3 ? map + 0x1000 : data;
        const std::uint32_t clear_size = map_trampoline == 2 ? 0x4000
            : map_trampoline == 5 ? 0x400
            : map_trampoline >= 3 ? 0x2000 : 0x400;
        for (std::uint32_t i = 0; i < clear_size; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(base_pos + 0, map_trampoline >= 5 ? 81
            : map_trampoline >= 3 ? 80 : 0);
        Write16(base_pos + 2, map_trampoline == 6 ? 752
            : map_trampoline == 1 ? 64
            : map_trampoline == 2 ? 0x2040
            : map_trampoline == 3 ? 0x01C0
            : map_trampoline == 4 ? 0x4669
            : map_trampoline == 5 ? 624 : 0);
        Write16(map + 0x14, map_trampoline >= 5 ? 3925 : 0x100);
        Write16(map + 0x16, map_trampoline == 2 ? 0x47A8
            : map_trampoline >= 5 ? 214
            : map_trampoline >= 3 ? 0x1000 : 8);
        Write16(info + 0, map_trampoline == 1 ? 8096
            : map_trampoline == 2 ? 23720
            : map_trampoline == 3 ? 0
            : map_trampoline == 4 ? static_cast<std::uint16_t>(-0x4632)
            : map_trampoline >= 5 ? 1
            : static_cast<std::uint16_t>(-80));
        Write16(info + 2, map_trampoline == 1 ? 0
            : map_trampoline == 2 ? static_cast<std::uint16_t>(-10008)
            : map_trampoline == 3 ? 0
            : map_trampoline == 4 ? 0x4489
            : map_trampoline >= 5 ? 512
            : static_cast<std::uint16_t>(-64));
        Write32(map_header, map);
        Write32(map_info, info);
        Write32(map_row_len, rows);
        Write32(map_data, map_trampoline >= 5 ? data : row_data);
        if (map_trampoline >= 5)
        {
            const std::pair<unsigned, unsigned> lengths[] = {
                {1, 6}, {3, 23}, {5, 41}, {7, 57}, {9, 1}, {10, 1},
                {11, 31}, {13, 36}, {17, 2}, {18, 1}, {19, 31},
            };
            for (const auto& [row, length] : lengths)
                Write8(rows + row, static_cast<std::uint8_t>(length));
        }
        Write8(rows + (map_trampoline == 2 ? 2274
                          : map_trampoline == 3 ? 47
                          : map_trampoline == 4 ? 51
                          : map_trampoline == 5 ? 5
                          : map_trampoline == 6 ? 21 : 0),
               static_cast<std::uint8_t>(tokens.size()));
        for (std::size_t i = 0; i < tokens.size(); ++i)
            Write16(row_data + static_cast<std::uint32_t>(i * 2), tokens[i]);
        Write16(normal_return, 0xE7FE);
        Write32(tx_count, 0);
        if (map_trampoline == 1)
        {
            // At 03000068: nop; nop; b 03000090.  Queue slot 0 then
            // executes `nop; bx pc`, switching to ARM at the following word.
            Write32(tx_queue + 0, 0x47780000);
            Write32(tx_queue + 4, 0xEAFFFFFE);
        }
        else if (map_trampoline == 2)
        {
            // At 03000068: movs r0,#0; lsls r0,r0,#0;
            // add r3,pc,#0x20; bx r3.  The unavoidable zero high halfword of
            // sMapY is a real LSLS rather than a NOP, so r0 must be zeroed
            // first to keep Z set.  Execution switches directly to ARM at
            // queue slot 0, where a forged item C00A in a letter-final packet
            // is BEQ 020002EC+slot*0x400.
            Write32(tx_queue, 0x0AC00095 | (static_cast<std::uint32_t>(branch_slot) << 8));
        }
        else if (map_trampoline >= 3 && map_trampoline <= 6)
        {
            // With ty=48, row 47 supplies the stack overflow.  The resulting
            // scalar halfwords are harmless shifts and execution falls
            // through the cleared LinkWork fields into queue slot 0.
            // A seven-byte western name `ACxG?<byte>z` leaves these first two
            // continuations queued after its header is presented:
            //   5C 43 78 47  -> muls r4,r3,r4; bx pc
            //   5C ?? C0 7A  -> BVC 0200020C+byte*0x400
            for (std::uint32_t address = S_LINK_CONNECTED_WESTERN;
                 address < tx_queue; ++address)
                Write8(address, 0);
            Write32(tx_queue + 0, 0x4778005C |
                (static_cast<std::uint32_t>(first_name_byte) << 8));
            Write32(tx_queue + 4, 0x7AC0005C |
                (static_cast<std::uint32_t>(branch_slot) << 8));
        }
        if (!arm_payload.empty())
        {
            const auto payload_target = 0x0200020C +
                static_cast<std::uint32_t>(branch_slot) * 0x400;
            for (std::size_t i = 0; i < arm_payload.size(); ++i)
                Write8(payload_target + static_cast<std::uint32_t>(i), arm_payload[i]);
        }
        QuiesceJoybusRegisters();

        if (map_trampoline >= 3 && map_trampoline <= 6)
        {
            std::cout << "probe-radar-name scalar";
            for (std::uint32_t address = 0x03000064; address < 0x03000070; address += 2)
                std::cout << ' ' << Hex(Read16(address), 4);
            std::cout << '\n';
            if (map_trampoline >= 5)
            {
                std::cout << "probe-radar-western-map"
                          << " info=" << Hex(info)
                          << " x=" << Hex(Read16(info), 4)
                          << " y=" << Hex(Read16(info + 2), 4)
                          << " row5=" << Hex(Read8(rows + 5), 2)
                          << " data=" << Hex(row_data) << '\n';
            }
        }

        SetRegister("lr", normal_return | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", radar_draw_map);
        bool printed_entry = false;
        bool printed_queue_thumb = false;
        bool printed_arm_queue = false;
        bool printed_arm_target = false;
        bool printed_thumb_reentry = false;
        bool printed_western_row = false;
        bool printed_western_info = false;
        bool printed_link_write = false;
        bool seen_arm = false;
        const std::uint32_t arm_target = (map_trampoline >= 3 ? 0x0200020C : 0x020002EC) +
            static_cast<std::uint32_t>(branch_slot) * 0x400;
        for (int i = 0; i < step_limit; ++i)
        {
            if (map_trampoline >= 5 && !printed_western_info &&
                (Register("pc") & ~1U) == 0x020019D0)
            {
                std::cout << "probe-radar-western-info"
                          << " info=" << Hex(Register("r3"))
                          << " y=" << Hex(Register("r4"))
                          << " map-y=" << Hex(Register("r2"))
                          << " bytes=" << Hex(Read8(Register("r3") + 2), 2)
                          << ',' << Hex(Read8(Register("r3") + 3), 2) << '\n';
                printed_western_info = true;
            }
            if (map_trampoline >= 5 && !printed_western_row &&
                (Register("pc") & ~1U) == 0x02001AC0)
            {
                std::cout << "probe-radar-western-row"
                          << " i=" << Hex(Register("r7"))
                          << " row-address=" << Hex(Register("r0"))
                          << " row-base=" << Hex(Register("r3"))
                          << " data=" << Hex(Register("r2"))
                          << " ty=" << Hex(Read32(Register("sp") + 0x240))
                          << " map-h=" << Hex(Read32(Register("sp") + 0x244)) << '\n';
                printed_western_row = true;
            }
            const auto target_pc = expected_target & ~1U;
            const auto current_pc = Register("pc") & ~1U;
            const auto link_write = western ? LINK_WRITE_EN : LINK_WRITE_JP;
            if (!printed_link_write && current_pc >= link_write &&
                current_pc < link_write + 0x74)
            {
                std::cout << "probe-radar-stream Link_Write"
                          << " pc=" << Hex(Register("pc"))
                          << " r0=" << Hex(Register("r0"))
                          << " r1=" << Hex(Register("r1"))
                          << " r2=" << Hex(Register("r2"))
                          << " r3=" << Hex(Register("r3"))
                          << " lr=" << Hex(Register("lr"))
                          << " sp=" << Hex(Register("sp")) << '\n';
                printed_link_write = true;
            }
            if (!printed_entry && expected_target != 0 &&
                current_pc >= target_pc && current_pc <= target_pc + 4)
            {
                std::cout << "probe-radar-stream entry target=" << Hex(expected_target);
                for (int reg = 0; reg <= 12; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    std::cout << ' ' << name << '=' << Hex(Register(name.c_str()));
                }
                std::cout << " sp=" << Hex(Register("sp"));
                if (map_trampoline >= 3 && map_trampoline <= 6)
                {
                    std::cout << " scalar=";
                    for (std::uint32_t address = 0x03000064; address < 0x03000070; address += 2)
                        std::cout << (address == 0x03000064 ? "" : ",") << Hex(Read16(address), 4);
                }
                std::cout << '\n';
                printed_entry = true;
                if (stop_at_target)
                    return;
            }
            if ((map_trampoline >= 2 && map_trampoline <= 6) && !printed_arm_queue &&
                Register("pc") >= 0x03000068 && Register("pc") <= tx_queue + 4)
            {
                std::cout << "probe-radar-arm-branch trampoline pc=" << Hex(Register("pc"))
                          << " cpsr=" << Hex(Register("cpsr"));
                for (int reg = 0; reg <= 7; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    std::cout << ' ' << name << '=' << Hex(Register(name.c_str()));
                }
                std::cout << " sp=" << Hex(Register("sp"));
                if ((map_trampoline >= 3 && map_trampoline <= 6) && Register("pc") == 0x0300006A)
                {
                    std::cout << " scalar=";
                    for (std::uint32_t address = 0x03000064; address < 0x03000070; address += 2)
                        std::cout << (address == 0x03000064 ? "" : ",") << Hex(Read16(address), 4);
                }
                std::cout << '\n';
                if (Register("pc") >= tx_queue)
                    printed_arm_queue = true;
            }
            if (printed_arm_queue && (Register("cpsr") & 0x20) == 0)
                seen_arm = true;
            if ((map_trampoline >= 2 && map_trampoline <= 6) && printed_arm_queue && !printed_arm_target &&
                (Register("pc") & ~3U) == ((arm_target + 4) & ~3U))
            {
                std::cout << "probe-radar-arm-branch target=" << Hex(arm_target)
                          << " pc=" << Hex(Register("pc"))
                          << " cpsr=" << Hex(Register("cpsr"));
                for (int reg = 0; reg <= 12; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    std::cout << ' ' << name << '=' << Hex(Register(name.c_str()));
                }
                std::cout << " sp=" << Hex(Register("sp")) << '\n';
                printed_arm_target = true;
            }
            if ((map_trampoline >= 2 && map_trampoline <= 6) && seen_arm && !printed_thumb_reentry &&
                (Register("cpsr") & 0x20) != 0)
            {
                std::cout << "probe-radar-arm-branch thumb-reentry pc=" << Hex(Register("pc"))
                          << " cpsr=" << Hex(Register("cpsr"))
                          << " lr=" << Hex(Register("lr"))
                          << " steps=" << i << '\n';
                printed_thumb_reentry = true;
            }
            if (map_trampoline == 1 && Register("pc") >= tx_queue && Register("pc") < tx_queue + 12)
            {
                if (!printed_queue_thumb)
                {
                    std::cout << "probe-radar-map-trampoline thumb pc=" << Hex(Register("pc"))
                              << " cpsr=" << Hex(Register("cpsr"))
                              << " scroll-x=" << Hex(Read16(0x0300006C), 4);
                    for (int reg = 0; reg <= 7; ++reg)
                    {
                        const auto name = "r" + std::to_string(reg);
                        std::cout << ' ' << name << '=' << Hex(Register(name.c_str()));
                    }
                    std::cout << " sp=" << Hex(Register("sp")) << '\n';
                    printed_queue_thumb = true;
                }
                if ((Register("cpsr") & 0x20) == 0)
                {
                    std::cout << "probe-radar-map-trampoline arm pc=" << Hex(Register("pc"))
                              << " cpsr=" << Hex(Register("cpsr")) << '\n';
                    return;
                }
            }
            const auto count = Read32(tx_count);
            const auto presented = Read32(REG_JOY_TRANS);
            // Long retail-shaped rows may wrap through LinkWork and corrupt
            // the queue before Radar_DrawMap has reached its overwritten
            // return PC.  Do not mistake that side effect for a successful
            // sender; first prove that execution reached the decoded target.
            if (printed_entry && (count != 0 || presented != 0))
            {
                std::cout << "probe-radar-stream region=" << (western ? "western" : "JP")
                          << " emitted pc=" << Hex(Register("pc"))
                          << " expected=" << Hex(expected_target)
                          << " presented=" << Hex(presented)
                          << " count=" << Hex(count);
                const auto shown = std::min<std::uint32_t>(count, 8);
                for (std::uint32_t packet = 0; packet < shown; ++packet)
                    std::cout << ' ' << Hex(Read32(tx_queue + packet * 4));
                std::cout << " steps=" << i << '\n';
                return;
            }
            const auto pc = Register("pc") & ~1U;
            if (pc == normal_return || pc == normal_return + 2)
            {
                std::cout << "probe-radar-stream region=" << (western ? "western" : "JP")
                          << " returned normally steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-stream region=" << (western ? "western" : "JP")
                  << " stopped pc=" << Hex(Register("pc"))
                  << " expected=" << Hex(expected_target)
                  << " cpsr=" << Hex(Register("cpsr"))
                  << " lr=" << Hex(Register("lr"))
                  << " count=" << Hex(Read32(tx_count)) << '\n';
    }

    void ProbeWesternArmPayload(const std::string& label,
                                const std::vector<std::uint8_t>& payload,
                                int step_limit = 512)
    {
        constexpr std::uint32_t target = 0x0203860C;
        constexpr std::uint32_t map = 0x02038000;
        for (std::size_t i = 0; i < m_western_arm_backing.size(); ++i)
            Write8(map + static_cast<std::uint32_t>(i), m_western_arm_backing[i]);
        for (std::uint32_t i = 0; i < 0x800; ++i)
            Write8(target + i, 0);
        for (std::size_t i = 0; i < payload.size(); ++i)
            Write8(target + static_cast<std::uint32_t>(i), payload[i]);
        Write32(S_TX_COUNT_WESTERN, 0);
        QuiesceJoybusRegisters();
        SetRegister("r0", 0x00040000);
        SetRegister("r1", 0x30000010);
        SetRegister("r2", 0x040000B0);
        SetRegister("r3", 0x00001000);
        SetRegister("r4", 0);
        SetRegister("r5", 0);
        SetRegister("r6", 0x00010000);
        SetRegister("r7", 0x35012700);
        SetRegister("r8", 0);
        SetRegister("r9", 0);
        SetRegister("r10", 0);
        SetRegister("r11", 0);
        SetRegister("r12", 0x040000B0);
        SetRegister("sp", 0x03007F00);
        SetRegister("lr", 0x0203FF01);
        SetRegister("cpsr", 0x4000001F); // ARM, system mode, Z=1
        SetRegister("pc", target);
        for (int i = 0; i < step_limit; ++i)
        {
            const auto pc = Register("pc");
            const auto presented = Read32(REG_JOY_TRANS);
            const auto count = Read32(S_TX_COUNT_WESTERN);
            if (presented != 0 || count != 0 ||
                (pc >= LINK_WRITE_EN && pc < LINK_WRITE_EN + 0x74))
            {
                std::cout << "probe-western-arm-payload hit label=" << label
                          << " pc=" << Hex(pc)
                          << " presented=" << Hex(presented)
                          << " count=" << Hex(count)
                          << " r0=" << Hex(Register("r0"))
                          << " steps=" << i << '\n';
                return;
            }
            if (pc == target + (payload.size() & ~std::size_t(3)))
            {
                std::cout << "probe-western-arm-payload end label=" << label
                          << " pc=" << Hex(pc)
                          << " cpsr=" << Hex(Register("cpsr"));
                for (int reg = 0; reg <= 12; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    std::cout << ' ' << name << '=' << Hex(Register(name.c_str()));
                }
                std::cout << " steps=" << i << '\n';
                return;
            }
            if (!((pc >= EWRAM_BASE && pc < EWRAM_BASE + 0x40000) ||
                  (pc >= IWRAM_BASE && pc < IWRAM_BASE + 0x8000) ||
                  (pc >= 0x02000000 && pc < 0x02040000)))
                break;
            m_core->step(m_core);
        }
    }

    void SetWesternArmBacking(std::vector<std::uint8_t> backing)
    {
        m_western_arm_backing = std::move(backing);
        std::cout << "western-arm-backing bytes=" << m_western_arm_backing.size() << '\n';
    }

    void SweepRadarNameArm()
    {
        // This is the natural western Smith row used by
        // probe-radar-name-arm-byte.  Sweep the one controllable name byte
        // with a fresh core each time so crashes cannot contaminate the next
        // candidate.
        const std::vector<std::uint16_t> row = {
            0x0000, 0x0232, 0x0000, 0x0218, 0x0000, 0x0000, 0x0218,
            0x0000, 0x0009, 0x0000, 0x0000, 0x0000, 0x0238, 0x0000,
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            0x0000, 0x0000, 0x0000, 0x0000, 0xA100, 0x4120, 0x6465,
            0x0000, 0x9003, 0x0000, 0x0000, 0x0000, 0x0300, 0x0000,
            0x7703, 0x0000, 0x5803, 0x0000, 0x0069, 0x0300,
        };
        for (unsigned slot = 0; slot <= 0xFF; ++slot)
        {
            ResetCore();
            std::cout << "probe-radar-name-arm-sweep slot=" << Hex(slot, 2) << '\n';
            ProbeRadarStream(true, row, 3, static_cast<std::uint8_t>(slot), 30000);
        }
    }

    void SweepRadarNameArmFirstByte(std::uint8_t slot)
    {
        const std::vector<std::uint16_t> row = {
            0x0000, 0x0232, 0x0000, 0x0218, 0x0000, 0x0000, 0x0218,
            0x0000, 0x0009, 0x0000, 0x0000, 0x0000, 0x0238, 0x0000,
            0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
            0x0000, 0x0000, 0x0000, 0x0000, 0xA100, 0x4120, 0x6465,
            0x0000, 0x9003, 0x0000, 0x0000, 0x0000, 0x0300, 0x0000,
            0x7703, 0x0000, 0x5803, 0x0000, 0x0069, 0x0300,
        };
        for (unsigned first = 0; first <= 0xFF; ++first)
        {
            ResetCore();
            std::cout << "probe-radar-name-arm-first-byte slot=" << Hex(slot, 2)
                      << " first=" << Hex(first, 2) << '\n';
            ProbeRadarStream(true, row, 3, slot, 30000, false,
                             static_cast<std::uint8_t>(first));
        }
    }

    void ProbeRadarPacketEntry()
    {
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t target = 0x03000518;

        for (std::uint32_t i = 0; i < 0x200; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(S_BASE_POS_JP + 0, 0);
        Write16(S_BASE_POS_JP + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(S_MAP_HEADER_JP, map);
        Write32(S_MAP_INFO_JP, info);
        Write32(S_MAP_ROW_LEN_JP, rows);
        Write32(S_MAP_DATA_JP, data);

        std::uint32_t cursor = data;
        auto token = [&](std::uint16_t value) {
            Write16(cursor, value);
            cursor += 2;
        };
        auto token32 = [&](std::uint32_t value) {
            token(static_cast<std::uint16_t>(value));
            token(static_cast<std::uint16_t>(value >> 16));
        };
        token(0x8120); // line[0..287]
        token32(0x00000300); // saved r4
        token32(0x00060000); // saved r5
        token32(0x00020001); // saved r6
        token32(0x42010A00); // saved r7
        token32(target | 1); // saved PC
        Write8(rows, static_cast<std::uint8_t>((cursor - data) / 2));

        // Current-route favorite bytes followed by a harmless branch to self.
        Write8(target + 0, 0x37);
        Write8(target + 1, 0x4B);
        Write8(target + 2, 0x41);
        Write8(target + 3, 0x55);
        Write8(target + 4, 0x2D);
        Write8(target + 5, 0xE7);

        SetRegister("lr", (EWRAM_BASE + 0x3FF00) | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", RADAR_DRAW_MAP_JP);
        for (int i = 0; i < 200000; ++i)
        {
            if ((Register("pc") & ~1U) == target)
            {
                std::cout << "probe-radar-packet-entry steps=" << i << '\n';
                PrintState();
                for (int reg = 0; reg <= 12; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    std::cout << name << '=' << Hex(Register(name.c_str()))
                              << (reg == 12 ? '\n' : ' ');
                }
                std::cout << "sp=" << Hex(Register("sp")) << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-packet-entry missed target pc=" << Hex(Register("pc")) << '\n';
    }

    void ProbeRadarDirectSend()
    {
        // This is the shortest first-PC stream found in the PAL Tipa item
        // heap: item 1230 starting at record word 22, followed by item 4769.
        // It returns to GilScreen_Main at 020101BA, immediately before
        // Link_SendEvent(r0, r1, 0).  Reproduce the exact radar tokens here
        // so the live register residue and emitted packet can be measured.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t target = 0x020101BA;
        constexpr std::array<std::uint16_t, 34> tokens = {
            0x3144, 0x0000, 0x808F, 0x6C40, 0x808F, 0x0000,
            0x0401, 0x0000, 0x01BB, 0x0401, 0x01BB, 0x0401, 0x01BB,
            0x0201, 0x0201, 0x01BE, 0x0201, 0x01BF, 0x0064, 0x0019,
            0x0025, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0064,
            0x0019, 0x0025, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000,
        };

        for (std::uint32_t i = 0; i < 0x200; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(S_BASE_POS_JP + 0, 0);
        Write16(S_BASE_POS_JP + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(S_MAP_HEADER_JP, map);
        Write32(S_MAP_INFO_JP, info);
        Write32(S_MAP_ROW_LEN_JP, rows);
        Write32(S_MAP_DATA_JP, data);
        // Stop immediately after the saved-PC high halfword.  Letting the
        // remainder of item 4769 decode includes FFFF RLE tokens; those wrap
        // past IWRAM's end and poison sTxCount before the gadget executes.
        Write8(rows, 14);
        for (std::size_t i = 0; i < tokens.size(); ++i)
            Write16(data + static_cast<std::uint32_t>(i * 2), tokens[i]);
        Write32(S_TX_COUNT_JP, 0);
        Write32(REG_JOY_TRANS, 0);

        SetRegister("lr", (EWRAM_BASE + 0x3FF00) | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", RADAR_DRAW_MAP_JP);
        bool saw_target = false;
        for (int i = 0; i < 200000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (!saw_target && pc >= target && pc <= target + 6)
            {
                saw_target = true;
                std::cout << "probe-radar-direct-send entry steps=" << i;
                for (int reg = 0; reg <= 7; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    std::cout << ' ' << name << '=' << Hex(Register(name.c_str()));
                }
                std::cout << " sp=" << Hex(Register("sp")) << '\n';
            }
            const auto presented = Read32(REG_JOY_TRANS);
            if (presented != 0)
            {
                std::cout << "probe-radar-direct-send emitted pc=" << Hex(Register("pc"))
                          << " presented=" << Hex(presented)
                          << " steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-direct-send stopped pc=" << Hex(Register("pc"))
                  << " saw-target=" << saw_target
                  << " presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " count=" << Read32(S_TX_COUNT_JP)
                  << " sp=" << Hex(Register("sp")) << '\n';
    }

    void ProbeRadarQueueInject()
    {
        // A row long enough to wrap IWRAM can install the transmit queue
        // directly.  This deliberately avoids executing a sender gadget:
        // line 438 restores sLinkWork.connected, lines 454..581 replace all
        // 64 queue words, and lines 582..583 set sTxCount to 64.  The row
        // begins at 03007CAC; saved LR is still line 296.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t target = 0x020101BA;
        constexpr std::uint32_t packet = 0x00B2020C; // open fake letter 178
        std::vector<std::uint16_t> tokens;
        const auto run = [&tokens](std::uint16_t count) {
            tokens.push_back(static_cast<std::uint16_t>(0x8000 | count));
        };
        const auto literal32 = [&tokens](std::uint32_t value) {
            tokens.push_back(static_cast<std::uint16_t>(value));
            tokens.push_back(static_cast<std::uint16_t>(value >> 16));
        };

        run(296);
        literal32(target | 1);
        run(140);
        tokens.push_back(1); // sLinkWork.connected = 1, step = 0
        run(15);
        for (int i = 0; i < 64; ++i)
            literal32(packet);
        literal32(64);

        for (std::uint32_t i = 0; i < 0x400; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(S_BASE_POS_JP + 0, 0);
        Write16(S_BASE_POS_JP + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(S_MAP_HEADER_JP, map);
        Write32(S_MAP_INFO_JP, info);
        Write32(S_MAP_ROW_LEN_JP, rows);
        Write32(S_MAP_DATA_JP, data);
        Write8(rows, static_cast<std::uint8_t>(tokens.size()));
        for (std::size_t i = 0; i < tokens.size(); ++i)
            Write16(data + static_cast<std::uint32_t>(i * 2), tokens[i]);
        Write32(S_TX_COUNT_JP, 0);
        QuiesceJoybusRegisters();

        SetRegister("lr", (EWRAM_BASE + 0x3FF00) | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", RADAR_DRAW_MAP_JP);
        bool installed = false;
        for (int i = 0; i < 200000; ++i)
        {
            if (Read32(S_TX_COUNT_JP) == 64 && Read32(S_TX_QUEUE_JP) == packet)
            {
                std::cout << "probe-radar-queue-inject installed pc=" << Hex(Register("pc"))
                          << " first=" << Hex(Read32(S_TX_QUEUE_JP))
                          << " last=" << Hex(Read32(S_TX_QUEUE_JP + 63 * 4))
                          << " count=" << Read32(S_TX_COUNT_JP)
                          << " tokens=" << tokens.size() << " steps=" << i << '\n';
                installed = true;
                break;
            }
            m_core->step(m_core);
        }
        if (!installed)
        {
            std::cout << "probe-radar-queue-inject failed pc=" << Hex(Register("pc"))
                      << " first=" << Hex(Read32(S_TX_QUEUE_JP))
                      << " count=" << Read32(S_TX_COUNT_JP) << '\n';
            return;
        }

        // The ordinary JoyBus transmit interrupt calls Link_TxPop.  Invoke
        // that exact retail routine once so the installed word and count
        // transition can be observed independently of mGBA's host poll
        // timing in this direct-function probe.
        for (int i = 0; i < 20000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if ((pc >= target && pc <= target + 8) || pc < 0x02000000)
                break;
            m_core->step(m_core);
        }
        std::cout << "probe-radar-queue-inject post-row pc=" << Hex(Register("pc"))
                  << " cpsr=" << Hex(Register("cpsr"))
                  << " ime=" << Hex(Read16(0x04000208), 4)
                  << " ie=" << Hex(Read16(0x04000200), 4) << '\n';
        const auto before = Read32(S_TX_COUNT_JP);
        CallThumb(0x0200153C, 0);
        const auto presented = Read32(REG_JOY_TRANS);
        std::cout << "probe-radar-queue-inject after-pop presented=" << Hex(presented)
                  << " count=" << Read32(S_TX_COUNT_JP)
                  << " delta=" << (before - Read32(S_TX_COUNT_JP)) << '\n';
    }

    void ProbeRadarQueueRepeat(bool western = false, bool compact = false)
    {
        // The wrapped row writes a tiny Thumb stage at 03000000.  It changes
        // sTxCount from zero to -1, then loops at itself with IRQs enabled.
        // Link_TxPop consequently presents queue[0], skips its signed shift
        // loop (1 < -1 is false), decrements the still-negative count, and
        // presents the same word again on every later JoyBus transfer.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t stage = IWRAM_BASE;
        // Repeating bytes 55 00 04 00 makes the 343rd/344th receive words
        // replace the following port's big-endian length with 00000400.
        constexpr std::uint32_t packet = 0x00040055;
        const std::uint32_t radar_draw_map = western ? RADAR_DRAW_MAP_EN : RADAR_DRAW_MAP_JP;
        const std::uint32_t base_pos = western ? S_BASE_POS_EN : S_BASE_POS_JP;
        const std::uint32_t map_header = western ? S_MAP_HEADER_EN : S_MAP_HEADER_JP;
        const std::uint32_t map_info = western ? S_MAP_INFO_EN : S_MAP_INFO_JP;
        const std::uint32_t map_row_len = western ? S_MAP_ROW_LEN_EN : S_MAP_ROW_LEN_JP;
        const std::uint32_t map_data = western ? S_MAP_DATA_EN : S_MAP_DATA_JP;
        const std::uint32_t link_connected = western ? S_LINK_CONNECTED_WESTERN : S_LINK_CONNECTED_JP;
        const std::uint32_t tx_queue = western ? S_TX_QUEUE_WESTERN : S_TX_QUEUE_JP;
        const std::uint32_t tx_count = western ? S_TX_COUNT_WESTERN : S_TX_COUNT_JP;
        const std::uint32_t link_tx_pop = western ? LINK_TX_POP_EN : 0x0200153C;
        std::vector<std::uint16_t> tokens;
        const auto run = [&tokens](std::uint16_t count) {
            tokens.push_back(static_cast<std::uint16_t>(0x8000 | count));
        };
        const auto literal32 = [&tokens](std::uint32_t value) {
            tokens.push_back(static_cast<std::uint16_t>(value));
            tokens.push_back(static_cast<std::uint16_t>(value >> 16));
        };

        if (compact)
        {
            // Radar_DrawMap returns with pop {r4-r7, pc}.  Prime those saved
            // registers from the same overflowing row so the wrapped stage
            // needs only three low-halfword-safe instructions.
            run(288);
            literal32(0);          // r4 = 0
            literal32(tx_count);   // r5 = &sTxCount
            literal32(stage | 1);  // r6 = stage | 1
            literal32(0);          // r7 (unused)
            literal32(stage | 1);  // pc
        }
        else
        {
            run(296);
            literal32(stage | 1);
        }
        run(128);
        if (compact)
        {
            tokens.insert(tokens.end(), {
                0x43E4, // mvns r4, r4 -> -1
                0x602C, // str r4, [r5]
                0x4730, // bx r6
            });
        }
        else
        {
            tokens.insert(tokens.end(), {
                0x4803, // ldr r0, [pc, #12] -> &sTxCount
                0x2100, // movs r1, #0
                0x43C9, // mvns r1, r1       -> -1
                0x6001, // str r1, [r0]
                0x4A02, // ldr r2, [pc, #8]  -> stage | 1
                0x4710, // bx r2
                0x0000, 0x0000,
                static_cast<std::uint16_t>(tx_count), 0x0300,
                0x0001, 0x0300,
            });
        }
        const std::uint32_t stage_end = stage + (compact ? 6 : 24);
        const auto stage_gap = static_cast<std::uint16_t>((link_connected - stage_end) / 2);
        if (stage_gap != 0)
            run(stage_gap);
        tokens.push_back(0x0001); // sLinkWork.connected = 1, step = 0
        run(15);
        literal32(packet);
        run(126);
        literal32(0); // do not transmit until the stage installs -1

        for (std::uint32_t i = 0; i < 0x400; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(base_pos + 0, 0);
        Write16(base_pos + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(map_header, map);
        Write32(map_info, info);
        Write32(map_row_len, rows);
        Write32(map_data, data);
        Write8(rows, static_cast<std::uint8_t>(tokens.size()));
        for (std::size_t i = 0; i < tokens.size(); ++i)
            Write16(data + static_cast<std::uint32_t>(i * 2), tokens[i]);
        Write32(tx_count, 0);
        QuiesceJoybusRegisters();

        SetRegister("lr", (EWRAM_BASE + 0x3FF00) | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", radar_draw_map);
        bool armed = false;
        for (int i = 0; i < 200000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if ((pc == stage || pc == stage + 2) && Read32(tx_count) == 0xFFFFFFFF)
            {
                std::cout << "probe-radar-queue-repeat region=" << (western ? "western" : "JP")
                          << " compact=" << compact
                          << " armed pc=" << Hex(Register("pc"))
                          << " packet=" << Hex(Read32(tx_queue))
                          << " count=" << Hex(Read32(tx_count))
                          << " tokens=" << tokens.size() << " steps=" << i << '\n';
                armed = true;
                break;
            }
            m_core->step(m_core);
        }
        if (!armed)
        {
            std::cout << "probe-radar-queue-repeat failed pc=" << Hex(Register("pc"))
                      << " packet=" << Hex(Read32(tx_queue))
                      << " count=" << Hex(Read32(tx_count)) << '\n';
            return;
        }

        for (int pop = 0; pop < 2; ++pop)
        {
            CallThumb(link_tx_pop, 0);
            std::cout << "probe-radar-queue-repeat pop=" << pop + 1
                      << " presented=" << Hex(Read32(REG_JOY_TRANS))
                      << " packet=" << Hex(Read32(tx_queue))
                      << " count=" << Hex(Read32(tx_count)) << '\n';
        }
    }

    void ProbeRadarQueuePatch(bool western = false)
    {
        // Eleven low-halfword-safe Thumb instructions fit before sLinkWork.
        // They synthesize a high-bit queue word which the radar grammar
        // cannot emit literally, then spin at the final BX with IRQs enabled.
        // The row arms count=1 immediately before returning; the patch runs
        // a handful of instructions later.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t stage = IWRAM_BASE;
        constexpr std::uint32_t packet = 0xB8630055;
        constexpr std::uint32_t packet2 = 0x00001555;
        const std::uint32_t radar_draw_map = western ? RADAR_DRAW_MAP_EN : RADAR_DRAW_MAP_JP;
        const std::uint32_t base_pos = western ? S_BASE_POS_EN : S_BASE_POS_JP;
        const std::uint32_t map_header = western ? S_MAP_HEADER_EN : S_MAP_HEADER_JP;
        const std::uint32_t map_info = western ? S_MAP_INFO_EN : S_MAP_INFO_JP;
        const std::uint32_t map_row_len = western ? S_MAP_ROW_LEN_EN : S_MAP_ROW_LEN_JP;
        const std::uint32_t map_data = western ? S_MAP_DATA_EN : S_MAP_DATA_JP;
        const std::uint32_t link_connected = western ? S_LINK_CONNECTED_WESTERN : S_LINK_CONNECTED_JP;
        const std::uint32_t tx_queue = western ? S_TX_QUEUE_WESTERN : S_TX_QUEUE_JP;
        const std::uint32_t tx_count = western ? S_TX_COUNT_WESTERN : S_TX_COUNT_JP;
        const std::uint32_t link_tx_pop = western ? LINK_TX_POP_EN : 0x0200153C;
        std::vector<std::uint16_t> tokens;
        const auto run = [&tokens](std::uint16_t count) {
            tokens.push_back(static_cast<std::uint16_t>(0x8000 | count));
        };
        const auto literal32 = [&tokens](std::uint32_t value) {
            tokens.push_back(static_cast<std::uint16_t>(value));
            tokens.push_back(static_cast<std::uint16_t>(value >> 16));
        };

        run(296);
        literal32(stage | 1);
        run(128);
        tokens.insert(tokens.end(), {
            0x20B8, // movs r0, #B8
            0x0200, // lsls r0, r0, #8
            0x3063, // adds r0, #63       -> B863
            0x0400, // lsls r0, r0, #16
            0x3055, // adds r0, #55       -> B8630055
            0x4679, // mov r1, pc         -> 0300000C
            static_cast<std::uint16_t>(0x3100 | (western ? 0x84 : 0x2C)),
                    // adds r1, #84/#2C -> sTxQueue[0]
            0x6008, // str r0, [r1]
            0x467A, // mov r2, pc
            0x3201, // adds r2, #1        -> Thumb address of final BX
            0x4710, // bx r2
            0x0000,
        });
        const auto stage_gap = static_cast<std::uint16_t>((link_connected - (stage + 24)) / 2);
        if (stage_gap != 0)
            run(stage_gap);
        tokens.push_back(0x0001); // sLinkWork.connected = 1, step = 0
        run(15);
        literal32(packet & 0xFFFF); // stage installs the high half
        literal32(packet2);
        run(124);
        literal32(2);

        for (std::uint32_t i = 0; i < 0x400; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(base_pos + 0, 0);
        Write16(base_pos + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(map_header, map);
        Write32(map_info, info);
        Write32(map_row_len, rows);
        Write32(map_data, data);
        Write8(rows, static_cast<std::uint8_t>(tokens.size()));
        for (std::size_t i = 0; i < tokens.size(); ++i)
            Write16(data + static_cast<std::uint32_t>(i * 2), tokens[i]);
        Write32(tx_count, 0);
        QuiesceJoybusRegisters();

        SetRegister("lr", (EWRAM_BASE + 0x3FF00) | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", radar_draw_map);
        bool armed = false;
        for (int i = 0; i < 200000; ++i)
        {
            if (Read32(tx_count) == 2 && Read32(tx_queue) == packet &&
                Read32(tx_queue + 4) == packet2)
            {
                std::cout << "probe-radar-queue-patch region=" << (western ? "western" : "JP")
                          << " armed pc=" << Hex(Register("pc"))
                          << " packet=" << Hex(Read32(tx_queue))
                          << " count=" << Read32(tx_count)
                          << " tokens=" << tokens.size() << " steps=" << i << '\n';
                armed = true;
                break;
            }
            m_core->step(m_core);
        }
        if (!armed)
        {
            std::cout << "probe-radar-queue-patch failed pc=" << Hex(Register("pc"))
                      << " packet=" << Hex(Read32(tx_queue))
                      << " count=" << Hex(Read32(tx_count)) << '\n';
            return;
        }
        for (int pop = 0; pop < 2; ++pop)
        {
            CallThumb(link_tx_pop, 0);
            std::cout << "probe-radar-queue-patch pop=" << pop + 1
                      << " presented=" << Hex(Read32(REG_JOY_TRANS))
                      << " count=" << Read32(tx_count) << '\n';
        }
    }

    void ProbeItem1200SmithDispatch()
    {
        // The clean Yuke wrong craft result 1200 indexes gBackdropTiles[0]
        // as an item-name pointer.  The smith recipe row prints that 2048-byte
        // graphics blob at X=16.  Its unchecked Text_Print writes screen 33
        // and phase 3, so the next smith update calls the exit word at
        // gSmithScreens[33], which is the even string pointer 0x0201CDFC.
        constexpr std::uint32_t backdrop_tiles_0 = 0x02020340;
        constexpr std::uint32_t oob_exit = 0x0201CDFC;
        constexpr std::uint32_t return_stub = EWRAM_BASE + 0x3FF00;

        CallThumb(TEXT_INIT_JP | 1, 0);
        CallThumb(TEXT_CLEAR_JP | 1, 0);
        Write32(G_SCREEN_JP, 1);
        Write32(G_MODE_JP, 3);
        Write32(G_LINK_ESTABLISHED_JP, 0);
        Write32(G_SCREEN_PHASE_JP, 1);
        CallThumb(TEXT_SET_X_JP | 1, 16);
        CallThumb(TEXT_PRINT_JP | 1, backdrop_tiles_0, 0);
        std::cout << "item1200 after-render ";
        PrintState();
        std::cout << "phase=" << Read32(G_SCREEN_PHASE_JP)
                  << " oob-exit-word=" << Hex(Read32(0x02025D7C)) << '\n';

        Write16(return_stub, 0xE7FE);
        SetRegister("lr", return_stub | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", SMITH_MODE_UPDATE_JP);
        bool entered = false;
        unsigned trace = 0;
        for (unsigned i = 0; i < 20000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == oob_exit)
                entered = true;
            if (pc == LINK_WRITE_JP || pc == LINK_RESET_JP)
                std::cout << "item1200 reached "
                          << (pc == LINK_WRITE_JP ? "Link_Write" : "Link_Reset")
                          << " step=" << i << " r0=" << Hex(Register("r0")) << '\n';
            if (entered && trace < 48)
            {
                std::cout << "item1200 trace step=" << i
                          << " pc=" << Hex(Register("pc"))
                          << " cpsr=" << Hex(Register("cpsr"))
                          << " r0=" << Hex(Register("r0"))
                          << " r1=" << Hex(Register("r1"))
                          << " r2=" << Hex(Register("r2"))
                          << " r3=" << Hex(Register("r3"))
                          << " sp=" << Hex(Register("sp"))
                          << " lr=" << Hex(Register("lr")) << '\n';
                ++trace;
            }
            if (pc == return_stub || pc == return_stub + 2)
            {
                std::cout << "item1200 smith update returned step=" << i << '\n';
                break;
            }
            m_core->step(m_core);
        }
        std::cout << "item1200 final ";
        PrintState();
        std::cout << "phase=" << Read32(G_SCREEN_PHASE_JP)
                  << " presented=" << Hex(Read32(REG_JOY_TRANS))
                  << " queued=" << Read32(S_TX_COUNT_JP) << '\n';
    }

    void ProbeItemCentered(unsigned item_id, std::uint32_t underpaint = 0,
                           bool smith_context = false,
                           std::int32_t fixed_x = 0x7FFFFFFF)
    {
        // SmithForgeScreen_PrintNextRow centers material names at 104-width.
        // For OOB item 1200 the "name" is a 2048-byte backdrop tile blob,
        // so this begins far below sTextCanvas and walks forward into IWRAM.
        // Keep the return stub below the EWRAM mirror range touched by that
        // draw; the generic CallThumb stub at 0203FF00 is itself overwritten.
        std::uint32_t backdrop_tiles = 0;
        std::uint32_t width = 0;
        switch (item_id)
        {
        case 1200: backdrop_tiles = 0x02020340; width = 13767; break;
        case 1201: backdrop_tiles = 0x02020BC0; width = 13977; break;
        case 1202: backdrop_tiles = 0x02021440; width = 13780; break;
        case 1203: backdrop_tiles = 0x02021CC0; width = 13982; break;
        case 3757: backdrop_tiles = 0x02222288; width = 3805; break;
        case 5585: backdrop_tiles = 0x02222220; width = 4549; break;
        case 42874:
        case 51968: backdrop_tiles = 0x02025FC8; width = 3018; break;
        case 36886:
        case 43670: backdrop_tiles = 0x02025F64; width = 3381; break;
        case 27729: backdrop_tiles = 0x021222C0; width = 3449; break;
        case 37862: backdrop_tiles = 0x02025FC8; width = 3018; break;
        case 44333: backdrop_tiles = 0x02025D98; width = 1179; break;
        case 51400: backdrop_tiles = 0x02025E00; width = 815; break;
        case 37084:
        case 51346: backdrop_tiles = 0x02025E00; width = 815; break;
        case 65416: backdrop_tiles = 0x0201D114; width = 289; break;
        case 6155:
        case 6157: backdrop_tiles = 0x02121021; width = 6304; break;
        case 6159: backdrop_tiles = 0x02222021; width = 8069; break;
        case 6532: backdrop_tiles = 0x02FE1DB1; width = 12300; break;
        case 6738:
        case 6746:
        case 6754:
        case 6762: backdrop_tiles = 0x027E02FE; width = 14082; break;
        case 55899: backdrop_tiles = 0x02020303; width = 14056; break;
        case 64317: backdrop_tiles = 0x025A1E47; width = 11267; break;
        default:
            throw std::runtime_error("centered item overflow ID is not a known JP candidate");
        }
        constexpr std::uint32_t return_stub = EWRAM_BASE + 0x10000;

        // Let AgbMain copy the interrupt dispatcher to IWRAM and settle into
        // its ordinary disconnected loop before invoking the menu renderer.
        RunFrames(2);
        const auto resume_pc = Register("pc");
        const auto resume_lr = Register("lr");
        const auto resume_cpsr = Register("cpsr");

        CallThumb(TEXT_INIT_JP | 1, 0);
        if (smith_context)
            CallThumb(TEXT_SET_FILL_JP | 1, 1, 0);
        CallThumb(TEXT_CLEAR_JP | 1, 0);
        if (smith_context)
        {
            // The bogus string can take over a million instructions even in
            // TEXT_WIDTH mode.  Use the same bounded-call shape as the draw,
            // with IRQs masked so the synthetic return stub is not confused
            // with the retail interrupt frame.
            Write16(return_stub, 0xE7FE);
            const auto saved_cpsr = Register("cpsr");
            SetRegister("r0", backdrop_tiles);
            SetRegister("r1", 1); // TEXT_WIDTH
            SetRegister("lr", return_stub | 1);
            SetRegister("cpsr", saved_cpsr | 0xA0);
            SetRegister("pc", TEXT_PRINT_JP | 1);
            bool measured = false;
            for (unsigned i = 0; i < 5000000; ++i)
            {
                const auto pc = Register("pc") & ~1U;
                if (pc == return_stub || pc == return_stub + 2)
                {
                    measured = true;
                    break;
                }
                m_core->step(m_core);
            }
            if (!measured)
                throw std::runtime_error("centered item width call did not return");
            width = Register("r0");
            SetRegister("cpsr", saved_cpsr);
        }
        // Text_Print composites with OR. Link_TxPop leaves the final
        // transmitted word in queue[0] when the count reaches zero, so a
        // normal earlier packet can act as controllable underpaint.
        Write32(S_TX_QUEUE_JP, underpaint);
        Write32(S_TX_COUNT_JP, 0);
        if (underpaint != 0)
        {
            Write8(S_LINK_CONNECTED_JP, 0xFF);
            Write8(S_LINK_CONNECTED_JP + 2, 0); // watchdog
            Write8(G_LINK_STARTED_JP, 1);
            Write8(G_LINK_ESTABLISHED_JP, 1);
        }
        // The unmodified JP font gives this blob a width of 13,767 pixels.
        // Measuring it in emulation takes over the generic CallThumb budget,
        // and the smith code has already made the same width-only pass before
        // the draw being modelled here.
        CallThumb(TEXT_SET_X_JP | 1,
                  fixed_x == 0x7FFFFFFF ? 104 - width : fixed_x);

        Write16(return_stub, 0xE7FE); // b .
        std::vector<std::uint8_t> ewram_before(0x40000);
        std::vector<std::uint8_t> iwram_before(0x8000);
        for (std::uint32_t i = 0; i < ewram_before.size(); ++i)
            ewram_before[i] = Read8(EWRAM_BASE + i);
        for (std::uint32_t i = 0; i < iwram_before.size(); ++i)
            iwram_before[i] = Read8(IWRAM_BASE + i);
        SetRegister("r0", backdrop_tiles);
        SetRegister("r1", 0); // TEXT_DRAW
        SetRegister("lr", return_stub | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", TEXT_PRINT_JP | 1);
        bool returned = false;
        std::uint32_t previous_tx_word = Read32(S_TX_QUEUE_JP);
        std::uint32_t previous_tx_count = Read32(S_TX_COUNT_JP);
        unsigned tx_word_changes = 0;
        unsigned tx_count_changes = 0;
        for (unsigned i = 0; i < 5000000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (pc == return_stub || pc == return_stub + 2)
            {
                std::cout << "item-centered id=" << item_id << " returned step=" << i << '\n';
                returned = true;
                break;
            }
            m_core->step(m_core);
            if (underpaint != 0)
                Write8(S_LINK_CONNECTED_JP + 2, 0); // model regular connected-host polls
            const auto tx_word = Read32(S_TX_QUEUE_JP);
            if (tx_word != previous_tx_word && tx_word_changes++ < 16)
            {
                std::cout << "item-centered id=" << item_id
                          << " tx-word-step=" << i + 1
                          << " pc=" << Hex(Register("pc"))
                          << " x=" << Hex(Read32(0x030010E8)) << ' '
                          << Hex(previous_tx_word) << " -> " << Hex(tx_word) << '\n';
                previous_tx_word = tx_word;
            }
            const auto tx_count = Read32(S_TX_COUNT_JP);
            if (tx_count != previous_tx_count && tx_count_changes++ < 16)
            {
                std::cout << "item-centered id=" << item_id
                          << " tx-count-step=" << i + 1
                          << " pc=" << Hex(Register("pc")) << ' '
                          << Hex(previous_tx_count) << " -> " << Hex(tx_count) << '\n';
                previous_tx_count = tx_count;
            }
        }
        std::cout << "item-centered id=" << item_id << " width=" << width
                  << " returned=" << returned
                  << " pc=" << Hex(Register("pc"))
                  << " lr=" << Hex(Register("lr"))
                  << " intr0=" << Hex(Read32(0x030029C0))
                  << " screen=" << Hex(Read32(G_SCREEN_JP))
                  << " mode=" << Hex(Read32(G_MODE_JP))
                  << " phase=" << Hex(Read32(G_SCREEN_PHASE_JP))
                  << " smith-context=" << smith_context
                  << " underpaint=" << Hex(underpaint)
                  << " tx-word=" << Hex(Read32(S_TX_QUEUE_JP))
                  << " tx-count=" << Hex(Read32(S_TX_COUNT_JP))
                  << " session-2cfc=" << Hex(Read32(0x03002CFC))
                  << " cmd-slot-count=" << static_cast<unsigned>(Read8(0x03002CFD))
                  << '\n';

        if (returned)
        {
            auto report_changes = [&](const char* label, std::uint32_t base,
                                      const std::vector<std::uint8_t>& before) {
                std::uint32_t first = 0;
                std::uint32_t last = 0;
                std::uint32_t changed = 0;
                bool have = false;
                for (std::uint32_t i = 0; i < before.size(); ++i)
                {
                    if (before[i] == Read8(base + i))
                        continue;
                    if (!have)
                    {
                        first = i;
                        have = true;
                    }
                    last = i;
                    ++changed;
                }
                std::cout << "item-centered id=" << item_id << ' ' << label
                          << " changed=" << changed;
                if (have)
                    std::cout << " range=" << Hex(base + first) << ".." << Hex(base + last);
                std::cout << '\n';
            };
            report_changes("ewram", EWRAM_BASE, ewram_before);
            report_changes("iwram", IWRAM_BASE, iwram_before);
            const std::uint32_t watches[] = {
                LINK_WRITE_JP, LINK_SEND_LETTER_REPLY_JP, RADAR_DRAW_MAP_JP,
                DETAIL_BUF_JP, LIST_BUF_JP, S_TX_QUEUE_JP, S_TX_COUNT_JP,
                G_SCREEN_JP, G_MODE_JP, G_SCREEN_PHASE_JP,
            };
            for (const auto address : watches)
            {
                const auto& before = (address >> 24) == 0x02 ? ewram_before : iwram_before;
                const auto mask = (address >> 24) == 0x02 ? 0x3FFFFU : 0x7FFFU;
                const auto offset = address & mask;
                const auto old_value = static_cast<std::uint32_t>(before[offset]) |
                                       (static_cast<std::uint32_t>(before[(offset + 1) & mask]) << 8) |
                                       (static_cast<std::uint32_t>(before[(offset + 2) & mask]) << 16) |
                                       (static_cast<std::uint32_t>(before[(offset + 3) & mask]) << 24);
                const auto new_value = Read32(address);
                if (old_value != new_value)
                    std::cout << "item-centered id=" << item_id
                              << " changed-word " << Hex(address) << ' '
                              << Hex(old_value) << " -> " << Hex(new_value) << '\n';
            }
            for (std::uint32_t address = 0x02025ABC; address < 0x02025C18; address += 4)
            {
                const auto offset = address & 0x3FFFF;
                const auto old_value = static_cast<std::uint32_t>(ewram_before[offset]) |
                                       (static_cast<std::uint32_t>(ewram_before[offset + 1]) << 8) |
                                       (static_cast<std::uint32_t>(ewram_before[offset + 2]) << 16) |
                                       (static_cast<std::uint32_t>(ewram_before[offset + 3]) << 24);
                const auto new_value = Read32(address);
                if (old_value != new_value)
                    std::cout << "item-centered id=" << item_id
                              << " dispatch-word " << Hex(address) << ' '
                              << Hex(old_value) << " -> " << Hex(new_value) << '\n';
            }
            for (std::uint32_t offset = 0; offset < 0x3300; ++offset)
            {
                const auto new_value = Read8(IWRAM_BASE + offset);
                if (iwram_before[offset] != new_value)
                    std::cout << "item-centered id=" << item_id
                              << " iwram-byte " << Hex(IWRAM_BASE + offset) << ' '
                              << Hex(iwram_before[offset], 2) << " -> "
                              << Hex(new_value, 2) << '\n';
            }
            // A real frame proves whether the resident interrupt dispatcher
            // and enough link/main-loop state survived the centered draw.
            SetRegister("pc", resume_pc);
            SetRegister("lr", resume_lr);
            SetRegister("cpsr", resume_cpsr);
            RunFrames(1);
            std::cout << "item-centered id=" << item_id << " after-frame ";
            PrintState();
            std::cout << "presented=" << Hex(Read32(REG_JOY_TRANS))
                      << " queued=" << Read32(S_TX_COUNT_JP) << '\n';

            if (underpaint != 0)
            {
                // Stop the retail transmit helper immediately after it puts
                // queue[0] on the wire. A corrupted positive count makes its
                // later shift loop enormous, but transmission happens first.
                QuiesceJoybusRegisters();
                Write16(return_stub, 0xE7FE);
                SetRegister("lr", return_stub | 1);
                SetRegister("cpsr", Register("cpsr") | 0x20);
                SetRegister("pc", 0x0200153C | 1);
                bool presented = false;
                for (unsigned i = 0; i < 128; ++i)
                {
                    m_core->step(m_core);
                    if (Read32(REG_JOY_TRANS) == Read32(S_TX_QUEUE_JP))
                    {
                        std::cout << "item-centered id=" << item_id
                                  << " tx-pop-presented=" << Hex(Read32(REG_JOY_TRANS))
                                  << " step=" << i + 1
                                  << " count=" << Hex(Read32(S_TX_COUNT_JP)) << '\n';
                        presented = true;
                        break;
                    }
                }
                if (!presented)
                    std::cout << "item-centered id=" << item_id
                              << " tx-pop-no-presentation pc=" << Hex(Register("pc")) << '\n';
            }
        }
    }

    void ProbeSmithRecipe3841(unsigned material_id = 6155,
                              std::uint32_t underpaint = 0x0000BD5C)
    {
        // Exact Marr's Pass record produced for recipe 3841.  A Lilty cleanly
        // wrong-crafts it from gil 5659.  Its first material is 6155, whose
        // bogus JP item-name pointer is the centered-text queue overwrite.
        std::array<std::uint16_t, 28> words = {
            0x8008, 0x0000, 0x180B, 0x0064, 0x1800, 0x0000, 0x1800, 0x0000,
            0x1800, 0x1800, 0x1800, 0x1800, 0x013A, 0x0300, 0x0000, 0x0000,
            0x013A, 0x0300, 0x0000, 0x0000, 0x013A, 0x0300, 0x0000, 0x0000,
            0x013A, 0x0300, 0x0000, 0x0000,
        };
        constexpr std::uint32_t session = 0x03002CA0;
        constexpr std::uint32_t return_stub = EWRAM_BASE + 0x10000;

        if (material_id != 6155)
        {
            words.fill(0);
            words[2] = static_cast<std::uint16_t>(material_id);
        }

        RunFrames(2);
        Write8(session, 1); // Lilty
        for (unsigned i = 0; i < 64; ++i)
            Write16(session + 0x64 + i * 2, 0xFFFF);
        Write16(session + 0x64, 3841);
        Write32(session + 0x11C, 62460);

        Write8(LIST_BUF_JP, 1);
        Write8(LIST_BUF_JP + 1, 0);
        Write16(LIST_BUF_JP + 2, 0);
        for (unsigned i = 0; i < words.size(); ++i)
            Write16(LIST_BUF_JP + 4 + i * 2, words[i]);
        for (unsigned i = 0; i < 16; ++i)
            Write8(LIST_BUF_JP + 60 + i, 0);

        // The normal known-recipe bitset is only four words, but recipe 3841
        // indexes word 107, bit 16: 030029D4 inside the fixed interrupt copy.
        // That ARM instruction happens to have the bit set.
        CallThumb(SMITH_IS_RECIPE_KNOWN_JP | 1, 0);
        std::cout << "recipe3841 known=" << Register("r0")
                  << " known-word=" << Hex(Read32(0x030029D4)) << '\n';

        CallThumb(SMITH_FORGE_SETUP_JP | 1, 0);
        Write8(0x0300281B, 0); // sSmithSel
        Write32(S_TX_QUEUE_JP, underpaint);
        Write32(S_TX_COUNT_JP, 0);
        Write8(S_LINK_CONNECTED_JP, 0xFF);
        Write8(S_LINK_CONNECTED_JP + 2, 0);
        Write8(G_LINK_STARTED_JP, 1);
        Write8(G_LINK_ESTABLISHED_JP, 1);

        for (unsigned row = 0; row < 5; ++row)
        {
            CallThumb(SMITH_FORGE_PRINT_ROW_JP | 1, 0);
            std::cout << "recipe3841 row=" << row
                      << " next=" << static_cast<unsigned>(Read8(0x03002819))
                      << " queue0=" << Hex(Read32(S_TX_QUEUE_JP))
                      << " count=" << Hex(Read32(S_TX_COUNT_JP)) << '\n';
        }

        Write16(return_stub, 0xE7FE);
        SetRegister("r0", 0);
        SetRegister("lr", return_stub | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", SMITH_FORGE_PRINT_ROW_JP | 1);
        bool returned = false;
        std::uint32_t previous_word = Read32(S_TX_QUEUE_JP);
        std::uint32_t previous_count = Read32(S_TX_COUNT_JP);
        unsigned changes = 0;
        bool saw_draw_entry = false;
        bool saw_negative_x = false;
        for (unsigned i = 0; i < 6000000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            if (!saw_draw_entry && pc == TEXT_PRINT_JP && Register("r1") == 0)
            {
                saw_draw_entry = true;
                std::cout << "smith-material id=" << material_id
                          << " draw-entry step=" << i
                          << " x=" << Hex(Read32(0x030010E8))
                          << " str=" << Hex(Register("r0")) << '\n';
            }
            if (pc == return_stub || pc == return_stub + 2)
            {
                returned = true;
                std::cout << "recipe3841 material-row returned step=" << i << '\n';
                break;
            }
            m_core->step(m_core);
            Write8(S_LINK_CONNECTED_JP + 2, 0);
            const auto text_x = Read32(0x030010E8);
            if (!saw_negative_x && text_x >= 0xFFFF0000)
            {
                saw_negative_x = true;
                std::cout << "smith-material id=" << material_id
                          << " centered-x=" << Hex(text_x)
                          << " width=" << (104U - text_x) << '\n';
            }
            const auto word = Read32(S_TX_QUEUE_JP);
            const auto count = Read32(S_TX_COUNT_JP);
            if ((word != previous_word || count != previous_count) && changes++ < 32)
            {
                std::cout << "smith-material id=" << material_id
                          << " link-change step=" << i + 1
                          << " pc=" << Hex(Register("pc"))
                          << " x=" << Hex(Read32(0x030010E8))
                          << " queue0=" << Hex(previous_word) << "->" << Hex(word)
                          << " count=" << Hex(previous_count) << "->" << Hex(count) << '\n';
                previous_word = word;
                previous_count = count;
            }
        }
        std::cout << "smith-material id=" << material_id
                  << " material-row returned=" << returned
                  << " queue0=" << Hex(Read32(S_TX_QUEUE_JP))
                  << " count=" << Hex(Read32(S_TX_COUNT_JP)) << '\n';

        // Link_TxPop shifts at most the 64-word queue that precedes sTxCount.
        // Report every word in that real queue whose masked command byte is
        // handled by the GameCube.  This keeps exact self-corrupting renders
        // searchable without treating the much larger corrupt count as an
        // allocation or reading arbitrary IWRAM as if it were queued data.
        for (unsigned index = 0; index < 64; ++index)
        {
            const auto word = Read32(S_TX_QUEUE_JP + index * 4);
            const auto command = word & 0x3F;
            const bool handled = command == 0x06 || command == 0x0C ||
                command == 0x14 || command == 0x15 || command == 0x17 ||
                command == 0x18 || command == 0x1A || command == 0x1C ||
                command == 0x1D || command == 0x1E || command == 0x1F;
            if (handled)
            {
                std::cout << "smith-material id=" << material_id
                          << " queue-useful index=" << index
                          << " word=" << Hex(word)
                          << " cmd=" << Hex(command) << '\n';
            }
        }
    }

    void PrepareSmithMaterial(unsigned material_id)
    {
        std::array<std::uint16_t, 28> words{};
        constexpr std::uint32_t session = 0x03002CA0;
        words[2] = static_cast<std::uint16_t>(material_id);

        RunFrames(2);
        Write8(session, 1);
        for (unsigned i = 0; i < 64; ++i)
            Write16(session + 0x64 + i * 2, 0xFFFF);
        Write16(session + 0x64, 3841);
        Write32(session + 0x11C, 62460);

        Write8(LIST_BUF_JP, 1);
        Write8(LIST_BUF_JP + 1, 0);
        Write16(LIST_BUF_JP + 2, 0);
        for (unsigned i = 0; i < words.size(); ++i)
            Write16(LIST_BUF_JP + 4 + i * 2, words[i]);
        for (unsigned i = 0; i < 16; ++i)
            Write8(LIST_BUF_JP + 60 + i, 0);

        CallThumb(SMITH_IS_RECIPE_KNOWN_JP | 1, 0);
        CallThumb(SMITH_FORGE_SETUP_JP | 1, 0);
        Write8(0x0300281B, 0);
        for (unsigned row = 0; row < 5; ++row)
            CallThumb(SMITH_FORGE_PRINT_ROW_JP | 1, 0);
        std::cout << "prepared-smith-material id=" << material_id
                  << " next=" << static_cast<unsigned>(Read8(0x03002819)) << '\n';
    }

    void ProbeRadarRop(int command_kind = 0)
    {
        // The structured smith records can produce 0x02010251 as the first
        // overwritten return address.  In the JP retail client this lands on
        // GilScreen_Main's clean epilogue gadget:
        //
        //   movs r0, #0; add sp, #8; pop {r4-r7}; pop {r1}; bx r1
        //
        // At gadget entry SP is Radar_DrawMap's original entry SP.  For the
        // GilScreen gadget its next PC comes from line[310], fourteen
        // halfwords after the first overwritten PC at line[296], and r4-r7
        // come from line[302..309].  The cmdwrite probe uses a second retail
        // epilogue which first pops r3, shifting those fields by one word.
        // DETAIL_BUF is a separate bulk destination, so a previously
        // downloaded letter body survives the smith-list and map downloads
        // and is an executable second stage.
        constexpr std::uint32_t map = EWRAM_BASE + 0x2E000;
        constexpr std::uint32_t info = map + 0x100;
        constexpr std::uint32_t rows = info + 8;
        constexpr std::uint32_t data = info + 0x20;
        constexpr std::uint32_t normal_return = EWRAM_BASE + 0x3FF00;
        // 0x02010251 enters at "movs r0,#0".  0x02010201 enters an earlier
        // path that plays a sound, sets r0=1, then reaches the same epilogue.
        const bool event_command = command_kind == 0;
        const bool item_command = command_kind == 1;
        const bool cmdslot_command = command_kind == 2;
        const bool letter_command = command_kind == 3 || command_kind == 4;
        const bool route_letter_command = command_kind == 4;
        const bool cmdwrite_command = command_kind == 5;
        const bool equipwrite_command = command_kind == 6;
        const bool route_equipwrite_command = command_kind == 7;
        const bool raw_write_command = command_kind >= 8 && command_kind <= 10;
        const bool en_raw_write_command = command_kind == 9;
        const bool pal_raw_write_command = command_kind == 10;
        const bool western_raw_write_command = en_raw_write_command || pal_raw_write_command;
        const std::uint32_t radar_draw_map = western_raw_write_command ? RADAR_DRAW_MAP_EN : RADAR_DRAW_MAP_JP;
        const std::uint32_t base_pos = western_raw_write_command ? S_BASE_POS_EN : S_BASE_POS_JP;
        const std::uint32_t map_header = western_raw_write_command ? S_MAP_HEADER_EN : S_MAP_HEADER_JP;
        const std::uint32_t map_info = western_raw_write_command ? S_MAP_INFO_EN : S_MAP_INFO_JP;
        const std::uint32_t map_row_len = western_raw_write_command ? S_MAP_ROW_LEN_EN : S_MAP_ROW_LEN_JP;
        const std::uint32_t map_data = western_raw_write_command ? S_MAP_DATA_EN : S_MAP_DATA_JP;
        const std::uint32_t tx_queue = western_raw_write_command ? S_TX_QUEUE_EN : S_TX_QUEUE_JP;
        const std::uint32_t tx_count = western_raw_write_command ? S_TX_COUNT_EN : S_TX_COUNT_JP;
        const std::uint32_t epilogue_gadget = route_equipwrite_command ? EWRAM_BASE + 0x0570C
            : cmdwrite_command ? EWRAM_BASE + 0x02554
            : en_raw_write_command ? EWRAM_BASE + 0x150C4
            : pal_raw_write_command ? EWRAM_BASE + 0x15280
            : EWRAM_BASE + ((event_command || letter_command || raw_write_command) ? 0x10250 : 0x10200);
        const std::uint32_t send_function = event_command ? LINK_SEND_EVENT_JP
            : item_command ? LINK_SEND_ITEM_OP_JP
            : cmdslot_command ? LINK_SEND_CMD_SLOT_JP
            : (equipwrite_command || route_equipwrite_command) ? LINK_SEND_EQUIP_SLOT_JP
            : raw_write_command ? (western_raw_write_command ? LINK_WRITE_EN : LINK_WRITE_JP)
            : LINK_SEND_LETTER_REPLY_JP;
        const std::uint32_t argument = event_command ? 0x42 : item_command ? 0xA6 : 72;
        constexpr std::uint32_t detail_payload = DETAIL_BUF_JP;

        for (std::uint32_t i = 0; i < 0x200; ++i)
            Write8(map + i, 0);
        Write16(G_DATA_FLAGS_JP, 2);
        Write16(base_pos + 0, 0);
        Write16(base_pos + 2, 0);
        Write16(map + 0x14, 0x100);
        Write16(map + 0x16, 8);
        Write16(info + 0, static_cast<std::uint16_t>(-80));
        Write16(info + 2, static_cast<std::uint16_t>(-64));
        Write32(map_header, map);
        Write32(map_info, info);
        Write32(map_row_len, rows);
        Write32(map_data, data);

        std::uint32_t cursor = data;
        auto token = [&](std::uint16_t value) {
            Write16(cursor, value);
            cursor += 2;
        };
        auto token32 = [&](std::uint32_t value) {
            token(static_cast<std::uint16_t>(value));
            token(static_cast<std::uint16_t>(value >> 16));
        };
        if (route_equipwrite_command)
        {
            // Radar's own saved r4-r7 occupy line[288..295].  0200570D moves
            // the restored r4 to r0, pops a replacement r4 and the next PC,
            // then branches.  This gives the route-compatible name an r0=5
            // while retaining controlled r5-r7.
            token(0x8120); // line[0..287]
            token32(5); // saved r4 -> r0 = OOB equip index 5
            token32(LIST_BUF_JP + 0x100 - 52); // saved r5
            token32(0x00004000); // saved r6
            token32(0x02014DB9); // saved r7; sum = 02018DB9
            token32(epilogue_gadget | 1); // line[296..297]
            token32(0); // replacement r4 at line[298..299]
            token32((detail_payload + 2) | 1); // line[300..301]
        }
        else
        {
            token(0x8128); // line[0..295]
            token32(epilogue_gadget | 1);
            for (int i = 0; i < 4; ++i)
                token(0); // line[298..301], skipped by add sp, #8
            if (cmdwrite_command)
                token32(0x02030000); // popped r3
            const std::uint32_t r4 = cmdwrite_command ? 0x00002810 : (send_function | 1);
            token32(r4);
            const std::uint32_t r5 = cmdwrite_command ? LIST_BUF_JP + 0x100 - 52
                : route_letter_command ? LIST_BUF_JP + 0x100 - 52
                : letter_command ? 0x00480001
                : equipwrite_command ? 5 : 0;
            const std::uint32_t r6 = cmdwrite_command ? 0x000040EE
                : route_letter_command ? 2 : letter_command ? 2 : argument;
            const std::uint32_t r6_word = raw_write_command ? 0x78563455
                : equipwrite_command ? 0x9A : r6;
            token32(r5);
            token32(r6_word);
            const std::uint32_t r7 = cmdwrite_command ? 0x02006C61 : 0;
            token32(r7);
            const std::uint32_t second_stage = detail_payload +
                ((route_letter_command || cmdwrite_command) ? 2 : 0);
            token32(second_stage | 1);
        }
        Write8(rows, static_cast<std::uint8_t>((cursor - data) / 2));

        // "1F2F G" is a legal six-character JP name and these bytes are also
        // three Thumb instructions:
        //   mov r1, r6; mov r2, r6; bx r4
        // r0 comes from the selected gadget, so this calls one of:
        // Link_SendEvent(0, 0x42, 0x42), or
        // Link_SendItemOp(1, 0xA6, 0xA6), or
        // Link_SendCmdSlot(1, -72).  The last one installs the Big Chungus
        // hitbox-pointer index directly and bypasses the normal 0..63 list.
        if (route_equipwrite_command)
        {
            // The existing Any% prefix doubles as code: ldr r3,[r5,#52];
            // add r7,r6; bx r7.  The branch enters Smith_EquipResult's tail,
            // which truncates r3 into r1 and calls Link_SendEquipSlot(r0,r1).
            Write16(detail_payload + 0, 0xA682);
            Write16(detail_payload + 2, 0x6B6B);
            Write16(detail_payload + 4, 0x4437);
            Write16(detail_payload + 6, 0x4738);
            Write32(LIST_BUF_JP + 0x100, 0x0000009A);
        }
        else if (cmdwrite_command)
        {
            // "えkk7D8G" preserves the route's 82 A6 6B 6B prefix.  Entering
            // at +2 executes ldr r3,[r5,#52]; add r7,r6; bx r7.  The staged
            // LIST_BUF word supplies r3=02030000.  Popped r7=02006C61 plus
            // r6=000040EE makes 0200AD4F, entering CmdList_SetSlot's tail.
            // It calls Link_SendCmdSlot(low8(r6), *(s16 *)(r3+r4+0xF4));
            // r4=2810 points that load at the next staged LIST_BUF word.
            Write16(detail_payload + 0, 0xA682);
            Write16(detail_payload + 2, 0x6B6B);
            Write16(detail_payload + 4, 0x4437);
            Write16(detail_payload + 6, 0x4738);
            Write32(LIST_BUF_JP + 0x100, 0x02030000);
            Write16(LIST_BUF_JP + 0x104, 0x1234);
        }
        else if (route_letter_command)
        {
            // "えkk3D G" is 82 A6 6B 6B 33 44 20 47.  Entering at its first
            // `k` executes ldr r3,[r5,#52]; add r3,r6; bx r4.  Point r5 at a
            // staged raw word in LIST_BUF, allowing the existing えkk
            // wrong-craft prefix to double as the final letter payload.
            Write16(detail_payload + 0, 0xA682);
            Write16(detail_payload + 2, 0x6B6B);
            Write16(detail_payload + 4, 0x4433);
            Write16(detail_payload + 6, 0x4720);
            Write32(LIST_BUF_JP + 0x100, 0xFFB7FFFF);
        }
        else if (letter_command)
        {
            // "kB3D G": neg r3,r5; add r3,r6; bx r4.  At name entry the
            // gadget/Radar residue already supplies r0=0, low8(r1)=1 and
            // low8(r2)=0.  Starting with r5=0x00480001 and r6=2 constructs
            // r3=0xFFB80001, so Link_SendLetterReply replies to letter zero
            // with item 1 and a signed source slot of -72.
            Write16(detail_payload + 0, 0x426B);
            Write16(detail_payload + 2, 0x4433);
            Write16(detail_payload + 4, 0x4720);
        }
        else if (cmdslot_command)
        {
            // "qB G": neg r1,r6; bx r4.  The radar decoder cannot emit a
            // literal halfword with bit 15 set, so pop +72 and negate it in
            // the name instead of trying to pop 0xFFB8.
            Write16(detail_payload + 0, 0x4271);
            Write16(detail_payload + 2, 0x4720);
        }
        else if (equipwrite_command)
        {
            // "(F1F G": mov r0,r5; mov r1,r6; bx r4.  With r5=5 and
            // r6=0x9A this calls Link_SendEquipSlot(5, 0x9A), producing
            // 1E 05 9A 00.  On the GameCube, signed index 5 aliases
            // m_inventoryItems[0] and signed value 0x9A is item -102.
            Write16(detail_payload + 0, 0x4628);
            Write16(detail_payload + 2, 0x4631);
            Write16(detail_payload + 4, 0x4720);
        }
        else if (raw_write_command)
        {
            // "0F G": mov r0,r6; bx r4.  This calls Link_Write directly
            // with all four bytes supplied by the radar-controlled stack.
            // 55 34 56 78 is deliberately a continuation-shaped word with
            // otherwise unrestricted payload bytes.
            Write16(detail_payload + 0, 0x4630);
            Write16(detail_payload + 2, 0x4720);
        }
        else
        {
            // "1F2F G": mov r1,r6; mov r2,r6; bx r4
            Write16(detail_payload + 0, 0x4631);
            Write16(detail_payload + 2, 0x4632);
            Write16(detail_payload + 4, 0x4720);
        }
        Write16(normal_return, 0xE7FE);
        Write32(tx_count, 0);

        SetRegister("lr", normal_return | 1);
        SetRegister("cpsr", Register("cpsr") | 0x20);
        SetRegister("pc", radar_draw_map);
        std::array<std::uint32_t, 8> name_entry_args{};
        bool saw_name_entry = false;
        for (int i = 0; i < 200000; ++i)
        {
            const auto pc = Register("pc") & ~1U;
            const bool at_second_stage = pc >= detail_payload && pc < detail_payload + 10;
            if (!saw_name_entry && at_second_stage)
            {
                for (int reg = 0; reg < 8; ++reg)
                {
                    const auto name = "r" + std::to_string(reg);
                    name_entry_args[reg] = Register(name.c_str());
                }
                saw_name_entry = true;
            }
            const std::uint32_t queued = Read32(tx_count);
            const std::uint32_t presented = Read32(REG_JOY_TRANS);
            if (queued >= (letter_command ? 3U : 1U) ||
                (raw_write_command && presented == 0x78563455))
            {
                const char* label = event_command ? "probe-radar-rop"
                    : item_command ? "probe-radar-item-rop"
                    : cmdslot_command ? "probe-radar-cmdslot-rop"
                    : cmdwrite_command ? "probe-radar-cmdwrite-rop"
                    : route_equipwrite_command ? "probe-radar-route-equipwrite-rop"
                    : equipwrite_command ? "probe-radar-equipwrite-rop"
                    : en_raw_write_command ? "probe-radar-raw-write-rop-en"
                    : pal_raw_write_command ? "probe-radar-raw-write-rop-pal"
                    : raw_write_command ? "probe-radar-raw-write-rop"
                    : route_letter_command ? "probe-radar-route-letter-rop"
                    : "probe-radar-letter-rop";
                std::cout << label
                          << (queued ? " queued pc=" : " presented pc=")
                          << Hex(Register("pc")) << " packets=";
                for (std::uint32_t packet = 0; packet < queued; ++packet)
                    std::cout << (packet == 0 ? "" : ",") << Hex(Read32(tx_queue + packet * 4));
                if (!queued)
                    std::cout << Hex(presented);
                std::cout << " count=" << queued << " steps=" << i;
                if (saw_name_entry)
                {
                    std::cout << " entry=";
                    for (int reg = 0; reg < 8; ++reg)
                        std::cout << (reg == 0 ? "" : ",") << "r" << reg << "=" << Hex(name_entry_args[reg]);
                }
                std::cout << '\n';
                return;
            }
            if (pc == normal_return || pc == normal_return + 2)
            {
                std::cout << "probe-radar-rop returned normally pc=" << Hex(Register("pc"))
                          << " steps=" << i << '\n';
                return;
            }
            m_core->step(m_core);
        }
        std::cout << "probe-radar-rop stopped pc=" << Hex(Register("pc"))
                  << " lr=" << Hex(Register("lr")) << " r0=" << Hex(Register("r0"))
                  << " tx-count=" << Read32(tx_count);
        if (saw_name_entry)
        {
            std::cout << " entry=";
            for (int reg = 0; reg < 8; ++reg)
                std::cout << (reg == 0 ? "" : ",") << "r" << reg << "=" << Hex(name_entry_args[reg]);
        }
        std::cout << '\n';
    }

private:
    mCore* m_core = nullptr;
    GBASIODriver m_joybus{};
    std::vector<std::uint32_t> m_video;
    std::vector<std::uint8_t> m_western_arm_backing;
};

void PrintResponse(const char* operation, const std::vector<std::uint8_t>& response)
{
    std::cout << operation << ':';
    for (const auto byte : response)
        std::cout << ' ' << Hex(byte, 2).substr(2);
    std::cout << '\n';
}

void RunScript(Harness& harness, const char* path)
{
    std::ifstream file;
    std::istream* source = &std::cin;
    if (std::string(path) != "-")
    {
        file.open(path);
        if (!file)
            throw std::runtime_error(std::string("could not open script: ") + path);
        source = &file;
    }

    std::string line;
    unsigned line_number = 0;
    while (std::getline(*source, line))
    {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line.resize(comment);
        std::istringstream tokens(line);
        std::string command;
        if (!(tokens >> command))
            continue;

        try
        {
            if (command == "handshake")
            {
                harness.Handshake();
            }
            else if (command == "reset")
            {
                PrintResponse("reset", harness.Command(JOY_RESET));
            }
            else if (command == "core-reset")
            {
                harness.ResetCore();
            }
            else if (command == "poll")
            {
                PrintResponse("poll", harness.Command(JOY_POLL));
            }
            else if (command == "read")
            {
                PrintResponse("read", harness.Command(JOY_TRANS));
            }
            else if (command == "write")
            {
                std::string value;
                tokens >> value;
                harness.Write(ParseU32(value));
            }
            else if (command == "send-message")
            {
                std::string payload;
                tokens >> payload;
                harness.SendMessage(ParseHexBytes(payload));
            }
            else if (command == "send-bulk-file")
            {
                std::string type, payload_path;
                tokens >> type >> std::quoted(payload_path);
                if (type.empty() || payload_path.empty())
                    throw std::runtime_error("send-bulk-file needs TYPE and PATH");
                const auto parsed_type = ParseU32(type);
                if (parsed_type > 15)
                    throw std::runtime_error("bulk type must be 0..15");
                harness.SendBulk(static_cast<std::uint8_t>(parsed_type), ReadFileBytes(payload_path));
            }
            else if (command == "call-thumb" || command == "call-thumb-allow")
            {
                std::string address, r0, r1;
                tokens >> address >> r0;
                const auto arg1 = tokens >> r1 ? ParseU32(r1) : 0;
                harness.CallThumb(ParseU32(address), ParseU32(r0), arg1,
                                  command == "call-thumb-allow");
            }
            else if (command == "call-thumb-until")
            {
                std::string address, r0, r1, stop, limit;
                tokens >> address >> r0 >> r1 >> stop;
                if (address.empty() || r0.empty() || r1.empty() || stop.empty())
                    throw std::runtime_error(
                        "call-thumb-until needs ADDRESS R0 R1 STOP [LIMIT]");
                const auto parsed_limit = tokens >> limit ? ParseU32(limit) : 1000000;
                harness.CallThumbUntil(ParseU32(address), ParseU32(r0), ParseU32(r1),
                                       ParseU32(stop), parsed_limit);
            }
            else if (command == "probe-letter-overflow")
            {
                std::string region, prefix, payload_path, offset, size, limit;
                tokens >> region >> prefix >> std::quoted(payload_path) >> offset >> size;
                if (region.empty() || prefix.empty() || payload_path.empty() ||
                    offset.empty() || size.empty())
                    throw std::runtime_error(
                        "probe-letter-overflow needs REGION PREFIX FILE OFFSET SIZE [LIMIT]");
                auto file_data = ReadFileBytes(payload_path);
                const auto parsed_offset = ParseU32(offset);
                const auto parsed_size = ParseU32(size);
                if (parsed_offset > file_data.size() ||
                    parsed_size > file_data.size() - parsed_offset)
                    throw std::runtime_error("letter overflow file slice is out of range");
                std::vector<std::uint8_t> payload(
                    file_data.begin() + parsed_offset,
                    file_data.begin() + parsed_offset + parsed_size);
                const auto parsed_limit = tokens >> limit ? ParseU32(limit) : 20000000;
                harness.ProbeLetterOverflow(region, ParseU32(prefix), payload, parsed_limit);
            }
            else if (command == "step-trace")
            {
                std::string count;
                tokens >> count;
                if (count.empty())
                    throw std::runtime_error("step-trace needs COUNT");
                harness.StepTrace(ParseU32(count));
            }
            else if (command == "wait")
            {
                std::string count;
                tokens >> count;
                harness.RunFrames(ParseU32(count));
            }
            else if (command == "keys")
            {
                std::string value;
                tokens >> value;
                harness.SetKeys(ParseU32(value));
            }
            else if (command == "drain")
            {
                const auto words = harness.Drain();
                std::cout << "drain:";
                for (const auto word : words)
                    std::cout << ' ' << Hex(word);
                std::cout << '\n';
            }
            else if (command == "quiesce")
            {
                harness.QuiesceJoybusRegisters();
            }
            else if (command == "sweep-smith")
            {
                harness.SweepSmith();
            }
            else if (command == "probe-queue-reset")
            {
                harness.ProbeQueueReset();
            }
            else if (command == "probe-name-underpaint")
            {
                harness.ProbeNameUnderpaint();
            }
            else if (command == "probe-letter-reply-reset")
            {
                harness.ProbeLetterReplyReset();
            }
            else if (command == "probe-western-warp-packets")
            {
                harness.ProbeWesternWarpPackets();
            }
            else if (command == "probe-smith-letter-index")
            {
                std::string target;
                tokens >> target;
                if (target.empty())
                    throw std::runtime_error("probe-smith-letter-index needs TARGET");
                const auto parsed_target = ParseU32(target);
                if (parsed_target > 0xFF)
                    throw std::runtime_error("letter index must be 0..255");
                harness.ProbeSmithLetterIndex(static_cast<std::uint8_t>(parsed_target));
            }
            else if (command == "probe-western-smith-letter-index")
            {
                std::string region, target;
                tokens >> region >> target;
                if ((region != "EN" && region != "PAL") || target.empty())
                    throw std::runtime_error(
                        "probe-western-smith-letter-index needs EN|PAL TARGET");
                const auto parsed_target = ParseU32(target);
                if (parsed_target > 0xFF)
                    throw std::runtime_error("letter index must be 0..255");
                harness.ProbeWesternSmithLetterIndex(
                    static_cast<std::uint8_t>(parsed_target), region == "PAL");
            }
            else if (command == "probe-radar-stack")
            {
                harness.ProbeRadarStack();
            }
            else if (command == "probe-radar-stack-western")
            {
                harness.ProbeRadarStack(true);
            }
            else if (command == "probe-radar-stream" ||
                     command == "probe-radar-stream-stop")
            {
                std::string region;
                tokens >> region;
                if (region != "JP" && region != "EN" && region != "PAL")
                    throw std::runtime_error("probe-radar-stream needs JP|EN|PAL and halfword tokens");
                std::vector<std::uint16_t> row;
                std::string value;
                while (tokens >> value)
                {
                    const auto parsed = ParseU32(value);
                    if (parsed > 0xFFFF)
                        throw std::runtime_error("radar token is not a halfword: " + value);
                    row.push_back(static_cast<std::uint16_t>(parsed));
                }
                harness.ProbeRadarStream(region != "JP", row, 0, 0, 200000,
                                         command == "probe-radar-stream-stop");
            }
            else if (command == "probe-radar-map-trampoline")
            {
                harness.ProbeRadarStream(true, {0x8128, 0x0069, 0x0300}, true);
            }
            else if (command == "probe-radar-arm-branch")
            {
                std::string slot;
                tokens >> slot;
                if (slot.empty() || ParseU32(slot) > 63)
                    throw std::runtime_error("probe-radar-arm-branch needs gift slot 0..63");
                // Natural mixed-tribe PAL heap stream: record 401 word 27,
                // followed by records 2500 and 6146, through the saved PC.
                harness.ProbeRadarStream(true, {
                    0x0000, 0x0232, 0x0000, 0x0218, 0x0000, 0x0000, 0x0218,
                    0x0000, 0x0009, 0x0000, 0x0000, 0x0000, 0x0238, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0xA100, 0x4120, 0x6465,
                    0x0000, 0x9003, 0x0000, 0x0000, 0x0000, 0x0300, 0x0000,
                    0x7703, 0x0000, 0x5803, 0x0000, 0x0069, 0x0300,
                }, 2,
                                         static_cast<std::uint8_t>(ParseU32(slot)));
            }
            else if (command == "probe-radar-name-arm-byte")
            {
                std::string name_byte, first_byte;
                tokens >> name_byte >> first_byte;
                if (name_byte.empty() || ParseU32(name_byte) > 255)
                    throw std::runtime_error("probe-radar-name-arm-byte needs byte 0..255");
                const auto first = first_byte.empty() ? 0x43 : ParseU32(first_byte);
                if (first > 0xFF)
                    throw std::runtime_error("optional first name byte must be 0..255");
                harness.ProbeRadarStream(true, {
                    0x0000, 0x0232, 0x0000, 0x0218, 0x0000, 0x0000, 0x0218,
                    0x0000, 0x0009, 0x0000, 0x0000, 0x0000, 0x0238, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0xA100, 0x4120, 0x6465,
                    0x0000, 0x9003, 0x0000, 0x0000, 0x0000, 0x0300, 0x0000,
                    0x7703, 0x0000, 0x5803, 0x0000, 0x0069, 0x0300,
                }, 3, static_cast<std::uint8_t>(ParseU32(name_byte)), 30000,
                                         false, static_cast<std::uint8_t>(first));
            }
            else if (command == "sweep-radar-name-arm")
            {
                harness.SweepRadarNameArm();
            }
            else if (command == "sweep-radar-name-arm-first-byte")
            {
                std::string slot;
                tokens >> slot;
                if (slot.empty() || ParseU32(slot) > 0xFF)
                    throw std::runtime_error(
                        "sweep-radar-name-arm-first-byte needs slot 0..255");
                harness.SweepRadarNameArmFirstByte(
                    static_cast<std::uint8_t>(ParseU32(slot)));
            }
            else if (command == "probe-radar-bg-affine")
            {
                // Synthetic register-control proof for a record-28 ARM stage:
                //   4629 mov r1,r5       destination
                //   0000 movs r0,r0
                //   4632 mov r2,r6       count
                //   01A0 lsls r0,r4,#6   source
                // A clean item record containing SWIEQ 0x0E at byte 5 can
                // therefore invoke BgAffineSet(source, destination, count).
                harness.ProbeRadarStream(true, {
                    0x811A,
                    0x0000, 0x0000, // r8
                    0x0000, 0x0000, // r9
                    0x0000, 0x0000, // r10
                    0x0E00, 0x0008, // r4: 02038000 >> 6
                    0x0090, 0x0300, // r5: transmit queue
                    0x0001, 0x0000, // r6: one affine record
                    0x0000, 0x0000, // r7
                    0x0069, 0x0300, // pc: scalar trampoline
                }, 4, 225, 30000);
            }
            else if (command == "probe-radar-complete-western")
            {
                // Hardware-aligned PAL/EN smith overlay.  A header beginning
                // at item 4000 byte 31 uses item 405 for MapInfo/row lengths.
                // Row 5 begins at item 10284 byte 41, continues through items
                // 17225 and 416, and ends exactly after replacing saved PC
                // with 03000001.  Execution then traverses the real western
                // map/link globals toward the queued-name ARM branch.
                harness.ProbeRadarStream(true, {
                    0xD8FF, 0x23F6, 0x0031, 0x4000, 0x0A60, 0x69C0, 0x007B,
                    0x6000, 0x0005, 0x6900, 0x6905, 0x6205, 0x6D05, 0x6405,
                    0x6405, 0x7505, 0x7705, 0x7905, 0x8005, 0x7005, 0x2069,
                    0x312E, 0x0031, 0x0000, 0x7200, 0x206F, 0x0063, 0x6100,
                    0x6D48, 0x726D, 0x0065, 0x0000, 0x7100, 0x6965, 0x0075,
                    0xF400, 0x0001, 0x2700, 0x3501, 0x0001, 0x0300,
                }, 5, 225, 30000);
            }
            else if (command == "probe-radar-complete-western-late")
            {
                // Exact PAL row 21 assembled from smith records 1369, 13568
                // and 416.  Unlike the earlier row-5 layout, this leaves a
                // clean Z=1 ARM-entry state and records 20 onward available
                // for a downloaded payload.
                harness.ProbeRadarStream(true, {
                    0x98CB, 0x00AB, 0x2000, 0x0041, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x8000, 0xA03F, 0x0040,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0xF400, 0x0001, 0x2700, 0x3501, 0x0001, 0x0300,
                }, 6, 225, 30000);
            }
            else if (command == "probe-radar-complete-western-late-slot")
            {
                std::string slot;
                tokens >> slot;
                if (slot.empty() || ParseU32(slot) > 0xFF)
                    throw std::runtime_error(
                        "probe-radar-complete-western-late-slot needs SLOT (0..255)");
                harness.ProbeRadarStream(true, {
                    0x98CB, 0x00AB, 0x2000, 0x0041, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x8000, 0xA03F, 0x0040,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0xF400, 0x0001, 0x2700, 0x3501, 0x0001, 0x0300,
                }, 6, static_cast<std::uint8_t>(ParseU32(slot)), 30000, false);
            }
            else if (command == "probe-radar-complete-western-late-payload")
            {
                std::string payload;
                tokens >> payload;
                if (payload.empty())
                    throw std::runtime_error(
                        "probe-radar-complete-western-late-payload needs HEXBYTES");
                harness.ProbeRadarStream(true, {
                    0x98CB, 0x00AB, 0x2000, 0x0041, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x8000, 0xA03F, 0x0040,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
                    0xF400, 0x0001, 0x2700, 0x3501, 0x0001, 0x0300,
                }, 6, 225, 200000, false, 0x43, ParseHexBytes(payload));
            }
            else if (command == "probe-western-arm-payload")
            {
                std::string label, payload, limit;
                tokens >> label >> payload;
                if (label.empty() || payload.empty())
                    throw std::runtime_error(
                        "probe-western-arm-payload needs LABEL HEXBYTES [LIMIT]");
                const auto parsed_limit = tokens >> limit ? ParseU32(limit) : 512;
                harness.ProbeWesternArmPayload(label, ParseHexBytes(payload),
                                               static_cast<int>(parsed_limit));
            }
            else if (command == "western-arm-backing")
            {
                std::string payload;
                tokens >> payload;
                if (payload.empty())
                    throw std::runtime_error("western-arm-backing needs HEXBYTES");
                harness.SetWesternArmBacking(ParseHexBytes(payload));
            }
            else if (command == "probe-radar-western-stack-pivot")
            {
                // Exact row-5 byte stream from clean PAL smith records
                // 9111, 4361 and 6425.  It returns to 03000003 and leaves one
                // complete ARM word plus one halfword above the saved PC.
                // Name byte EA branches to PAL LIST_BUF+20C; callers may poke
                // an ARM stack-pivot gadget there before running the probe.
                harness.ProbeRadarStream(true, {
                    0xD8D5, 0x4DD6, 0x00E6, 0x0000, 0x0000, 0x0000, 0x0000,
                    0x0000, 0x0000, 0x0100, 0x600E, 0x00EA, 0x0000, 0x0000,
                    0x0200, 0x0000, 0x0080, 0x0082, 0x0000, 0x6418, 0x4439,
                    0x8431, 0x0039, 0x0900, 0x093A, 0x093A, 0x003A, 0x0000,
                    0x0000, 0x0000, 0x0000, 0x0000, 0x0006, 0x0600, 0x0000,
                    0x3200, 0x0003, 0x0300, 0x0009, 0x0000, 0x0C00,
                }, 5, 0xEA, 200000);
            }
            else if (command == "probe-radar-packet-entry")
            {
                harness.ProbeRadarPacketEntry();
            }
            else if (command == "probe-radar-direct-send")
            {
                harness.ProbeRadarDirectSend();
            }
            else if (command == "probe-radar-queue-inject")
            {
                harness.ProbeRadarQueueInject();
            }
            else if (command == "probe-radar-queue-repeat")
            {
                harness.ProbeRadarQueueRepeat();
            }
            else if (command == "probe-radar-queue-repeat-western")
            {
                harness.ProbeRadarQueueRepeat(true);
            }
            else if (command == "probe-radar-queue-repeat-compact")
            {
                harness.ProbeRadarQueueRepeat(false, true);
            }
            else if (command == "probe-radar-queue-repeat-compact-western")
            {
                harness.ProbeRadarQueueRepeat(true, true);
            }
            else if (command == "probe-radar-queue-patch")
            {
                harness.ProbeRadarQueuePatch();
            }
            else if (command == "probe-radar-queue-patch-western")
            {
                harness.ProbeRadarQueuePatch(true);
            }
            else if (command == "probe-item1200-smith-dispatch")
            {
                harness.ProbeItem1200SmithDispatch();
            }
            else if (command == "probe-item1200-centered")
            {
                harness.ProbeItemCentered(1200);
            }
            else if (command == "probe-item-centered")
            {
                std::string item_id;
                tokens >> item_id;
                if (item_id.empty())
                    throw std::runtime_error("probe-item-centered needs ITEM_ID");
                std::string underpaint;
                tokens >> underpaint;
                harness.ProbeItemCentered(ParseU32(item_id),
                    underpaint.empty() ? 0 : ParseU32(underpaint));
            }
            else if (command == "probe-item-smith-centered")
            {
                std::string item_id;
                tokens >> item_id;
                if (item_id.empty())
                    throw std::runtime_error("probe-item-smith-centered needs ITEM_ID");
                std::string underpaint;
                tokens >> underpaint;
                harness.ProbeItemCentered(ParseU32(item_id),
                    underpaint.empty() ? 0 : ParseU32(underpaint), true);
            }
            else if (command == "probe-item-at-x")
            {
                std::string item_id, x, underpaint;
                tokens >> item_id >> x >> underpaint;
                if (item_id.empty() || x.empty())
                    throw std::runtime_error("probe-item-at-x needs ITEM_ID X [UNDERPAINT]");
                harness.ProbeItemCentered(ParseU32(item_id),
                    underpaint.empty() ? 0 : ParseU32(underpaint), false,
                    static_cast<std::int32_t>(ParseU32(x)));
            }
            else if (command == "probe-smith-recipe3841")
            {
                harness.ProbeSmithRecipe3841();
            }
            else if (command == "probe-smith-material")
            {
                std::string item_id, underpaint;
                tokens >> item_id >> underpaint;
                if (item_id.empty())
                    throw std::runtime_error("probe-smith-material needs ITEM_ID [UNDERPAINT]");
                harness.ProbeSmithRecipe3841(ParseU32(item_id),
                    underpaint.empty() ? 0 : ParseU32(underpaint));
            }
            else if (command == "prepare-smith-material")
            {
                std::string item_id;
                tokens >> item_id;
                if (item_id.empty())
                    throw std::runtime_error("prepare-smith-material needs ITEM_ID");
                harness.PrepareSmithMaterial(ParseU32(item_id));
            }
            else if (command == "probe-radar-rop")
            {
                harness.ProbeRadarRop();
            }
            else if (command == "probe-radar-item-rop")
            {
                harness.ProbeRadarRop(1);
            }
            else if (command == "probe-radar-cmdslot-rop")
            {
                harness.ProbeRadarRop(2);
            }
            else if (command == "probe-radar-letter-rop")
            {
                harness.ProbeRadarRop(3);
            }
            else if (command == "probe-radar-route-letter-rop")
            {
                harness.ProbeRadarRop(4);
            }
            else if (command == "probe-radar-cmdwrite-rop")
            {
                harness.ProbeRadarRop(5);
            }
            else if (command == "probe-radar-equipwrite-rop")
            {
                harness.ProbeRadarRop(6);
            }
            else if (command == "probe-radar-route-equipwrite-rop")
            {
                harness.ProbeRadarRop(7);
            }
            else if (command == "probe-radar-raw-write-rop")
            {
                harness.ProbeRadarRop(8);
            }
            else if (command == "probe-radar-raw-write-rop-en")
            {
                harness.ProbeRadarRop(9);
            }
            else if (command == "probe-radar-raw-write-rop-pal")
            {
                harness.ProbeRadarRop(10);
            }
            else if (command == "state")
            {
                harness.PrintState();
            }
            else if (command == "regs")
            {
                harness.PrintRegisters();
            }
            else if (command == "dump")
            {
                std::string address, size;
                tokens >> address >> size;
                harness.Dump(ParseU32(address), ParseU32(size));
            }
            else if (command == "save-memory")
            {
                std::string address, size, output_path;
                tokens >> address >> size >> std::quoted(output_path);
                if (address.empty() || size.empty() || output_path.empty())
                    throw std::runtime_error("save-memory needs ADDRESS SIZE PATH");
                harness.SaveMemory(ParseU32(address), ParseU32(size), output_path);
            }
            else if (command == "poke8" || command == "poke16" || command == "poke32")
            {
                std::string address, value;
                tokens >> address >> value;
                const auto parsed_address = ParseU32(address);
                const auto parsed_value = ParseU32(value);
                if (command == "poke8")
                    harness.Write8(parsed_address, static_cast<std::uint8_t>(parsed_value));
                else if (command == "poke16")
                    harness.Write16(parsed_address, static_cast<std::uint16_t>(parsed_value));
                else
                    harness.Write32(parsed_address, parsed_value);
            }
            else if (command == "poke-hex")
            {
                std::string address, payload;
                tokens >> address >> payload;
                if (address.empty() || payload.empty())
                    throw std::runtime_error("poke-hex needs ADDRESS and HEXBYTES");
                const auto parsed_address = ParseU32(address);
                const auto bytes = ParseHexBytes(payload);
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    harness.Write8(parsed_address + static_cast<std::uint32_t>(i), bytes[i]);
            }
            else if (command == "poke-file")
            {
                std::string address, payload_path, offset, size;
                tokens >> address >> std::quoted(payload_path) >> offset >> size;
                if (address.empty() || payload_path.empty() || offset.empty() || size.empty())
                    throw std::runtime_error("poke-file needs ADDRESS PATH OFFSET SIZE");
                const auto parsed_address = ParseU32(address);
                const auto parsed_offset = ParseU32(offset);
                const auto parsed_size = ParseU32(size);
                const auto bytes = ReadFileBytes(payload_path);
                if (parsed_offset > bytes.size() || parsed_size > bytes.size() - parsed_offset)
                    throw std::runtime_error("poke-file range is outside input file");
                for (std::uint32_t i = 0; i < parsed_size; ++i)
                    harness.Write8(parsed_address + i, bytes[parsed_offset + i]);
            }
            else
            {
                throw std::runtime_error("unknown command: " + command);
            }
        }
        catch (const std::exception& error)
        {
            throw std::runtime_error(std::string(path) + ':' + std::to_string(line_number) + ": " + error.what());
        }
    }
}

void Usage(const char* argv0)
{
    std::cerr << "usage: " << argv0 << " <ffcc_cli.bin> [transcript.txt]\n"
              << "commands: handshake, reset, poll, read, write WORD, wait FRAMES, state, regs,\n"
              << "          send-message HEXBYTES, send-bulk-file TYPE PATH,\n"
              << "          call-thumb[-allow] ADDRESS R0 [R1], keys MASK, drain,\n"
              << "          call-thumb-until ADDRESS R0 R1 STOP [LIMIT], step-trace COUNT,\n"
              << "          probe-letter-overflow REGION PREFIX FILE OFFSET SIZE [LIMIT],\n"
              << "          quiesce,\n"
              << "          sweep-smith,\n"
              << "          probe-queue-reset, probe-letter-reply-reset,\n"
              << "          probe-western-warp-packets, probe-smith-letter-index TARGET,\n"
              << "          probe-western-smith-letter-index EN|PAL TARGET,\n"
              << "          probe-radar-stack,\n"
              << "          probe-radar-stream[-stop] JP|EN|PAL TOKENS...,\n"
              << "          probe-radar-complete-western,\n"
              << "          probe-radar-western-stack-pivot,\n"
              << "          probe-radar-packet-entry,\n"
              << "          probe-radar-direct-send, probe-radar-queue-inject,\n"
              << "          probe-radar-name-arm-byte BYTE, probe-radar-bg-affine,\n"
              << "          probe-radar-queue-repeat,\n"
              << "          probe-radar-queue-patch,\n"
              << "          probe-item1200-smith-dispatch,\n"
              << "          probe-item1200-centered, probe-item-centered ITEM_ID,\n"
              << "          probe-item-at-x ITEM_ID X [UNDERPAINT],\n"
              << "          probe-radar-rop, probe-radar-item-rop, probe-radar-cmdslot-rop,\n"
              << "          probe-radar-letter-rop,\n"
              << "          probe-radar-route-letter-rop,\n"
              << "          probe-radar-cmdwrite-rop,\n"
              << "          probe-radar-equipwrite-rop,\n"
              << "          probe-radar-route-equipwrite-rop,\n"
              << "          probe-radar-raw-write-rop,\n"
              << "          probe-radar-raw-write-rop-en,\n"
              << "          probe-radar-raw-write-rop-pal,\n"
              << "          dump ADDRESS SIZE,\n"
              << "          poke8/poke16/poke32 ADDRESS VALUE, poke-hex ADDRESS HEXBYTES,\n"
              << "          poke-file ADDRESS PATH OFFSET SIZE\n";
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        Usage(argv[0]);
        return 2;
    }

    try
    {
        Harness harness(argv[1]);
        if (argc == 3)
        {
            RunScript(harness, argv[2]);
        }
        else
        {
            harness.Handshake();
            harness.PrintState();
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
