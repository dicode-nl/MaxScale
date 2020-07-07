/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-07-07
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "rpl_event.hh"
#include "dbconnection.hh"

#include <maxscale/protocol/mariadb/mysql.hh>

#include <zlib.h>
#include <chrono>
#include <iostream>
#include <iomanip>

using namespace std::literals::chrono_literals;
using namespace std::literals::string_literals;

namespace maxsql
{
constexpr int HEADER_LEN = 19;

int RplEvent::get_event_length(const std::vector<char>& header)
{
    return *((uint32_t*) (header.data() + 4 + 1 + 4));
}

RplEvent::RplEvent(const MariaRplEvent& maria_event)
    : m_raw(maria_event.raw_data(), maria_event.raw_data() + maria_event.raw_data_size())
{
    init();
}

RplEvent::RplEvent(std::vector<char>&& raw_)
    : m_raw(std::move(raw_))
{
    if (m_raw.empty())
    {
        return;
    }

    init();
}

void RplEvent::init()
{
    auto buf = reinterpret_cast<uint8_t*>(&m_raw[0]);

    m_timestamp = mariadb::get_byte4(buf);
    buf += 4;
    m_event_type = mariadb_rpl_event(*buf);
    buf += 1;
    m_server_id = mariadb::get_byte4(buf);
    buf += 4;
    m_event_length = mariadb::get_byte4(buf);
    buf += 4;
    m_next_event_pos = mariadb::get_byte4(buf);
    buf += 4;
    m_flags = mariadb::get_byte2(buf);
    buf += 2;

    auto pCrc = reinterpret_cast<const uint8_t*>(m_raw.data() + m_raw.size() - 4);
    m_checksum = mariadb::get_byte4(pCrc);
}

void RplEvent::set_next_pos(uint32_t next_pos)
{
    m_next_event_pos = next_pos;

    auto buf = reinterpret_cast<uint8_t*>(&m_raw[4 + 1 + 4 + 4]);
    mariadb::set_byte4(buf, m_next_event_pos);

    recalculate_crc();
}

void RplEvent::recalculate_crc()
{
    m_checksum = crc32(0, (uint8_t*)m_raw.data(), m_raw.size() - 4);
    auto pCrc = reinterpret_cast<uint8_t*>(m_raw.data() + m_raw.size() - 4);
    mariadb::set_byte4(pCrc, m_checksum);
}


Rotate RplEvent::rotate() const
{
    Rotate rot;
    rot.is_fake = m_timestamp == 0;
    rot.is_artifical = m_flags & LOG_EVENT_ARTIFICIAL_F;
    rot.file_name = get_rotate_name(m_raw.data(), m_raw.size());

    return rot;
}

std::string RplEvent::query_event_sql() const
{
    std::string sql;

    if (event_type() == QUERY_EVENT)
    {
        constexpr int DBNM_OFF = 8;                 // Database name offset
        constexpr int VBLK_OFF = 4 + 4 + 1 + 2;     // Varblock offset
        constexpr int PHDR_OFF = 4 + 4 + 1 + 2 + 2; // Post-header offset
        constexpr int BINLOG_HEADER_LEN = 19;

        const uint8_t* ptr = (const uint8_t*)this->m_raw.data();
        int dblen = ptr[DBNM_OFF];
        int vblklen = mariadb::get_byte2(ptr + VBLK_OFF);

        int len = event_length() - BINLOG_HEADER_LEN - (PHDR_OFF + vblklen + 1 + dblen);
        sql.assign((const char*) ptr + PHDR_OFF + vblklen + 1 + dblen, len);
    }

    return sql;
}

std::ostream& operator<<(std::ostream& os, const Rotate& rot)
{
    os << rot.file_name << "  is_ariticial=" << rot.is_artifical << "  is_fake=" << rot.is_fake;
    return os;
}

GtidEvent RplEvent::gtid_event() const
{
    auto dptr = pBody();

    auto sequence_nr = *((uint64_t*) dptr);
    dptr += 8;
    auto domain_id = *((uint32_t*) dptr);
    dptr += 4;
    auto flags = *((uint8_t*) dptr);
    dptr += 1;

    uint64_t commit_id = 0;
    if (flags & FL_GROUP_COMMIT_ID)
    {
        commit_id = *((uint64_t*) dptr);
    }

    return GtidEvent({domain_id, 0, sequence_nr}, flags, commit_id);
}

std::ostream& operator<<(std::ostream& os, const GtidEvent& ev)
{
    os << ev.gtid;
    return os;
}

GtidListEvent RplEvent::gtid_list() const
{
    auto dptr = pBody();

    std::vector<Gtid> gtids;
    uint32_t count = *((uint32_t*) dptr);
    dptr += 4;
    for (uint32_t i = 0; i < count; ++i)
    {
        auto domain_id = *((uint32_t*) dptr);
        dptr += 4;
        auto server_id = *((uint32_t*) dptr);
        dptr += 4;
        auto sequence_nr = *((uint64_t*) dptr);
        dptr += 8;
        gtids.push_back({domain_id, server_id, sequence_nr});
    }
    return GtidListEvent(std::move(gtids));
}

std::ostream& operator<<(std::ostream& os, const GtidListEvent& ev)
{
    os << ev.gtid_list;
    return os;
}

std::string dump_rpl_msg(const RplEvent& rpl_event, Verbosity v)
{
    std::ostringstream oss;

    oss << to_string(rpl_event.event_type()) << '\n';

    if (v == Verbosity::All)
    {
        oss << "  timestamp      " << rpl_event.timestamp() << '\n';
        oss << "  event_type      " << rpl_event.event_type() << '\n';
        oss << "  event_length   " << rpl_event.event_length() << '\n';
        oss << "  server_id      " << rpl_event.server_id() << '\n';
        oss << "  next_event_pos " << rpl_event.next_event_pos() << '\n';
        oss << "  flags          " << std::hex << "0x" << rpl_event.flags() << std::dec << '\n';
        oss << "  checksum       " << std::hex << "0x" << rpl_event.checksum() << std::dec << '\n';
    }

    switch (rpl_event.event_type())
    {
    case ROTATE_EVENT:
        {
            auto event = rpl_event.rotate();
            oss << event << '\n';
        }
        break;

    case GTID_EVENT:
        {
            auto event = rpl_event.gtid_event();
            oss << event << '\n';
        }
        break;

    case GTID_LIST_EVENT:
        {
            auto event = rpl_event.gtid_list();
            oss << event << '\n';
        }
        break;

    case FORMAT_DESCRIPTION_EVENT:
        break;

    default:
        // pass
        break;
    }

    return oss.str();
}

// TODO, turn this into an iterator. Use in file_reader as well.
maxsql::RplEvent read_event(std::istream& file, long* file_pos)
{
    std::vector<char> raw(HEADER_LEN);

    file.seekg(*file_pos);
    file.read(raw.data(), HEADER_LEN);

    if (file.eof())
    {
        return maxsql::RplEvent();      // trying to read passed end of file
    }
    else if (!file.good())
    {
        MXS_ERROR("Error reading event at position %ld: %d, %s", *file_pos, errno, mxb_strerror(errno));
        return maxsql::RplEvent();
    }

    auto event_length = maxsql::RplEvent::get_event_length(raw);

    raw.resize(event_length);
    file.read(raw.data() + HEADER_LEN, event_length - HEADER_LEN);

    if (file.eof())
    {
        return maxsql::RplEvent();      // trying to read passed end of file
    }
    else if (!file.good())
    {
        MXS_ERROR("Error reading event at position %ld: %d, %s", *file_pos, errno, mxb_strerror(errno));
        return maxsql::RplEvent();
    }

    maxsql::RplEvent rpl(std::move(raw));

    *file_pos = rpl.next_event_pos();

    return rpl;
}


std::ostream& operator<<(std::ostream& os, const RplEvent& rpl_msg)
{
    os << dump_rpl_msg(rpl_msg, Verbosity::All);
    return os;
}

std::vector<char> create_rotate_event(const std::string& file_name,
                                      uint32_t server_id,
                                      uint32_t pos,
                                      Kind kind)
{
    std::vector<char> data(HEADER_LEN + file_name.size() + 12);
    uint8_t* ptr = (uint8_t*)&data[0];

    // Timestamp, hm.
    mariadb::set_byte4(ptr, 0);
    ptr += 4;

    // This is a rotate event
    *ptr++ = ROTATE_EVENT;

    // server_id
    mariadb::set_byte4(ptr, server_id);
    ptr += 4;

    // Event length
    mariadb::set_byte4(ptr, data.size());
    ptr += 4;

    mariadb::set_byte4(ptr, pos);
    ptr += 4;

    // Flags
    mariadb::set_byte2(ptr, kind == Kind::Artificial ? LOG_EVENT_ARTIFICIAL_F : 0);
    ptr += 2;

    // PAYLOAD
    // The position in the new file. Always sizeof magic.
    mariadb::set_byte8(ptr, 4);
    ptr += 8;

    // The binlog name  (not null-terminated)
    memcpy(ptr, file_name.c_str(), file_name.size());
    ptr += file_name.size();

    // Checksum of the whole event
    mariadb::set_byte4(ptr, crc32(0, (uint8_t*)data.data(), data.size() - 4));

    return data;
}

std::vector<char> create_binlog_checkpoint(const std::string& file_name, uint32_t server_id,
                                           uint32_t next_pos)
{
    std::vector<char> data(HEADER_LEN + 4 + file_name.size() + 4);
    uint8_t* ptr = (uint8_t*)&data[0];

    // Timestamp, hm.
    mariadb::set_byte4(ptr, -1);
    ptr += 4;

    // This is a rotate event
    *ptr++ = BINLOG_CHECKPOINT_EVENT;

    // server_id
    mariadb::set_byte4(ptr, server_id);
    ptr += 4;

    // Event length
    mariadb::set_byte4(ptr, data.size());
    ptr += 4;

    // Next pos
    mariadb::set_byte4(ptr, next_pos);
    ptr += 4;

    // Flags
    mariadb::set_byte2(ptr, 0);
    ptr += 2;

    // PAYLOAD

    // Length of name
    mariadb::set_byte4(ptr, file_name.size());
    ptr += 4;

    // The binlog name  (not null-terminated)
    memcpy(ptr, file_name.c_str(), file_name.size());
    ptr += file_name.size();

    // Checksum of the whole event
    mariadb::set_byte4(ptr, crc32(0, (uint8_t*)data.data(), data.size() - 4));

    return data;
}
}
