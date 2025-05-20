#include "driver/api/impl/impl.h"

SQLRETURN getServerVersion(
     SQLHDBC         conn,
     SQLPOINTER      buffer_ptr,
     SQLSMALLINT     buffer_len,
     SQLSMALLINT *   string_length_ptr
);
