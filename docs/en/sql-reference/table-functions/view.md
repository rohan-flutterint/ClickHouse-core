---
sidebar_position: 51
sidebar_label: view
---

## view

Turns a subquery into a table. The function implements views (see [CREATE VIEW](https://clickhouse.com/docs/en/sql-reference/statements/create/view/#create-view)). The resulting table does not store data, but only stores the specified `SELECT` query. When reading from the table, ClickHouse executes the query and deletes all unnecessary columns from the result.

**Syntax**

``` sql
view(subquery)
```

**Arguments**

-   `subquery` — `SELECT` query.

**Returned value**

-   A table.

**Example**

Input table:

``` text
┌─id─┬─name─────┬─days─┐
│  1 │ January  │   31 │
│  2 │ February │   29 │
│  3 │ March    │   31 │
│  4 │ April    │   30 │
└────┴──────────┴──────┘
```

Query:

``` sql
SELECT * FROM view(SELECT name FROM months);
```

Result:

``` text
┌─name─────┐
│ January  │
│ February │
│ March    │
│ April    │
└──────────┘
```

You can use the `view` function as a parameter of the [remote](https://clickhouse.com/docs/en/sql-reference/table-functions/remote/#remote-remotesecure) and [cluster](https://clickhouse.com/docs/en/sql-reference/table-functions/cluster/#cluster-clusterallreplicas) table functions:

``` sql
SELECT * FROM remote(`127.0.0.1`, view(SELECT a, b, c FROM table_name));
```

``` sql
SELECT * FROM cluster(`cluster_name`, view(SELECT a, b, c FROM table_name));
```

Function `view` can be used to operate with the result of the `EXPLAIN SELECT` query.

Query:

``` sql
SELECT * FROM view(EXPLAIN SELECT * FROM t1) WHERE explain like '%ReadFromStorage%';
```

Result:

``` text
┌─explain─────────────────────┐
│   ReadFromStorage (TinyLog) │
└─────────────────────────────┘
```

**See Also**

-   [View Table Engine](https://clickhouse.com/docs/en/engines/table-engines/special/view/)
-   [EXPLAIN](../statements/explain.md)

[Original article](https://clickhouse.com/docs/en/sql-reference/table-functions/view/) <!--hide-->
