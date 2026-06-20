import { useEffect, useLayoutEffect, useRef, useState } from "react";
import "./App.css";
import {
  DRYRUN_EXTENSION_VERSION,
  type DryrunRow,
  type EngineStatus,
  extractParquetPaths,
  getDryrunEngine,
  runDryrun,
} from "./dryrunEngine";

type Scenario = {
  id: string;
  title: string;
  primaryFact: string;
  secondaryFact: string;
  sql: string;
  schemas: SchemaSource[];
};

type SchemaColumn = {
  name: string;
  type: string;
};

type SchemaSource = {
  title: string;
  columns: SchemaColumn[];
};

const GAIA_PATH = "https://dryrun-data.dahl.dev/gaia-5m.parquet";
const HOUSE_PRICES_PATH =
  "https://dryrun-data.dahl.dev/house_prices_all.parquet";
const YELLOW_2022_PREFIX = "https://dryrun-data.dahl.dev/yellow_tripdata_2022";
const TAXI_ZONES_PATH = "https://dryrun-data.dahl.dev/taxi_zone_lookup.parquet";
const YELLOW_2022_PATHS = Array.from(
  { length: 12 },
  (_, index) =>
    `${YELLOW_2022_PREFIX}-${String(index + 1).padStart(2, "0")}.parquet`,
);
const YELLOW_2022_SQL_LIST = `[${YELLOW_2022_PATHS.map(
  (path) => `'${path}'`,
).join(", ")}]`;

const SCENARIOS: Scenario[] = [
  {
    id: "gaia-filter",
    title: "Gaia filter",
    primaryFact: "1 file",
    secondaryFact: "93.2 MB",
    sql: `SELECT l, b, parallax,
FROM '${GAIA_PATH}'`,
    schemas: [
      {
        title: GAIA_PATH,
        columns: [
          { name: "l", type: "float" },
          { name: "b", type: "float" },
          { name: "parallax", type: "float" },
          { name: "phot_g_mean_mag", type: "float" },
          { name: "bp_rp", type: "float" },
        ],
      },
    ],
  },
  {
    id: "house-prices-rowgroups",
    title: "House prices pruning",
    primaryFact: "1 file",
    secondaryFact: "195.6 MB",
    sql: `SELECT count(*)
FROM '${HOUSE_PRICES_PATH}'
WHERE price > 500000000`,
    schemas: [
      {
        title: HOUSE_PRICES_PATH,
        columns: [
          { name: "price", type: "uinteger" },
          { name: "date", type: "usmallint" },
          { name: "postcode1", type: "blob" },
          { name: "postcode2", type: "blob" },
          { name: "type", type: "tinyint" },
          { name: "is_new", type: "utinyint" },
          { name: "duration", type: "tinyint" },
          { name: "addr1", type: "blob" },
          { name: "addr2", type: "blob" },
          { name: "street", type: "blob" },
          { name: "locality", type: "blob" },
          { name: "town", type: "blob" },
          { name: "district", type: "blob" },
          { name: "county", type: "blob" },
        ],
      },
    ],
  },
  {
    id: "nyc-taxi-2022",
    title: "NYC taxi 2022",
    primaryFact: "12 files",
    secondaryFact: "615.11 MB",
    sql: `SELECT VendorID, passenger_count, trip_distance, fare_amount
FROM read_parquet(${YELLOW_2022_SQL_LIST})`,
    schemas: [
      {
        title: `${YELLOW_2022_PREFIX}-*.parquet`,
        columns: yellowTripdataColumns(),
      },
    ],
  },
  {
    id: "nyc-taxi-zone-join",
    title: "NYC taxi zone join",
    primaryFact: "2 files",
    secondaryFact: "38.15 MB",
    sql: `SELECT t.fare_amount, z.Zone
FROM '${YELLOW_2022_PATHS[0]}' t
JOIN '${TAXI_ZONES_PATH}' z
ON t.PULocationID = z.LocationID
WHERE z.Borough = 'Manhattan'`,
    schemas: [
      {
        title: YELLOW_2022_PATHS[0],
        columns: yellowTripdataColumns(),
      },
      {
        title: TAXI_ZONES_PATH,
        columns: [
          { name: "LocationID", type: "bigint" },
          { name: "Borough", type: "varchar" },
          { name: "Zone", type: "varchar" },
          { name: "service_zone", type: "varchar" },
        ],
      },
    ],
  },
];

