#pragma once

#include <Columns/IColumn_fwd.h>
#include <Core/QueryLogElementType.h>
#include <Interpreters/Cache/QueryResultCacheUsage.h>
#include <Interpreters/ClientInfo.h>
#include <Parsers/IAST.h>
#include <Storages/ColumnsDescription.h>
#include <Common/ProfileEvents.h>
#include <Common/TransactionID.h>

#include <memory>
#include <set>
#include <unordered_set>

namespace ProfileEvents
{
class Counters;
}


namespace DB
{
struct AsyncReadCounters;
struct Settings;

/** Allows to log information about queries execution:
  * - info about start of query execution;
  * - performance metrics (are set at the end of query execution);
  * - info about errors of query execution.
  */

/// A struct which will be inserted as row into query_log table
struct QueryLogElement
{
    using Type = QueryLogElementType;

    Type type = QUERY_START;

    /// Depending on the type of query and type of stage, not all the fields may be filled.

    time_t event_time{};
    Decimal64 event_time_microseconds{};
    time_t query_start_time{};
    Decimal64 query_start_time_microseconds{};
    UInt64 query_duration_ms{};

    /// The data fetched from DB to execute the query
    UInt64 read_rows{};
    UInt64 read_bytes{};

    /// The data written to DB
    UInt64 written_rows{};
    UInt64 written_bytes{};

    /// The data sent to the client
    UInt64 result_rows{};
    UInt64 result_bytes{};

    UInt64 memory_usage{};

    String current_database;
    String query;
    String formatted_query;
    UInt64 normalized_query_hash{};

    IAST::QueryKind query_kind{};
    std::set<String> query_databases;
    std::set<String> query_tables;
    std::set<String> query_columns;
    std::set<String> query_partitions;
    std::set<String> query_projections;
    std::set<String> query_views;

    std::unordered_set<String> used_aggregate_functions;
    std::unordered_set<String> used_aggregate_function_combinators;
    std::unordered_set<String> used_database_engines;
    std::unordered_set<String> used_data_type_families;
    std::unordered_set<String> used_dictionaries;
    std::unordered_set<String> used_formats;
    std::unordered_set<String> used_functions;
    std::unordered_set<String> used_storages;
    std::unordered_set<String> used_table_functions;
    std::unordered_set<String> used_executable_user_defined_functions;
    std::unordered_set<String> used_sql_user_defined_functions;
    std::set<String> used_row_policies;
    std::unordered_set<String> used_privileges;
    std::unordered_set<String> missing_privileges;

    Int32 exception_code{}; // because ErrorCodes are int
    String exception;
    String stack_trace;
    std::string_view exception_format_string{};
    std::vector<std::string> exception_format_string_args{};

    ClientInfo client_info;

    String log_comment;

    std::vector<UInt64> thread_ids;
    UInt64 peak_threads_usage = 0;
    std::shared_ptr<ProfileEvents::Counters::Snapshot> profile_counters;
    std::shared_ptr<AsyncReadCounters> async_read_counters;
    std::shared_ptr<Settings> query_settings;

    TransactionID tid;

    QueryResultCacheUsage query_result_cache_usage = QueryResultCacheUsage::Unknown;

    static std::string name() { return "QueryLog"; }

    static ColumnsDescription getColumnsDescription();
    static NamesAndAliases getNamesAndAliases();
    void appendToBlock(MutableColumns & columns) const;

    static void appendClientInfo(const ClientInfo & client_info, MutableColumns & columns, size_t & i);
};
}
