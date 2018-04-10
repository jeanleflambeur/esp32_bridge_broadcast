#include "fec_codec.h"
#include <cassert>
#include <algorithm>

#include <Arduino.h>
#ifdef min
# undef min
#endif
#ifdef max
# undef max
#endif


static constexpr unsigned BLOCK_NUMS[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
                                           10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
                                           21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };

const uint8_t Fec_Codec::MAX_CODING_K;
const uint8_t Fec_Codec::MAX_CODING_N;
const size_t Fec_Codec::PACKET_OVERHEAD;

constexpr size_t STACK_SIZE = 2048;

#define ENCODER_LOG(...)
//#define ENCODER_LOG(...) Serial.printf(__VA_ARGS__)
#define DECODER_LOG(...)
//#define DECODER_LOG(...) Serial.printf(__VA_ARGS__)

#pragma pack(push, 1)

struct Packet_Header
{
    //    uint32_t crc = 0;
    uint32_t block_index : 24;
    uint32_t packet_index : 8;
    uint16_t size : 16;
};

#pragma pack(pop)

static_assert(Fec_Codec::PACKET_OVERHEAD == sizeof(Packet_Header), "Check the PACKET_OVERHEAD size");

////////////////////////////////////////////////////////////////////////////////////////////

Fec_Codec::Fec_Codec()
{

}

////////////////////////////////////////////////////////////////////////////////////////////

bool Fec_Codec::init(const Descriptor& descriptor)
{
    if (descriptor.coding_k == 0 ||
        descriptor.coding_n <= descriptor.coding_k ||
        descriptor.coding_k > MAX_CODING_K ||
        descriptor.coding_n > MAX_CODING_N)
    {
        assert(0 && "Invalid descriptor - bad coding params");
        return false;
    }
    if (descriptor.mtu == 0)
    {
        assert(0 && "Invalid descriptor - bad mtu");
        return false;
    }
    if (descriptor.encoder_priority >= configMAX_PRIORITIES)
    {
        assert(0 && "Invalid descriptor - bad encoder priority");
        return false;
    }
    if (descriptor.decoder_priority >= configMAX_PRIORITIES)
    {
        assert(0 && "Invalid descriptor - bad encoder priority");
        return false;
    }

    stop_tasks();

    m_descriptor = descriptor;

    m_encoder_pool_size = m_descriptor.coding_k * 2;
    m_decoder_pool_size = m_descriptor.coding_n * 2;

    if (m_fec)
    {
        fec_free(m_fec);
    }
    m_fec = fec_new(m_descriptor.coding_k, m_descriptor.coding_n);

    m_encoded_packet_size = sizeof(Packet_Header) + m_descriptor.mtu;

    return start_tasks();
}

////////////////////////////////////////////////////////////////////////////////////////////

