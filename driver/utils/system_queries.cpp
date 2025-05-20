#include "driver/connection.h"
#include "driver/statement.h"
#include "driver/api/impl/impl.h"

// This function is useful when we want to propagate the diagnostics from
// an ephemeral container, for example a statement created for just one query
/**
 * @brief Copies all diagnostic records from one diagnostics container to another.
 *
 * Transfers each diagnostic record from the source container to the destination container in reverse order.
 *
 * @param from Source diagnostics container.
 * @param to Destination diagnostics container.
 */
void copyDiagnosticsRecords(DiagnosticsContainer * from, DiagnosticsContainer * to)
{
    auto count = from->getDiagStatusCount();
    for (size_t i = count; i > 0; --i)
    {
        auto record = from->getDiagStatus(i);
        to->insertDiagStatus(std::move(record));
    }
}

/**
 * @brief Retrieves the server version string from a database connection.
 *
 * Executes the SQL query `select version()` on the provided connection handle, stores the resulting version string in the supplied buffer, and sets the length of the string. Handles errors by propagating diagnostic records and may throw an exception if the version string retrieval fails.
 *
 * @param buffer_ptr Pointer to the buffer where the server version string will be stored.
 * @param buffer_len Length of the buffer in characters.
 * @param string_length_ptr Pointer to a variable that receives the length of the version string.
 * @return SQLRETURN Status code indicating the result of the operation.
 */
SQLRETURN getServerVersion(
     SQLHDBC         hdbc,
     SQLPOINTER      buffer_ptr,
     SQLSMALLINT     buffer_len,
     SQLSMALLINT *   string_length_ptr
)
{
    SQLRETURN res = SQL_SUCCESS;
    Connection * dbc = static_cast<Connection*>(hdbc);
    Statement * stmt = nullptr;
    res = impl::allocStmt((SQLHDBC)dbc, (SQLHSTMT *)&stmt);
    if (!SQL_SUCCEEDED(res))
        return res;

    res = CALL_WITH_TYPED_HANDLE(
        SQL_HANDLE_STMT,
        stmt,
        [](Statement & stmt) {
            const auto query = toUTF8("select version()");
            stmt.executeQuery(query);
            return SQL_SUCCESS;
        });

    if (SQL_SUCCEEDED(res))
    {
        res = impl::Fetch(stmt);
        if (SQL_SUCCEEDED(res))
        {
            SQLLEN indicator = 0;
            res = impl::GetData(
                stmt,
                1,
                getCTypeFor<SQLTCHAR*>(),
                buffer_ptr,
                buffer_len,
                &indicator
            );
            *string_length_ptr = indicator;
            if (indicator < 0)
            {
                impl::freeHandle(stmt);
                // Wrap the error into the same diagnostic message as all other exception handlers
                CALL_WITH_TYPED_HANDLE(SQL_HANDLE_DBC, dbc, [](auto /*unused*/){
                    throw SqlException("Unexpected value of the server version");
                });
                res = SQL_ERROR;
                *string_length_ptr = 0;
            }
        }
    }

    copyDiagnosticsRecords(stmt, dbc);
    dbc->setReturnCode(res);
    impl::freeHandle(stmt);
    return res;

}
