/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2024-10-14
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "writer.hh"
#include "config.hh"
#include "file_writer.hh"
#include "inventory.hh"
#include "pinloki.hh"
#include <maxbase/hexdump.hh>

#include <fstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <assert.h>

#include <assert.h>
#include <mariadb_rpl.h>

using namespace std::literals::string_literals;

// TODO multidomain is not handled, except for the state of replication (or m_current_gtid_list).
//      Incidentally this works with multidomain, as long as the master and any new master have
//      the same exact binlogs.
namespace pinloki
{

Writer::Writer(Generator generator, mxb::Worker* worker, InventoryWriter* inv)
    : m_generator(generator)
    , m_worker(worker)
    , m_inventory(*inv)
    , m_current_gtid_list(m_inventory.config().boot_strap_gtid_list())
{
    mxb_assert(m_worker);
    m_thread = std::thread(&Writer::run, this);
}

Writer::~Writer()
{
    m_running = false;
    m_cond.notify_one();
    m_thread.join();
}

mxq::Connection::ConnectionDetails Writer::get_connection_details()
{
    mxq::Connection::ConnectionDetails details;

    m_worker->call([&]() {
                       details = m_generator();
                   }, mxb::Worker::EXECUTE_AUTO);

    return details;
}

mxq::GtidList Writer::get_gtid_io_pos() const
{
    std::lock_guard<std::mutex> guard(m_lock);
    return m_current_gtid_list;
}

void Writer::update_gtid_list(const mxq::Gtid& gtid)
{
    std::lock_guard<std::mutex> guard(m_lock);
    m_current_gtid_list.replace(gtid);
}

void Writer::start_replication(mxq::Connection& conn)
{
    std::vector<maxsql::Gtid> gtids;
    if (m_inventory.rpl_state().empty() && m_current_gtid_list.is_valid())
    {
        // If the m_current_gtid_list is set, meaning a user has set it with
        // set @@global.gtid_slave_pos='0-1000-1234', then the actual start
        // state must be one before that gtid.
        for (auto& g : m_current_gtid_list.gtids())
        {
            gtids.push_back(g.previous());
        }
    }
    else
    {
        gtids = m_current_gtid_list.gtids();
    }

    conn.start_replication(m_inventory.config().server_id(), std::move(gtids));
}

void Writer::run()
{
    while (m_running)
    {
        try
        {
            FileWriter file(&m_inventory, *this);
            mxq::Connection conn(get_connection_details());
            start_replication(conn);

            while (m_running)
            {
                auto rpl_event = maxsql::RplEvent(conn.get_rpl_msg());
                if (rpl_event.event_type() != HEARTBEAT_LOG_EVENT)
                {
                    MXB_SDEBUG("INCOMING " << rpl_event);
                }

                file.add_event(rpl_event);

                switch (rpl_event.event_type())
                {
                case GTID_EVENT:
                    {
                        maxsql::GtidEvent gtid_event = rpl_event.gtid_event();
                        file.begin_txn();
                        update_gtid_list(gtid_event.gtid);

                        if (gtid_event.flags & mxq::F_STANDALONE)
                        {
                            m_commit_on_query = true;
                        }
                    }
                    break;

                case QUERY_EVENT:
                    if (m_commit_on_query)
                    {
                        save_gtid_list(file);
                        m_commit_on_query = false;
                    }
                    else if (rpl_event.is_commit())
                    {
                        save_gtid_list(file);
                    }
                    break;

                case XID_EVENT:
                    save_gtid_list(file);
                    break;

                default:
                    break;
                }
            }
        }
        catch (const std::exception& x)
        {
            if (m_timer.alarm())
            {
                MXS_ERROR("Error received during replication: %s", x.what());
            }

            auto new_gtid_list = m_inventory.config().boot_strap_gtid_list();

            if (new_gtid_list.to_string() == m_current_gtid_list.to_string())
            {
                std::unique_lock<std::mutex> guard(m_lock);
                m_cond.wait_for(guard, std::chrono::seconds(1), [this]() {
                                    return !m_running;
                                });
            }
            else
            {
                m_current_gtid_list = new_gtid_list;
            }
        }
    }
}

void Writer::save_gtid_list(FileWriter& file_writer)
{
    if (m_current_gtid_list.is_valid())
    {
        file_writer.commit_txn();

        std::ofstream ofs(m_inventory.config().gtid_file_path());
        ofs << m_current_gtid_list;
        ofs.flush();
    }
}
}
