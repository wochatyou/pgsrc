### check-postgresql 监控的指标：
```
our $action_info = {
 # Name                 # clusterwide? # helpstring
 archive_ready       => [1, 'Check the number of WAL files ready in the pg_xlog/archive_status'],
 autovac_freeze      => [1, 'Checks how close databases are to autovacuum_freeze_max_age.'],
 backends            => [1, 'Number of connections, compared to max_connections.'],
 bloat               => [0, 'Check for table and index bloat.'],
 checkpoint          => [1, 'Checks how long since the last checkpoint'],
 cluster_id          => [1, 'Checks the Database System Identifier'],
 commitratio         => [0, 'Report if the commit ratio of a database is too low.'],
 connection          => [0, 'Simple connection check.'],
 custom_query        => [0, 'Run a custom query.'],
 database_size       => [0, 'Report if a database is too big.'],
 dbstats             => [1, 'Returns stats from pg_stat_database: Cacti output only'],
 disabled_triggers   => [0, 'Check if any triggers are disabled'],
 disk_space          => [1, 'Checks space of local disks Postgres is using.'],
 fsm_pages           => [1, 'Checks percentage of pages used in free space map.'],
 fsm_relations       => [1, 'Checks percentage of relations used in free space map.'],
 hitratio            => [0, 'Report if the hit ratio of a database is too low.'],
 hot_standby_delay   => [1, 'Check the replication delay in hot standby setup'],
 relation_size       => [0, 'Checks the size of tables and indexes.'],
 index_size          => [0, 'Checks the size of indexes.'],
 table_size          => [0, 'Checks the size of tables (including TOAST).'],
 indexes_size        => [0, 'Checks the size of indexes on tables.'],
 total_relation_size => [0, 'Checks the size of tables (including indexes and TOAST).'],
 last_analyze        => [0, 'Check the maximum time in seconds since any one table has been analyzed.'],
 last_vacuum         => [0, 'Check the maximum time in seconds since any one table has been vacuumed.'],
 last_autoanalyze    => [0, 'Check the maximum time in seconds since any one table has been autoanalyzed.'],
 last_autovacuum     => [0, 'Check the maximum time in seconds since any one table has been autovacuumed.'],
 listener            => [0, 'Checks for specific listeners.'],
 locks               => [0, 'Checks the number of locks.'],
 logfile             => [1, 'Checks that the logfile is being written to correctly.'],
 new_version_bc      => [0, 'Checks if a newer version of Bucardo is available.'],
 new_version_box     => [0, 'Checks if a newer version of boxinfo is available.'],
 new_version_cp      => [0, 'Checks if a newer version of check_postgres.pl is available.'],
 new_version_pg      => [0, 'Checks if a newer version of Postgres is available.'],
 new_version_tnm     => [0, 'Checks if a newer version of tail_n_mail is available.'],
 partman_premake     => [1, 'Checks if premake partitions are present.'],
 pgb_pool_cl_active  => [1, 'Check the number of active clients in each pgbouncer pool.'],
 pgb_pool_cl_waiting => [1, 'Check the number of waiting clients in each pgbouncer pool.'],
 pgb_pool_sv_active  => [1, 'Check the number of active server connections in each pgbouncer pool.'],
 pgb_pool_sv_idle    => [1, 'Check the number of idle server connections in each pgbouncer pool.'],
 pgb_pool_sv_used    => [1, 'Check the number of used server connections in each pgbouncer pool.'],
 pgb_pool_sv_tested  => [1, 'Check the number of tested server connections in each pgbouncer pool.'],
 pgb_pool_sv_login   => [1, 'Check the number of login server connections in each pgbouncer pool.'],
 pgb_pool_maxwait    => [1, 'Check the current maximum wait time for client connections in pgbouncer pools.'],
 pgbouncer_backends  => [0, 'Check how many clients are connected to pgbouncer compared to max_client_conn.'],
 pgbouncer_checksum  => [0, 'Check that no pgbouncer settings have changed since the last check.'],
 pgbouncer_maxwait   => [0, 'Check how long the first (oldest) client in queue has been waiting.'],
 pgagent_jobs        => [0, 'Check for no failed pgAgent jobs within a specified period of time.'],
 prepared_txns       => [1, 'Checks number and age of prepared transactions.'],
 query_runtime       => [0, 'Check how long a specific query takes to run.'],
 query_time          => [1, 'Checks the maximum running time of current queries.'],
 replicate_row       => [0, 'Verify a simple update gets replicated to another server.'],
 replication_slots   => [1, 'Check the replication delay for replication slots'],
 same_schema         => [0, 'Verify that two databases have the exact same tables, columns, etc.'],
 sequence            => [0, 'Checks remaining calls left in sequences.'],
 settings_checksum   => [0, 'Check that no settings have changed since the last check.'],
 slony_status        => [1, 'Ensure Slony is up to date via sl_status.'],
 timesync            => [0, 'Compare database time to local system time.'],
 txn_idle            => [1, 'Checks the maximum "idle in transaction" time.'],
 txn_time            => [1, 'Checks the maximum open transaction time.'],
 txn_wraparound      => [1, 'See how close databases are getting to transaction ID wraparound.'],
 version             => [1, 'Check for proper Postgres version.'],
 wal_files           => [1, 'Check the number of WAL files in the pg_xlog directory'],

};

检查PostgreSQL的版本：
postgres=# SELECT setting FROM pg_settings WHERE name = 'server_version';
 setting 
---------
 16.1
(1 row)

SELECT freez, txns, ROUND(100*(txns/freez::float)) AS perc, datname
FROM (SELECT foo.freez::int, age(datfrozenxid) AS txns, datname FROM pg_database d JOIN (SELECT setting AS freez FROM pg_settings WHERE name = 'autovacuum_freeze_max_age') AS foo
ON (true) WHERE d.datallowconn) AS foo2 ORDER BY 3 DESC, 4 ASC;

-- 这条命令获得autovacuum_freeze_max_age参数的值, 这个参数是必须vacuum的最大年龄
postgres=# SELECT setting AS freez FROM pg_settings WHERE name = 'autovacuum_freeze_max_age';
   freez   
-----------
 200000000
(1 row)

postgres=# SELECT age(datfrozenxid) AS txns, datname FROM pg_database;
 txns |  datname  
------+-----------
   23 | postgres
   23 | oracle
   23 | template1
   23 | template0
(4 rows)
关键是理解datfrozenxid。官方文档：
All transaction IDs before this one have been replaced with a permanent (“frozen”) transaction ID in this database. This is used to track whether the database needs to be vacuumed in order to prevent transaction ID wraparound or to allow pg_xact to be shrunk. It is the minimum of the per-table pg_class.relfrozenxid values.

在本数据库中，任何在datfrozenxid之前的事务均被冻结了。它是pg_class.relfrozenxid的最小值。 取它的年龄和autovacuum_freeze_max_age对比，如果超过了autovacuum_freeze_max_age的多少百分比就要报警

```
检查bloat的查询：
```
SELECT
  current_database() AS db, schemaname, tablename, reltuples::bigint AS tups, relpages::bigint AS pages, otta,
  ROUND(CASE WHEN otta=0 OR sml.relpages=0 OR sml.relpages=otta THEN 0.0 ELSE sml.relpages/otta::numeric END,1) AS tbloat,
  CASE WHEN relpages < otta THEN 0 ELSE relpages::bigint - otta END AS wastedpages,
  CASE WHEN relpages < otta THEN 0 ELSE bs*(sml.relpages-otta)::bigint END AS wastedbytes,
  CASE WHEN relpages < otta THEN '0 bytes'::text ELSE (bs*(relpages-otta))::bigint::text || ' bytes' END AS wastedsize,
  iname, ituples::bigint AS itups, ipages::bigint AS ipages, iotta,
  ROUND(CASE WHEN iotta=0 OR ipages=0 OR ipages=iotta THEN 0.0 ELSE ipages/iotta::numeric END,1) AS ibloat,
  CASE WHEN ipages < iotta THEN 0 ELSE ipages::bigint - iotta END AS wastedipages,
  CASE WHEN ipages < iotta THEN 0 ELSE bs*(ipages-iotta) END AS wastedibytes,
  CASE WHEN ipages < iotta THEN '0 bytes' ELSE (bs*(ipages-iotta))::bigint::text || ' bytes' END AS wastedisize,
  CASE WHEN relpages < otta THEN
    CASE WHEN ipages < iotta THEN 0 ELSE bs*(ipages-iotta::bigint) END
    ELSE CASE WHEN ipages < iotta THEN bs*(relpages-otta::bigint)
      ELSE bs*(relpages-otta::bigint + ipages-iotta::bigint) END
  END AS totalwastedbytes
FROM (
  SELECT
    nn.nspname AS schemaname,
    cc.relname AS tablename,
    COALESCE(cc.reltuples,0) AS reltuples,
    COALESCE(cc.relpages,0) AS relpages,
    COALESCE(bs,0) AS bs,
    COALESCE(CEIL((cc.reltuples*((datahdr+ma-
      (CASE WHEN datahdr%ma=0 THEN ma ELSE datahdr%ma END))+nullhdr2+4))/(bs-20::float)),0) AS otta,
    COALESCE(c2.relname,'?') AS iname, COALESCE(c2.reltuples,0) AS ituples, COALESCE(c2.relpages,0) AS ipages,
    COALESCE(CEIL((c2.reltuples*(datahdr-12))/(bs-20::float)),0) AS iotta -- very rough approximation, assumes all cols
  FROM
     pg_class cc
  JOIN pg_namespace nn ON cc.relnamespace = nn.oid AND nn.nspname <> 'information_schema'
  LEFT JOIN
  (
    SELECT
      ma,bs,foo.nspname,foo.relname,
      (datawidth+(hdr+ma-(case when hdr%ma=0 THEN ma ELSE hdr%ma END)))::numeric AS datahdr,
      (maxfracsum*(nullhdr+ma-(case when nullhdr%ma=0 THEN ma ELSE nullhdr%ma END))) AS nullhdr2
    FROM (
      SELECT
        ns.nspname, tbl.relname, hdr, ma, bs,
        SUM((1-coalesce(null_frac,0))*coalesce(avg_width, 2048)) AS datawidth,
        MAX(coalesce(null_frac,0)) AS maxfracsum,
        hdr+(
          SELECT 1+count(*)/8
          FROM pg_stats s2
          WHERE null_frac<>0 AND s2.schemaname = ns.nspname AND s2.tablename = tbl.relname
        ) AS nullhdr
      FROM pg_attribute att
      JOIN pg_class tbl ON att.attrelid = tbl.oid
      JOIN pg_namespace ns ON ns.oid = tbl.relnamespace
      LEFT JOIN pg_stats s ON s.schemaname=ns.nspname
      AND s.tablename = tbl.relname
      AND s.inherited=false
      AND s.attname=att.attname,
      (
        SELECT
          (SELECT current_setting('block_size')::numeric) AS bs,
            CASE WHEN SUBSTRING(SPLIT_PART(v, ' ', 2) FROM '#"[0-9]+.[0-9]+#"%' for '#')
              IN ('8.0','8.1','8.2') THEN 27 ELSE 23 END AS hdr,
          CASE WHEN v ~ 'mingw32' OR v ~ '64-bit' THEN 8 ELSE 4 END AS ma
        FROM (SELECT version() AS v) AS foo
      ) AS constants
      WHERE att.attnum > 0 AND tbl.relkind='r'
      GROUP BY 1,2,3,4,5
    ) AS foo
  ) AS rs
  ON cc.relname = rs.relname AND nn.nspname = rs.nspname
  LEFT JOIN pg_index i ON indrelid = cc.oid
  LEFT JOIN pg_class c2 ON c2.oid = i.indexrelid
) AS sml;
```
