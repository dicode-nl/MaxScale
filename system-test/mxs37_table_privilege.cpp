/**
 * @file mxs37_table_privilege.cpp mxs37 (bug719) regression case ("mandatory SELECT privilege on db level?")
 * - create user with only 'SELECT' priveledge
 * - try to connecto to MAxscle with this user
 */

#include <maxbase/format.hh>
#include <maxtest/mariadb_connector.hh>
#include <maxtest/testconnections.hh>
#include <maxtest/sql_t1.hh>

using std::string;

namespace
{
const char db[] = "priv_test";
const char table[] = "priv_test.t1";
const char proc[] = "priv_test.p1";

const char db_user[] = "db_user";
const char db_pass[] = "db_pass";
const char table_user[] = "table_user";
const char table_pass[] = "table_pass";
const char column_user[] = "column_user";
const char column_pass[] = "column_pass";
const char process_user[] = "process_user";
const char process_pass[] = "process_pass";
}

int test_logins(TestConnections& test);

int main(int argc, char* argv[])
{
    TestConnections test(argc, argv);
    test.set_timeout(60);

    test.maxscales->ssl = false;

    auto& mxs = test.maxscale();
    auto conn = mxs.open_rwsplit_connection();

    auto create_user = [&](const char* user, const char* pass) {
        conn->cmd_f("DROP USER IF EXISTS '%s'@'%%'", user);
        conn->cmd_f("CREATE USER '%s'@'%%' IDENTIFIED BY '%s';", user, pass);
    };

    create_user(db_user, db_pass);
    create_user(table_user, table_pass);
    create_user(column_user, column_pass);
    create_user(process_user, process_pass);

    if (test.ok())
    {
        test.tprintf("Users created.");
        // Create a database, a table, a column and a stored procedure.
        conn->cmd_f("CREATE OR REPLACE DATABASE %s;", db);
        conn->cmd_f("CREATE TABLE %s (c1 INT, c2 INT);", table);
        conn->cmd_f("INSERT INTO %s VALUES (1, 2);", table);
        conn->cmd_f("CREATE PROCEDURE %s () "
                    "BEGIN "
                    "SELECT rand(); "
                    "END; ",
                    proc);

        if (test.ok())
        {
            // Check that logging in fails, as none of the users have privs.
            int logins = test_logins(test);
            test.expect(logins == 0, "Login succeeded when it should have failed.");
        }

        if (test.ok())
        {
            const char grant_fmt[] = "GRANT SELECT %s TO '%s'@'%%';";

            string db_grant = mxb::string_printf("ON %s.*", db);
            conn->cmd_f(grant_fmt, db_grant.c_str(), db_user);

            string table_grant = mxb::string_printf("ON %s", table);
            conn->cmd_f(grant_fmt, table_grant.c_str(), table_user);

            string column_grant = mxb::string_printf("(c2) ON %s", table);
            conn->cmd_f(grant_fmt, column_grant.c_str(), column_user);

            conn->cmd_f("GRANT EXECUTE ON PROCEDURE %s TO '%s'@'%%';", proc, process_user);
        }

        if (test.ok())
        {
            int logins = test_logins(test);
            test.expect(logins == 4, "Login failed when it should have succeeded.");
        }
        conn->cmd_f("DROP DATABASE %s;", db);
    }

    const char drop_fmt[] = "DROP USER '%s'@'%%';";
    conn->cmd_f(drop_fmt, db_user);
    conn->cmd_f(drop_fmt, table_user);
    conn->cmd_f(drop_fmt, column_user);
    conn->cmd_f(drop_fmt, process_user);

    return test.global_result;
}

int test_logins(TestConnections& test)
{
    int successes = 0;
    int port = test.maxscales->rwsplit_port[0];
    auto ip = test.maxscales->ip4(0);

    auto test_user = [&](const string& user, const string& pass, const string& query) {
        bool rval = false;
        MYSQL* conn = open_conn_db(port, ip, db, user, pass);
        if (mysql_errno(conn) == 0)
        {
            // Query should work.
            if (test.try_query(conn, "%s", query.c_str()))
            {
                rval = true;
            }
        }
        mysql_close(conn);
        return rval;
    };

    const char query_fmt[] = "SELECT %s from %s;";
    string query = mxb::string_printf(query_fmt, "*", table);
    successes += test_user(db_user, db_pass, query) == true;
    successes += test_user(table_user, table_pass, query) == true;

    query = mxb::string_printf(query_fmt, "c2", table);
    successes += test_user(column_user, column_pass, query) == true;

    query = mxb::string_printf("CALL %s();", proc);
    successes += test_user(process_user, process_pass, query) == true;

    return successes;
};