IRAM_ATTR bool Fec_Codec::is_initialized() const
{
    return m_fec != nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::set_data_encoded_cb(void (*cb)(void* data, size_t size))
{
    m_encoder.cb = cb;
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::set_data_decoded_cb(void (*cb)(void* data, size_t size))
{
    m_decoder.cb = cb;
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::stop_tasks()
{
    if (m_encoder.task)
    {
        vTaskDelete(m_encoder.task);
        m_encoder.task = nullptr;
    }
    if (m_encoder.packet_queue)
    {
        vQueueDelete(m_encoder.packet_queue);
        m_encoder.packet_queue = nullptr;
    }
    if (m_encoder.packet_pool)
    {
        vQueueDelete(m_encoder.packet_pool);
        m_encoder.packet_pool = nullptr;
    }
    if (m_encoder.packet_pool_owned)
    {
        for (size_t i = 0; i < m_encoder_pool_size; i++)
        {
            Encoder::Packet& p = m_encoder.packet_pool_owned[i];
            delete p.data;
        }
        delete[] m_encoder.packet_pool_owned;
        m_encoder.packet_pool_owned = nullptr;
    }
    for (Encoder::Packet& p : m_encoder.block_fec_packets)
    {
        delete p.data;
        p.data = nullptr;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////

    if (m_decoder.task)
    {
        vTaskDelete(m_decoder.task);
        m_decoder.task = nullptr;
    }
    if (m_decoder.packet_queue)
    {
        vQueueDelete(m_decoder.packet_queue);
        m_decoder.packet_queue = nullptr;
    }
    if (m_decoder.packet_pool)
    {
        vQueueDelete(m_decoder.packet_pool);
        m_decoder.packet_pool = nullptr;
    }
    if (m_decoder.packet_pool_owned)
    {
        for (size_t i = 0; i < m_decoder_pool_size; i++)
        {
            Decoder::Packet& p = m_decoder.packet_pool_owned[i];
            delete p.data;
        }
        delete[] m_decoder.packet_pool_owned;
        m_decoder.packet_pool_owned = nullptr;
    }
}

////////////////////////////////////////////////////////////////////////////////////////////

bool Fec_Codec::start_tasks()
{
    stop_tasks();

    m_encoder = Encoder();
    m_decoder = Decoder();

    ////////////////////////////////////////////////////////////////////////////////////////////

    m_encoder.packet_queue = xQueueCreate(m_encoder_pool_size, sizeof(Encoder::Packet));
    if (m_encoder.packet_queue == nullptr)
    {
        stop_tasks();
        return false;
    }
    m_encoder.packet_pool = xQueueCreate(m_encoder_pool_size, sizeof(Encoder::Packet));
    if (m_encoder.packet_pool == nullptr)
    {
        stop_tasks();
        return false;
    }
    m_encoder.packet_pool_owned = new Encoder::Packet[m_encoder_pool_size];
    for (size_t i = 0; i < m_encoder_pool_size; i++)
    {
        m_encoder.packet_pool_owned[i].data = new uint8_t[m_encoded_packet_size];
    }
    for (size_t i = 0; i < m_encoder_pool_size; i++)
    {
        BaseType_t res = xQueueSend(m_encoder.packet_pool, &m_encoder.packet_pool_owned[i], 0);
        if (res != pdPASS)
        {
            stop_tasks();
            return false;
        }
    }
    m_encoder.block_fec_packets.resize(m_descriptor.coding_n - m_descriptor.coding_k);
    for (Encoder::Packet& p : m_encoder.block_fec_packets)
    {
        p.data = new uint8_t[m_encoded_packet_size];
    }
    if (m_descriptor.encoder_core != Core::Any)
    {
        int core = m_descriptor.encoder_core == Core::Core_0 ? 0 : 1;
        BaseType_t res = xTaskCreatePinnedToCore(&static_encoder_task_proc, "Encoder", STACK_SIZE, this, m_descriptor.encoder_priority, &m_encoder.task, core);
        if (res != pdPASS)
        {
            stop_tasks();
            return false;
        }
    }
    else
    {
        BaseType_t res = xTaskCreate(&static_encoder_task_proc, "Encoder", STACK_SIZE, this, m_descriptor.encoder_priority, &m_encoder.task);
        if (res != pdPASS)
        {
            stop_tasks();
            return false;
        }
    }

    ////////////////////////////////////////////////////////////////////////////////////////////


    m_decoder.packet_queue = xQueueCreate(m_decoder_pool_size, sizeof(Decoder::Packet));
    if (m_decoder.packet_queue == nullptr)
    {
        stop_tasks();
        return false;
    }
    m_decoder.packet_pool = xQueueCreate(m_decoder_pool_size, sizeof(Decoder::Packet));
    if (m_decoder.packet_pool == nullptr)
    {
        stop_tasks();
        return false;
    }
    m_decoder.packet_pool_owned = new Decoder::Packet[m_decoder_pool_size];
    for (size_t i = 0; i < m_decoder_pool_size; i++)
    {
        m_decoder.packet_pool_owned[i].data = new uint8_t[m_descriptor.mtu];
    }
    for (size_t i = 0; i < m_decoder_pool_size; i++)
    {
        BaseType_t res = xQueueSend(m_decoder.packet_pool, &m_decoder.packet_pool_owned[i], 0);
        if (res != pdPASS)
        {
            stop_tasks();
            return false;
        }
    }
    if (m_descriptor.decoder_core != Core::Any)
    {
        int core = m_descriptor.decoder_core == Core::Core_0 ? 0 : 1;
        BaseType_t res = xTaskCreatePinnedToCore(&static_decoder_task_proc, "Decoder", STACK_SIZE, this, m_descriptor.decoder_priority, &m_decoder.task, core);
        if (res != pdPASS)
        {
            stop_tasks();
            return false;
        }
    }
    else
    {
        BaseType_t res = xTaskCreate(&static_decoder_task_proc, "Decoder", STACK_SIZE, this, m_descriptor.decoder_priority, &m_decoder.task);
        if (res != pdPASS)
        {
            stop_tasks();
            return false;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::static_encoder_task_proc(void* params)
{
    Fec_Codec* ptr = reinterpret_cast<Fec_Codec*>(params);
    assert(ptr);
    ptr->encoder_task_proc();
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::static_decoder_task_proc(void* params)
{
    Fec_Codec* ptr = reinterpret_cast<Fec_Codec*>(params);
    assert(ptr);
    ptr->decoder_task_proc();
}

////////////////////////////////////////////////////////////////////////////////////////////

IRAM_ATTR void Fec_Codec::encoder_task_proc()
{
    while (true)
    {
        {
            ENCODER_LOG("1: Waiting for packet: %d\n", uxQueueSpacesAvailable(m_encoder.packet_queue));

            Encoder::Packet packet;
            BaseType_t res = xQueueReceive(m_encoder.packet_queue, &packet, portMAX_DELAY);
            if (res == pdFALSE || !packet.data)
            {
                continue;
            }
            ENCODER_LOG("1: Received packet: %d\n", uxQueueSpacesAvailable(m_encoder.packet_queue));

            if (m_encoder.cb)
            {
                seal_packet(packet, m_encoder.last_block_index, m_encoder.block_packets.size());
                m_encoder.cb(packet.data, m_encoded_packet_size);
            }

            m_encoder.block_packets.push_back(packet);
        }

        //compute fec packets
        if (m_encoder.block_packets.size() >= m_descriptor.coding_k)
        {
            if (1)
            {
                //init data for the fec_encode
                for (size_t i = 0; i < m_descriptor.coding_k; i++)
                {
                    Encoder::Packet& packet = m_encoder.block_packets[i];
                    m_encoder.fec_src_ptrs[i] = packet.data + sizeof(Packet_Header);
                }

                size_t fec_count = m_descriptor.coding_n - m_descriptor.coding_k;
                for (size_t i = 0; i < fec_count; i++)
                {
                    m_encoder.fec_dst_ptrs[i] = m_encoder.block_fec_packets[i].data + sizeof(Packet_Header);
                }

                //encode
                fec_encode(m_fec, m_encoder.fec_src_ptrs.data(), m_encoder.fec_dst_ptrs.data(), BLOCK_NUMS + m_descriptor.coding_k, m_descriptor.coding_n - m_descriptor.coding_k, m_descriptor.mtu);

                //seal the result
                for (size_t i = 0; i < fec_count; i++)
                {
                    m_encoder.block_fec_packets[i].size = m_descriptor.mtu;
                    if (m_encoder.cb)
                    {
                        seal_packet(m_encoder.block_fec_packets[i], m_encoder.last_block_index, m_descriptor.coding_k + i);
                        m_encoder.cb(m_encoder.block_fec_packets[i].data, m_encoded_packet_size);
                    }
                }

                //QLOGI("Encoded fec: {}", Clock::now() - start);
            }

            ENCODER_LOG("Returning packets: %d\n", uxQueueSpacesAvailable(m_encoder.packet_pool));
            //return packets to the pool
            for (Encoder::Packet& packet: m_encoder.block_packets)
            {
                BaseType_t res = xQueueSend(m_encoder.packet_pool, &packet, 0);
                assert(res);
            }
            m_encoder.block_packets.clear();
            m_encoder.last_block_index++;
        }
    }

    vTaskDelete(nullptr);
    m_encoder.task = nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////

IRAM_ATTR bool Fec_Codec::encode_data(const void* _data, size_t size, bool block)
{
    if (!m_encoder.task)
    {
        return false;
    }

    Encoder::Packet& crt_packet = m_encoder.crt_packet;

    uint8_t const* data = reinterpret_cast<uint8_t const*>(_data);
    while (size > 0)
    {
        if (!crt_packet.data)
        {
            ENCODER_LOG("0: Waiting for pool packet: %d\n", uxQueueSpacesAvailable(m_encoder.packet_pool));
            BaseType_t res = xQueueReceive(m_encoder.packet_pool, &crt_packet, block ? portMAX_DELAY : 0);
            if (res != pdPASS || !crt_packet.data)
            {
                return false;
            }
            crt_packet.size = 0;
        }

        size_t s = std::min(m_descriptor.mtu - crt_packet.size, size);
        size_t offset = crt_packet.size;
        memcpy(crt_packet.data + sizeof(Packet_Header) + offset, data, s);
        data += s;
        size -= s;
        crt_packet.size += s;

        //packet ready? send for encoding
        if (crt_packet.size >= m_descriptor.mtu)
        {
            ENCODER_LOG("0: Enqueueing packet in the queue: %d\n", uxQueueSpacesAvailable(m_encoder.packet_queue));
            BaseType_t res = xQueueSend(m_encoder.packet_queue, &crt_packet, block ? portMAX_DELAY : 0);
            if (res != pdPASS)
            {
                ENCODER_LOG("0: Failed. Returning packet to the pool: %d\n", uxQueueSpacesAvailable(m_encoder.packet_pool));
                //put it back in the pool and return false
                res = xQueueSend(m_encoder.packet_pool, &crt_packet, 0);
                assert(res == pdPASS);
                crt_packet = Encoder::Packet();
                return false;
            }
            crt_packet = Encoder::Packet();
        }
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////

IRAM_ATTR bool Fec_Codec::decode_data(const void* _data, size_t size, bool block)
{
    if (!m_decoder.task)
    {
        return false;
    }

    Decoder::Packet& crt_packet = m_decoder.crt_packet;

    uint8_t const* data = reinterpret_cast<uint8_t const*>(_data);
    while (size > 0)
    {
        if (!crt_packet.data)
        {
            DECODER_LOG("0: Waiting for pool packet: %d\n", uxQueueSpacesAvailable(m_decoder.packet_pool));
            BaseType_t res = xQueueReceive(m_decoder.packet_pool, &crt_packet, block ? portMAX_DELAY : 0);
            if (res != pdPASS || !crt_packet.data)
            {
                return false;
            }
            crt_packet.size = 0;
            crt_packet.received_header = false;
            crt_packet.is_processed = false;
        }

        //wait until receiving the header
        if (!crt_packet.received_header)
        {
            size_t s = std::min(sizeof(Packet_Header) - crt_packet.size, size);
            size_t offset = crt_packet.size;
            memcpy(crt_packet.data + offset, data, s);
            data += s;
            size -= s;
            crt_packet.size += s;

            //did we receive the header? parse it
            if (crt_packet.size == sizeof(Packet_Header))
            {
                const Packet_Header& header = *reinterpret_cast<const Packet_Header*>(crt_packet.data);
                crt_packet.block_index = header.block_index;
                crt_packet.packet_index = header.packet_index;
                crt_packet.received_header = true;
                crt_packet.size = 0;
            }
        }
        else //we got the header, store only the data now
        {
            size_t s = std::min(m_descriptor.mtu - crt_packet.size, size);
            size_t offset = crt_packet.size;
            memcpy(crt_packet.data + offset, data, s);
            data += s;
            size -= s;
            crt_packet.size += s;
        }

        //packet ready? send for decoding
        if (crt_packet.size >= m_descriptor.mtu)
        {
            DECODER_LOG("0: Enqueueing packet in the queue: %d\n", uxQueueSpacesAvailable(m_decoder.packet_queue));
            BaseType_t res = xQueueSend(m_decoder.packet_queue, &crt_packet, block ? portMAX_DELAY : 0);
            if (res != pdPASS)
            {
                DECODER_LOG("0: Failed. Returning packet to the pool: %d\n", uxQueueSpacesAvailable(m_decoder.packet_pool));
                //put it back in the pool and return false
                res = xQueueSend(m_decoder.packet_pool, &crt_packet, 0);
                assert(res == pdPASS);
                crt_packet = Decoder::Packet();
                return false;
            }
            crt_packet = Decoder::Packet();
        }
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::decoder_task_proc()
{
    while (true)
    {
        {
            Decoder::Packet packet;
            DECODER_LOG("1: Waiting for packet: %d\n", uxQueueSpacesAvailable(m_decoder.packet_queue));

            BaseType_t res = xQueueReceive(m_decoder.packet_queue, &packet, portMAX_DELAY);
            if (res == pdFALSE || !packet.data)
            {
                continue;
            }
            DECODER_LOG("1: Received packet: %d\n", uxQueueSpacesAvailable(m_decoder.packet_queue));

            uint32_t block_index = packet.block_index;
            uint32_t packet_index = packet.packet_index;
            DECODER_LOG("1: Packet %d, block %d\n", packet_index, block_index);

            if (packet_index >= m_descriptor.coding_n)
            {
                DECODER_LOG("1: Packet index out of range: %d > %d\n", packet_index, m_descriptor.coding_n);
                BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                assert(res == pdPASS);
                continue;
            }
            if (block_index < m_decoder.crt_block_index)
            {
                DECODER_LOG("1: Old packet: %d < %d\n", block_index, m_decoder.crt_block_index);
                BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                assert(res == pdPASS);
                continue;
            }

            if (block_index > m_decoder.crt_block_index)
            {
                DECODER_LOG("1: Abandoned block %d due to %d: packets %d, fec packets %d\n", m_decoder.crt_block_index, block_index, m_decoder.block_packets.size(), m_decoder.block_fec_packets.size());
                //purge the entire block, we have a new one coming
                for (Decoder::Packet& packet: m_decoder.block_packets)
                {
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res);
                }
                for (Decoder::Packet& packet: m_decoder.block_fec_packets)
                {
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res);
                }
                m_decoder.block_packets.clear();
                m_decoder.block_fec_packets.clear();
                m_decoder.crt_block_index = block_index;
            }

            //store packet
            if (packet_index >= m_descriptor.coding_k) //fec?
            {
                auto iter = std::lower_bound(m_decoder.block_fec_packets.begin(), m_decoder.block_fec_packets.end(),
                                             packet_index, [](Decoder::Packet const& p, uint32_t index) { return p.packet_index < index; });
                if (iter != m_decoder.block_fec_packets.end() && (*iter).packet_index == packet_index)
                {
                    DECODER_LOG("1: Duplicate fec packet %d from block %d (index %d)\n", packet_index, block_index, block_index * m_descriptor.coding_k + packet_index);
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res == pdPASS);
                    continue;
                }
                else
                {
                    m_decoder.block_fec_packets.insert(iter, packet);
                }
            }
            else
            {
                auto iter = std::lower_bound(m_decoder.block_packets.begin(), m_decoder.block_packets.end(),
                                             packet_index, [](Decoder::Packet const& p, uint32_t index) { return p.packet_index < index; });
                if (iter != m_decoder.block_packets.end() && (*iter).packet_index == packet_index)
                {
                    DECODER_LOG("1: Duplicate packet %d from block %d (index %d)\n", packet_index, block_index, block_index * m_descriptor.coding_k + packet_index);
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res == pdPASS);
                    continue;
                }
                else
                {
                    m_decoder.block_packets.insert(iter, packet);
                }
            }
        }

        {
            //entire block received
            if (m_decoder.block_packets.size() >= m_descriptor.coding_k)
            {
                DECODER_LOG("1: Complete block\n");
                for (Decoder::Packet& packet: m_decoder.block_packets)
                {
                    uint32_t seq_number = packet.block_index * m_descriptor.coding_k + packet.packet_index;
                    if (!packet.is_processed)
                    {
                        //                        if (s_last_seq_number + 1 != seq_number)
                        //                            printf("packet C %d: %s\n", seq_number, s_last_seq_number + 1 == seq_number ? "Ok" : "Skipped");
                        //                        s_last_seq_number = seq_number;

                        if (m_decoder.cb)
                        {
                            m_decoder.cb(packet.data, packet.size);
                        }
                        packet.is_processed = true;
                    }
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res);
                }
                for (Decoder::Packet& packet: m_decoder.block_fec_packets)
                {
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res);
                }
                m_decoder.block_packets.clear();
                m_decoder.block_fec_packets.clear();
                m_decoder.crt_block_index++;
                continue;
            }

            //try to process consecutive packets before the block is finished to minimize latency
            for (size_t i = 0; i < m_decoder.block_packets.size(); i++)
            {
                Decoder::Packet& packet = m_decoder.block_packets[i];
                if (packet.packet_index == i)
                {
                    uint32_t seq_number = packet.block_index * m_descriptor.coding_k + packet.packet_index;
                    if (!packet.is_processed)
                    {
                        //                        if (s_last_seq_number + 1 != seq_number)
                        //                            printf("packet E %d: %s\n", seq_number, s_last_seq_number + 1 == seq_number ? "Ok" : "Skipped");
                        //                        s_last_seq_number = seq_number;
                        if (m_decoder.cb)
                        {
                            m_decoder.cb(packet.data, packet.size);
                        }
                        packet.is_processed = true;
                    }
                }
                else
                {
                    break;
                }
            }

            //can we fec decode?
            if (m_decoder.block_packets.size() + m_decoder.block_fec_packets.size() >= m_descriptor.coding_k)
            {
                DECODER_LOG("1: Complete FEC block\n");

                std::array<unsigned int, 32> indices;
                size_t primary_index = 0;
                size_t used_fec_index = 0;
                for (size_t i = 0; i < m_descriptor.coding_k; i++)
                {
                    if (primary_index < m_decoder.block_packets.size() && i == m_decoder.block_packets[primary_index].packet_index)
                    {
                        m_decoder.fec_src_ptrs[i] = m_decoder.block_packets[primary_index].data;
                        indices[i] = m_decoder.block_packets[primary_index].packet_index;
                        primary_index++;
                    }
                    else
                    {
                        m_decoder.fec_src_ptrs[i] = m_decoder.block_fec_packets[used_fec_index].data;
                        indices[i] = m_decoder.block_fec_packets[used_fec_index].packet_index;
                        used_fec_index++;
                    }
                }

                //insert the missing packets, they will be filled with data by the fec_decode below
                size_t fec_index = 0;
                for (size_t i = 0; i < m_descriptor.coding_k; i++)
                {
                    if (i >= m_decoder.block_packets.size() || i != m_decoder.block_packets[i].packet_index)
                    {
                        Decoder::Packet packet;
                        BaseType_t res = xQueueReceive(m_decoder.packet_pool, &packet, portMAX_DELAY);
                        if (res == pdFALSE || !packet.data)
                        {
                            assert(0);
                            continue;
                        }

                        packet.is_processed = false;
                        packet.size = m_descriptor.mtu;
                        packet.block_index = m_decoder.crt_block_index;
                        packet.packet_index = i;
                        m_decoder.block_packets.insert(m_decoder.block_packets.begin() + i, packet);
                        m_decoder.fec_dst_ptrs[fec_index++] = packet.data;
                    }
                }

                fec_decode(m_fec, m_decoder.fec_src_ptrs.data(), m_decoder.fec_dst_ptrs.data(), indices.data(), m_descriptor.mtu);

                //now dispatch them
                for (Decoder::Packet& packet: m_decoder.block_packets)
                {
                    uint32_t seq_number = packet.block_index * m_descriptor.coding_k + packet.packet_index;
                    if (!packet.is_processed)
                    {
                        //                        if (s_last_seq_number + 1 != seq_number)
                        //                            printf("Packet F %d: %s\n", seq_number, s_last_seq_number + 1 == seq_number ? "Ok" : "Skipped");
                        //                        s_last_seq_number = seq_number;
                        if (m_decoder.cb)
                        {
                            m_decoder.cb(packet.data, packet.size);
                        }
                        packet.is_processed = true;
                    }
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res);
                }
                for (Decoder::Packet& packet: m_decoder.block_fec_packets)
                {
                    BaseType_t res = xQueueSend(m_decoder.packet_pool, &packet, 0);
                    assert(res);
                }
                m_decoder.block_packets.clear();
                m_decoder.block_fec_packets.clear();
                m_decoder.crt_block_index++;
                continue;
            }
        }
    }

    vTaskDelete(nullptr);
    m_decoder.task = nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////

void Fec_Codec::seal_packet(Encoder::Packet& packet, uint32_t block_index, uint8_t packet_index)
{
    Packet_Header& header = *reinterpret_cast<Packet_Header*>(packet.data);
    header.size = packet.size;
    header.block_index = block_index;
    header.packet_index = packet_index;
}

////////////////////////////////////////////////////////////////////////////////////////////

const Fec_Codec::Descriptor& Fec_Codec::get_descriptor() const
{
    return m_descriptor;
}

////////////////////////////////////////////////////////////////////////////////////////////

/*Fec_Codec s_fec_codec;
size_t s_fec_encoded_data_size = 0;
size_t s_fec_decoded_data_size = 0;

void fec_encoded_cb(void* data, size_t size)
{
    s_fec_encoded_data_size += size;
}
void fec_decoded_cb(void* data, size_t size)
{
    s_fec_decoded_data_size += size;
}

void test_fec_encoding()
{
    s_fec_encoded_data_size = 0;
    s_fec_decoded_data_size = 0;
    s_fec_codec.set_data_encoded_cb(&fec_encoded_cb);
    s_fec_codec.set_data_decoded_cb(&fec_decoded_cb);

    LOG("starting test\n");

    uint8_t data[128] = { 0 };

    uint32_t start_tp = millis();

    size_t iteration = 0;
    size_t fec_data_in = 0;
    //encode
    while (millis() - start_tp < 1000)
    {
//        Serial.printf("Encoding %d\n", iteration);

        if (!s_fec_codec.encode_data(data, sizeof(data), true))
        {
            Serial.printf("Failed to encode\n");
            return;
        }
        fec_data_in += sizeof(data);
        //LOG("Pass %d %dms\n", i, millis() - start_pass_tp);

        iteration++;
    }
    float ds = 1.f;//d / 1000.f;
    float total_data_in = fec_data_in / 1024.f;
    float total_data_out = s_fec_encoded_data_size / 1024.f;
    LOG("Total IN: %.2fKB, %.2fKB/s, OUT: %.2fKB, %.2fKB/s\n", total_data_in, total_data_in / ds, total_data_out, total_data_out / ds);
}

volatile size_t xxx = 0;
void fec_encoded2_cb(void* data, size_t size)
{
    s_fec_encoded_data_size += size;

    //if (rand() > RAND_MAX / 4)
    xxx++;

    size_t n = s_fec_codec.get_descriptor().coding_n;
    if ((xxx % n) < n * 75 / 100)
    {
        s_fec_codec.decode_data(data, size, true);
    }
    else
    {
        //LOG("Skipped packet %d\n", xxx);
    }
}

void test_fec_decoding()
{
    s_fec_encoded_data_size = 0;
    s_fec_decoded_data_size = 0;
    s_fec_codec.set_data_encoded_cb(&fec_encoded2_cb);
    s_fec_codec.set_data_decoded_cb(&fec_decoded_cb);

    LOG("starting test\n");

    uint8_t data[128] = { 0 };

    uint32_t start_tp = millis();

    size_t iteration = 0;
    size_t fec_data_in = 0;
    //encode
    while (millis() - start_tp < 1000)
    {
        //Serial.printf("Encoding %d\n", iteration);

        if (!s_fec_codec.encode_data(data, sizeof(data), true))
        {
            Serial.printf("Failed to encode\n");
            return;
        }
        fec_data_in += sizeof(data);
        //LOG("Pass %d %dms\n", i, millis() - start_pass_tp);

        iteration++;
    }
    float ds = 1.f;//d / 1000.f;
    float total_data_in = fec_data_in / 1024.f;
    float total_data_encoded = s_fec_encoded_data_size / 1024.f;
    float total_data_decoded = s_fec_decoded_data_size / 1024.f;
    LOG("Total IN: %.2fKB, %.2fKB/s, ENCODED: %.2fKB, %.2fKB/s, DECODED: %.2fKB, %.2fKB/s\n", total_data_in, total_data_in / ds, total_data_encoded, total_data_encoded / ds, total_data_decoded, total_data_decoded / ds);
}

*/
