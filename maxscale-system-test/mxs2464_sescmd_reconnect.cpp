/**
 * MXS-2464: Crash in route_stored_query with ReadWriteSplit
 * https://jira.mariadb.org/browse/MXS-2464
 */

#include "testconnections.h"

void run_test(TestConnections& test, const char* query)
{
    test.maxscales->connect_rwsplit();
    std::thread thr([&]() {
                        sleep(5);
                        test.tprintf("block node 0");
                        test.repl->block_node(0);
                        test.tprintf("wait for monitor");
                        test.maxscales->wait_for_monitor(2);
                        test.tprintf("unblock node 0");
                        test.repl->unblock_node(0);
                    });

    test.set_timeout(60);
    test.tprintf("%s", query);
    test.try_query(test.maxscales->conn_rwsplit[0], "%s", query);
    test.stop_timeout();

    test.tprintf("disconnect");
    test.maxscales->disconnect();
    test.tprintf("join");
    thr.join();
}

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);

    run_test(test, "SET @a = (SELECT SLEEP(10))");

    test.repl->connect();
    auto master_id = test.repl->get_server_id_str(0);
    test.repl->disconnect();

    std::string query = "SET @a = (SELECT SLEEP(CASE @@server_id WHEN " + master_id + " THEN 10 ELSE 0 END))";
    run_test(test, query.c_str());

    return test.global_result;
}
