#include <format>
#include <string>
#include <vector>
#include <algorithm>
#include <gtest/gtest.h>

#include "driver/test/client_test_base.h"
#include "driver/test/result_set_reader.hpp"
#include "driver/utils/sql_encoding.h"

class AuthenticationTest
    : public ClientTestBase
{

public:
    // Prefix for usernames created in tests.
    // The full username format is: {user_prefix}_{getNextUserId()}
    static constexpr std::string_view user_prefix = "odbc_it_user_";

    // The user ID is a suffix in the username to ensure uniqueness.
    // Although the fixture deletes all users created during the test, the test might
    // crash (e.g., due to a segfault or structured exceptions).
    // To avoid collisions, the fixture queries the latest user ID from the database
    // and serves suggested values through this function.
    int getNextUserId()
    {
        return ++next_user_id;
    }

    // Retrieve the latest user ID from the database.
    void SetUp() override
    {
        ClientTestBase::SetUp();

        auto start_user_id_query = fromUTF8<SQLTCHAR>(std::format(
            "SELECT "
            "    max(toInt32(substring(name, {}))) id "
            "FROM system.users "
            "WHERE name LIKE '{}%';",
            user_prefix.size() + 1, user_prefix));

        ODBC_CALL_ON_STMT_THROW(hstmt, SQLExecDirect(hstmt, start_user_id_query.data(), SQL_NTS));

        ResultSetReader reader{hstmt};
        if(reader.fetch())
        {
            next_user_id = reader.getData<SQLINTEGER>("id").value_or(0);
        }
        ODBC_CALL_ON_STMT_THROW(hstmt, SQLFreeStmt(hstmt, SQL_CLOSE));
    }

    // Delete all users created by the tests, i.e., those with the `{user_prefix}` prefix.
    void TearDown() override
    {
        // Close the statement in case the tests failed to do so
        ODBC_CALL_ON_STMT_THROW(hstmt, SQLFreeStmt(hstmt, SQL_CLOSE));

        auto users_query = fromUTF8<SQLTCHAR>(std::format(
            "SELECT "
            "  name "
            "FROM system.users "
            "WHERE name LIKE '{}%' ",
            user_prefix));
        ODBC_CALL_ON_STMT_THROW(hstmt, SQLExecDirect(hstmt, users_query.data(), SQL_NTS));
        std::vector<std::string> users{};

        ResultSetReader reader{hstmt};
        while(reader.fetch())
        {
            users.push_back(reader.getData<std::string>("name").value());
        }
        ODBC_CALL_ON_STMT_THROW(hstmt, SQLFreeStmt(hstmt, SQL_CLOSE));

        for (const auto& user : users)
        {
            auto drop_user_query = fromUTF8<SQLTCHAR>(std::format("DROP USER IF EXISTS '{}'", user));
            ODBC_CALL_ON_STMT_THROW(hstmt, SQLExecDirect(hstmt, drop_user_query.data(), SQL_NTS));
        }

        ClientTestBase::TearDown();
    }

private:
    int next_user_id = 0;

};

// Test various passwords containing special characters.
// The test creates a bunch of users with different passwords and attempts to log in.
TEST_F(AuthenticationTest, PasswordEncoding)
{
    std::vector<std::string> passwords {
        "A", "1", " ", "", "+",
        "AB+", "~", "A~A", "AB~", "AB!",
        "A{A",

        // SQLConnect fails when the password contains a `}` character.
        // See: https://github.com/ClickHouse/clickhouse-odbc/issues/497
        // Example: "A}A",
    };

    // Create passwords of variable length
    // TODO(slabko): Add `}` to this string once
    // https://github.com/ClickHouse/clickhouse-odbc/issues/497 is resolved.
    std::string all_chars =
        "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|~";

    // Create a password from a sliding window over `all_chars`.
    // The window starts small, grows to `window_size`, slides forward, and then shrinks at the end.
    // For example, if `all_chars` is "Hello" and `window_size` is 3,
    // the output would be: H, He, Hel, ell, llo, lo, o.
    static const int32_t window_size = 32;
    for (int32_t i = 1; i < all_chars.size() + window_size; ++i)
    {
        auto pass = all_chars.substr(std::max(0, i - window_size), std::min(i, window_size));
        passwords.emplace_back(std::move(pass));
    }
    // If you're wondering why we're limited to 32 characters, see:
    // https://github.com/ClickHouse/UnixODBC.git UnixODBC uses a fixed 32-byte
    // buffer when the application uses UTF-16 and UTF-8 driver. In this narrow
    // case, the password length cannot exceed 32 characters, or it will be
    // truncated.

    // Create a user for each password
    std::map<std::string, std::string> users{};

    for (size_t i = 0; i < passwords.size(); ++i)
    {
        auto user = std::format("{}{}", user_prefix, getNextUserId());
        auto pass = passwords.at(i);

        auto query = fromUTF8<SQLTCHAR>(std::format(
            "CREATE USER {} IDENTIFIED WITH plaintext_password BY {}", user, toSqlQueryValue(pass)));

        ODBC_CALL_ON_STMT_THROW(hstmt, SQLExecDirect(hstmt, query.data(), SQL_NTS));
        ODBC_CALL_ON_STMT_THROW(hstmt, SQLFreeStmt(hstmt, SQL_CLOSE));

        users.insert({user, pass});
    }

    auto dsn = fromUTF8<SQLTCHAR>(TestEnvironment::getInstance().getDSN());

    // Then attempt to log in with each of the users created above.
    for (const auto& [user, pass] : users)
    {
        SCOPED_TRACE(testing::Message() << std::format("User: {}, Password: {}", user, pass));
        auto user_utf = fromUTF8<SQLTCHAR>(user);
        auto pass_utf = fromUTF8<SQLTCHAR>(pass);

        SQLHENV env = nullptr;
        SQLHDBC dbc = nullptr;
        SQLHSTMT stmt = nullptr;

        try
        {
            ODBC_CALL_ON_ENV_THROW(env, SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env));
            ODBC_CALL_ON_ENV_THROW(env, SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0));
            ODBC_CALL_ON_ENV_THROW(env, SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc));
            ODBC_CALL_ON_DBC_THROW(dbc, SQLConnect(dbc,
                dsn.data(), SQL_NTS,
                user_utf.data(), SQL_NTS,
                pass_utf.data(), SQL_NTS));

            ODBC_CALL_ON_DBC_THROW(dbc, SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt));

            auto query = fromUTF8<SQLTCHAR>("SELECT user() name");
            ODBC_CALL_ON_STMT_THROW(stmt, SQLExecDirect(stmt, query.data(), SQL_NTS));

            ResultSetReader reader{stmt};
            EXPECT_TRUE(reader.fetch());
            EXPECT_EQ(user, reader.getData<std::string>("name").value());
        }
        catch (const std::exception& ex)
        {
            ADD_FAILURE() << std::format(
                "Authentication failed for user: '{}', password: '{}'\n{}", user, pass, ex.what());
        }

        // Cleanup works because all failures are non-fatal:
        // EXPECT is used instead of ASSERT, and ADD_FAILURE instead of FAIL after these
        // handlers were created.
        if (stmt)
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);

        if (dbc)
            SQLFreeHandle(SQL_HANDLE_DBC, dbc);

        if (env)
            SQLFreeHandle(SQL_HANDLE_ENV, env);
    }
}