function yellowTripdataColumns(): SchemaColumn[] {
  return [
    { name: "VendorID", type: "bigint" },
    { name: "tpep_pickup_datetime", type: "timestamp" },
    { name: "tpep_dropoff_datetime", type: "timestamp" },
    { name: "passenger_count", type: "double" },
    { name: "trip_distance", type: "double" },
    { name: "RatecodeID", type: "double" },
    { name: "store_and_fwd_flag", type: "varchar" },
    { name: "PULocationID", type: "bigint" },
    { name: "DOLocationID", type: "bigint" },
    { name: "payment_type", type: "bigint" },
    { name: "fare_amount", type: "double" },
    { name: "extra", type: "double" },
    { name: "mta_tax", type: "double" },
    { name: "tip_amount", type: "double" },
    { name: "tolls_amount", type: "double" },
    { name: "improvement_surcharge", type: "double" },
    { name: "total_amount", type: "double" },
    { name: "congestion_surcharge", type: "double" },
    { name: "airport_fee", type: "double" },
  ];
}

type EngineState = {
  ready: boolean;
  status: EngineStatus | "Starting" | "Failed";
  detail: string;
};

type QueryState = {
  running: boolean;
  error: string | null;
  result: DryrunRow | null;
  parquetPath: string | null;
};

