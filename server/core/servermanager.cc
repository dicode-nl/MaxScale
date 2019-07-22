/*
 * Copyright (c) 2019 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2023-01-01
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "internal/servermanager.hh"

#include <mutex>
#include <string>
#include <vector>
#include <maxbase/format.hh>
#include <maxscale/json_api.hh>

using std::string;
using Guard = std::lock_guard<std::mutex>;

namespace
{

class ThisUnit
{
public:

    /**
     * Call a function on every server in the global server list.
     *
     * @param apply The function to apply. If the function returns false, iteration is discontinued.
     */
    void foreach_server(std::function<bool(Server*)> apply)
    {
        Guard guard(m_all_servers_lock);
        for (Server* server : m_all_servers)
        {
            if (!apply(server))
            {
                break;
            }
        }
    }

    void insert_front(Server* server)
    {
        Guard guard(m_all_servers_lock);
        m_all_servers.insert(m_all_servers.begin(), server);
    }

    void erase(Server* server)
    {
        Guard guard(m_all_servers_lock);
        auto it = std::find(m_all_servers.begin(), m_all_servers.end(), server);
        mxb_assert(it != m_all_servers.end());
        m_all_servers.erase(it);
    }

private:
    std::mutex m_all_servers_lock;         /**< Protects access to array */
    std::vector<Server*> m_all_servers;    /**< Global list of servers, in configuration file order */
};

ThisUnit this_unit;

}

Server* ServerManager::create_server(const char* name, const MXS_CONFIG_PARAMETER& params)
{
    Server* server = Server::server_alloc(name, params);
    if (server)
    {
        // This keeps the order of the servers the same as in 2.2
        this_unit.insert_front(server);
    }
    return server;
}


void ServerManager::server_free(Server* server)
{
    mxb_assert(server);
    this_unit.erase(server);

    /* Clean up session and free the memory */
    if (server->persistent)
    {
        int nthr = config_threadcount();

        for (int i = 0; i < nthr; i++)
        {
            dcb_persistent_clean_count(server->persistent[i], i, true);
        }
        MXS_FREE(server->persistent);
    }

    delete server;
}

Server* ServerManager::find_by_unique_name(const string& name)
{
    Server* rval = nullptr;
    this_unit.foreach_server(
            [&rval, name](Server* server) {
                 if (server->is_active && server->name() == name)
                 {
                     rval = server;
                     return false;
                 }
                 return true;
            }
    );
    return rval;
}

void ServerManager::printAllServers()
{
    this_unit.foreach_server([](Server* server) {
        if (server->server_is_active())
        {
            server->printServer();
        }
        return true;
    });
}

void ServerManager::dprintAllServers(DCB* dcb)
{
    this_unit.foreach_server([dcb](Server* server) {
        if (server->is_active)
        {
            Server::dprintServer(dcb, server);
        }
        return true;
    });
}

void ServerManager::dListServers(DCB* dcb)
{
    const string horizontalLine =
            "-------------------+-----------------+-------+-------------+--------------------\n";
    string message;
    // Estimate the likely size of the string. Should be enough for 5 servers.
    message.reserve((4 + 5) * horizontalLine.length());
    message += "Servers.\n" + horizontalLine;
    message += mxb::string_printf("%-18s | %-15s | Port  | Connections | %-20s\n",
                                  "Server", "Address", "Status");
    message += horizontalLine;

    bool have_servers = false;
    this_unit.foreach_server(
            [&message, &have_servers](Server* server) {
                if (server->server_is_active())
                {
                    have_servers = true;
                    string stat = server->status_string();
                    message += mxb::string_printf("%-18s | %-15s | %5d | %11d | %s\n",
                                                  server->name(), server->address, server->port,
                                                  server->stats().n_current, stat.c_str());
                }
                return true;
            });

    if (have_servers)
    {
        message += horizontalLine;
        dcb_printf(dcb, "%s", message.c_str());
    }
}

/**
 * Return a resultset that has the current set of servers in it
 *
 * @return A Result set
 */
std::unique_ptr<ResultSet> ServerManager::getList()
{
    std::unique_ptr<ResultSet> set =
            ResultSet::create({"Server", "Address", "Port", "Connections", "Status"});

    this_unit.foreach_server(
            [&set](Server* server) {
                if (server->server_is_active())
                {
                    string stat = server->status_string();
                    set->add_row({server->name(), server->address,
                                  std::to_string(server->port),
                                  std::to_string(server->stats().n_current), stat});
                }
                return true;
            });

    return set;
}

json_t* ServerManager::server_list_to_json(const char* host)
{
    json_t* data = json_array();
    this_unit.foreach_server(
            [data, host](Server* server) {
                if (server->server_is_active())
                {
                    json_array_append_new(data, server->to_json_data(host));
                }
                return true;
            });
    return mxs_json_resource(host, MXS_JSON_API_SERVERS, data);
}

SERVER* SERVER::find_by_unique_name(const string& name)
{
    return ServerManager::find_by_unique_name(name);
}

std::vector<SERVER*> SERVER::server_find_by_unique_names(const std::vector<string>& server_names)
{
    std::vector<SERVER*> rval;
    rval.reserve(server_names.size());
    for (auto elem : server_names)
    {
        rval.push_back(ServerManager::find_by_unique_name(elem));
    }
    return rval;
}

void ServerManager::dprintAllServersJson(DCB* dcb)
{
    json_t* all_servers_json = ServerManager::server_list_to_json("");
    char* dump = json_dumps(all_servers_json, JSON_INDENT(4));
    dcb_printf(dcb, "%s", dump);
    MXS_FREE(dump);
    json_decref(all_servers_json);
}