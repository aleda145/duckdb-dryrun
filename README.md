# DuckDB dryrun

`dryrun` estimates Parquet scan bytes for `SELECT` statements without executing the target query.

Example:

```sql
SELECT *
FROM dryrun(
    'SELECT * FROM ''https://dryrun-data.dahl.dev/gaia-5m.parquet'''
);
```

Result:

```
         estimated_bytes = 97722749
         estimated_files = 1
    estimated_row_groups = 1
        total_row_groups = 1
              confidence = high
                   notes =
estimated_metadata_bytes = 1137
```

See the DuckDB-Wasm demo at https://dryrun.dahl.dev/

## Development

This repository is based on the DuckDB extension template.

- `make` to build
- `make wasm` to build for Wasm, placed in `web/`
- `make test` to run tests
- `make benchmark` to run benchmarks

## Benchmarks

The default benchmarks run against remote Parquet files. Benchmarks are used to figure out where to focus next.

Current Results:

```
case                                | dryrun_bytes | profiled_bytes | profiled/dryrun | row_groups | confidence
------------------------------------+--------------+----------------+-----------------+------------+-----------
titanic_full                        | 38839        | 40009          | 1.030x          | 1/1        | high
titanic_PassengerId                 | 4357         | 20741          | 4.760x          | 1/1        | high
titanic_self_join                   | 4357         | 20741          | 4.760x          | 1/1        | low
properties_full                     | 109609741    | 109625381      | 1.000x          | 1/1        | high
properties_price                    | 2095766      | 2226838        | 1.063x          | 1/1        | high
house_prices_full                   | 195476002    | 195595234      | 1.001x          | 51/51      | high
house_prices_price_date_town        | 105197738    | 114255866      | 1.086x          | 51/51      | high
house_prices_price_gt_100m          | 47861839     | 48123983       | 1.005x          | 30/51      | medium
house_prices_count_price_gt_500m    | 3999028      | 4261172        | 1.066x          | 2/51       | medium
house_prices_york_avg_price_by_date | 9159818      | 6958299        | 0.760x          | 4/51       | medium
gaia_full                           | 97722749     | 97724404       | 1.000x          | 1/1        | high
gaia_count                          | 0            | missing        | n/a             | 1/1        | high
gaia_b                              | 20626142     | 20757214       | 1.006x          | 1/1        | high
gaia_multi_column_expr              | 20626142     | 20757214       | 1.006x          | 1/1        | high
gaia_count_b_eq_1                   | 20626142     | 20757214       | 1.006x          | 1/1        | medium
gaia_abs_b_eq_1                     | 20626142     | 20757214       | 1.006x          | 1/1        | low
gaia_union_b_filters                | 20626142     | 20757214       | 1.006x          | 1/1        | low
yellow_2022_full_list               | 614948371    | 615099615      | 1.000x          | 12/12      | high
yellow_2022_multi_columns           | 111582985    | 112370869      | 1.007x          | 12/12      | high
yellow_2022_pulocation_filter       | 74691666     | 75478098       | 1.011x          | 12/12      | medium
yellow_2022_abs_pulocation_filter   | 74691666     | 75478098       | 1.011x          | 12/12      | low
yellow_2022_jan_feb_list_filter     | 6290371      | 6421443        | 1.021x          | 2/2        | medium
yellow_2022_http_glob_count         | error        | error          | n/a             | error      | error
yellow_2022_union_months            | 692143       | 823215         | 1.189x          | 2/2        | low
yellow_zone_left_count_keys_only    | 1912834      | 1983220        | 1.037x          | 2/2        | low
yellow_zone_left_payload            | 17598556     | 17666116       | 1.004x          | 2/2        | low
yellow_zone_pickup_dropoff_left     | 20043771     | 20489601       | 1.022x          | 2/2        | low
yellow_zone_fact_filter             | 4407843      | 4475552        | 1.015x          | 2/2        | medium
yellow_zone_dim_filter              | 4408096      | 4475552        | 1.015x          | 2/2        | medium
yellow_zone_group_by_borough        | 4405419      | 4475552        | 1.016x          | 2/2        | low
```

## Road Map

Currently Parquet file scans are supported. Estimates are most useful for remote files, where scan bytes map directly to network I/O. Local Parquet files are supported, but local filesystem caching and OS reads make profiled byte comparisons less direct.

Future work includes DuckLake, local DuckDB table scans, and better local Parquet profiling.

## Contributions

Welcome!