function App() {
  const [selectedScenarioId, setSelectedScenarioId] = useState(SCENARIOS[0].id);
  const [schemaOpen, setSchemaOpen] = useState(false);
  const [sql, setSql] = useState(SCENARIOS[0].sql);
  const [scenarioRun, setScenarioRun] = useState(0);
  const [engine, setEngine] = useState<EngineState>({
    ready: false,
    status: "Starting",
    detail: "The page is ready while DuckDB-Wasm starts.",
  });
  const [query, setQuery] = useState<QueryState>({
    running: false,
    error: null,
    result: null,
    parquetPath: null,
  });
  const runId = useRef(0);
  const textareaRef = useRef<HTMLTextAreaElement | null>(null);

  const selectedScenario = SCENARIOS.find(
    (scenario) => scenario.id === selectedScenarioId,
  );
  const latestResult = query.result;
  const displayParquetPath =
    query.parquetPath ?? extractParquetPaths(sql)[0] ?? GAIA_PATH;

  useLayoutEffect(() => {
    const textarea = textareaRef.current;
    if (!textarea) {
      return;
    }

    textarea.style.height = "auto";
    textarea.style.height = `${textarea.scrollHeight}px`;
  }, [sql]);

  useEffect(() => {
    let mounted = true;

    getDryrunEngine((status) => {
      if (!mounted) {
        return;
      }
      setEngine({
        ready: status === "Ready",
        status,
        detail: statusDetail(status),
      });
    })
      .then(({ extensionRepository }) => {
        if (!mounted) {
          return;
        }
        setEngine({
          ready: true,
          status: "Ready",
          detail: `Extension repository: ${extensionRepository}`,
        });
      })
      .catch((error: unknown) => {
        if (!mounted) {
          return;
        }
        setEngine({
          ready: false,
          status: "Failed",
          detail: normalizeError(error),
        });
      });

    return () => {
      mounted = false;
    };
  }, []);

  useEffect(() => {
    const currentRun = runId.current + 1;
    runId.current = currentRun;

    if (!engine.ready) {
      return;
    }

    markQueryPending(sql);

    const timer = window.setTimeout(() => {
      void executeDryrun(sql, currentRun);
    }, 400);

    return () => {
      window.clearTimeout(timer);
    };
  }, [engine.ready, sql, scenarioRun]);

  function selectScenario(scenario: Scenario) {
    setSelectedScenarioId(scenario.id);
    markQueryPending(scenario.sql);
    setSql(scenario.sql);
    setScenarioRun((previous) => previous + 1);
  }

  function markQueryPending(nextSql: string) {
    setQuery({
      running: true,
      error: null,
      result: null,
      parquetPath: extractParquetPaths(nextSql)[0] ?? null,
    });
  }

  async function executeDryrun(nextSql: string, currentRun: number) {
    if (!nextSql.trim()) {
      setQuery((previous) => ({
        ...previous,
        running: false,
        error: "Write a SELECT query that references a Parquet file.",
      }));
      return;
    }

    setQuery((previous) => ({
      ...previous,
      running: true,
      error: null,
    }));

    try {
      const nextResult = await runDryrun(nextSql);
      const nextPath = extractParquetPaths(nextSql)[0] ?? null;

      if (runId.current !== currentRun) {
        return;
      }

      setQuery({
        running: false,
        error: null,
        result: nextResult,
        parquetPath: nextPath,
      });
    } catch (error: unknown) {
      if (runId.current !== currentRun) {
        return;
      }

      setQuery((previous) => ({
        ...previous,
        running: false,
        error: normalizeError(error),
      }));
    }
  }

  return (
    <main className="app-shell">
      <header className="hero">
        <h1>duckdb dryrun</h1>
        <p>
          Estimate Parquet scan bytes before running a query.{" "}
          <a
            href="https://github.com/aleda145/duckdb-dryrun/"
            target="_blank"
            rel="noreferrer"
          >
            GitHub
          </a>
        </p>
      </header>

      <section className="scenario-section" aria-label="Gaia dryrun scenarios">
        <div className="scenario-grid">
          {SCENARIOS.map((scenario) => (
            <ScenarioCard
              scenario={scenario}
              key={scenario.id}
              active={scenario.id === selectedScenarioId}
              onSelect={() => selectScenario(scenario)}
            />
          ))}
        </div>
      </section>

      {selectedScenario ? (
        <section className="schema-section" aria-label="Selected example schema">
          <button
            type="button"
            className="schema-button"
            onClick={() => setSchemaOpen((previous) => !previous)}
            aria-expanded={schemaOpen}
          >
            {schemaOpen ? "Hide Schema" : "Schema"}
          </button>
          {schemaOpen ? <SchemaPanel scenario={selectedScenario} /> : null}
        </section>
      ) : null}

      <section className="query-section" aria-label="SQL dryrun call">
        <div className="duckdb-call">
          <div className="call-line call-wrapper">
            <code>SELECT * FROM dryrun(&apos;</code>
          </div>
          <textarea
            className="sql-payload"
            ref={textareaRef}
            value={sql}
            onChange={(event) => {
              setSelectedScenarioId("custom");
              markQueryPending(event.target.value);
              setSql(event.target.value);
            }}
            spellCheck={false}
            aria-label="SQL string passed to dryrun"
          />
          <div className="call-line call-wrapper end">
            <code>&apos;);</code>
          </div>
        </div>
      </section>

      <section className="result-section" aria-label="Dryrun result">
        <div className="result-heading">
          <div>
            <h2>Dry run result</h2>
            <p>{selectedScenario?.title ?? "Custom SQL"}</p>
          </div>
          <span>{displayParquetPath}</span>
        </div>

        {!engine.ready ? (
          <LoadingPanel
            title="Loading DuckDB-Wasm"
            detail={`Loading dryrun ${DRYRUN_EXTENSION_VERSION}.`}
          />
        ) : query.running ? (
          <LoadingPanel
            title="Running dryrun"
            detail="Estimating the selected query."
          />
        ) : query.error ? (
          <ErrorPanel message={query.error} />
        ) : latestResult ? (
          <ResultList result={latestResult} />
        ) : (
          <EmptyPanel engineReady={engine.ready} />
        )}
      </section>

      <footer className="footer">
        <a href="https://dahl.dev">Built by Alex</a>
      </footer>
    </main>
  );
}

function ScenarioCard({
  scenario,
  active,
  onSelect,
}: {
  scenario: Scenario;
  active: boolean;
  onSelect: () => void;
}) {
  return (
    <article className={active ? "scenario-card active" : "scenario-card"}>
      <button type="button" className="scenario-button" onClick={onSelect}>
        <span className="scenario-copy">
          <strong>{scenario.title}</strong>
        </span>
        <span className="scenario-facts">
          <span>{scenario.primaryFact}</span>
          <span>{scenario.secondaryFact}</span>
        </span>
      </button>
    </article>
  );
}

function SchemaPanel({ scenario }: { scenario: Scenario }) {
  return (
    <div className="schema-panel">
      <div className="schema-heading">
        <strong>{scenario.title} schema</strong>
        <span>{scenario.schemas.length === 1 ? "1 source" : `${scenario.schemas.length} sources`}</span>
      </div>
      <div className="schema-sources">
        {scenario.schemas.map((source) => (
          <section className="schema-source" key={source.title}>
            <h2>{source.title}</h2>
            <div className="schema-columns">
              {source.columns.map((column) => (
                <div className="schema-column" key={column.name}>
                  <code>{column.name}</code>
                  <span>{column.type}</span>
                </div>
              ))}
            </div>
          </section>
        ))}
      </div>
    </div>
  );
}

function ResultList({ result }: { result: DryrunRow }) {
  return (
    <div className="result-list">
      <Metric
        label="estimated_bytes"
        value={formatBytes(result.estimated_bytes)}
      />
      <Metric
        label="rowgroups"
        value={`${formatNumber(result.estimated_row_groups)} / ${formatNumber(result.total_row_groups)}`}
      />
      <Metric label="files" value={formatNumber(result.estimated_files)} />
      <Metric label="confidence" value={result.confidence ?? "unknown"} />
      <Metric
        label="Parquet footer read"
        value={formatBytes(result.estimated_metadata_bytes)}
      />
    </div>
  );
}

function Metric({ label, value }: { label: string; value: string }) {
  return (
    <div className="metric">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function ErrorPanel({ message }: { message: string }) {
  return (
    <div className="message-panel error" role="alert">
      <strong>Current SQL did not dryrun</strong>
      <p>{message}</p>
    </div>
  );
}

function LoadingPanel({ title, detail }: { title: string; detail: string }) {
  return (
    <div className="message-panel loading" role="status" aria-live="polite">
      <span className="spinner" aria-hidden="true" />
      <div>
        <strong>{title}</strong>
        <p>{detail}</p>
      </div>
    </div>
  );
}

function EmptyPanel({ engineReady }: { engineReady: boolean }) {
  return (
    <div className="message-panel">
      <strong>
        {engineReady ? "Waiting for valid SQL" : "Engine loading"}
      </strong>
      <p>
        {engineReady
          ? "Choose an example query or edit the SQL string."
          : `DuckDB-Wasm is loading dryrun ${DRYRUN_EXTENSION_VERSION}.`}
      </p>
    </div>
  );
}

function statusDetail(status: EngineStatus): string {
  if (status === "Preparing DuckDB-Wasm") {
    return "Starting the browser runtime.";
  }
  if (status === "Loading Parquet support") {
    return "Loading Parquet metadata functions.";
  }
  if (status === "Loading dryrun extension") {
    return `Fetching dryrun ${DRYRUN_EXTENSION_VERSION}.`;
  }
  return "Ready.";
}

function formatBytes(value: number | bigint | null | undefined): string {
  if (value === undefined) {
    return "loading";
  }

  const numeric = Number(value ?? 0);
  if (!Number.isFinite(numeric) || numeric <= 0) {
    return "0 B";
  }

  const units = ["B", "KB", "MB", "GB", "TB"];
  let size = numeric;
  let unitIndex = 0;
  while (size >= 1024 && unitIndex < units.length - 1) {
    size /= 1024;
    unitIndex += 1;
  }

  const formatted =
    size >= 100 || unitIndex === 0 ? size.toFixed(0) : size.toFixed(1);
  return `${formatted} ${units[unitIndex]}`;
}

function formatNumber(value: number | bigint | null | undefined): string {
  if (value === undefined) {
    return "loading";
  }

  const numeric = Number(value ?? 0);
  if (!Number.isFinite(numeric)) {
    return "0";
  }
  return new Intl.NumberFormat("en-US").format(numeric);
}

function normalizeError(error: unknown): string {
  if (error instanceof Error) {
    return error.message;
  }
  return String(error);
}

export default App;
