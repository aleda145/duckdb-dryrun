import { useEffect, useRef, useState } from "react";
import "./App.css";
import {
  DRYRUN_EXTENSION_VERSION,
  type DryrunRow,
  type EngineStatus,
  type NetworkTrafficEvent,
  extractParquetPaths,
  getDryrunEngine,
  runDryrun,
  subscribeNetworkTraffic,
} from "./dryrunEngine";

type Scenario = {
  id: string;
  title: string;
  summary: string;
  rows: string;
  fileSize: string;
  sql: string;
};

const GAIA_PATH = "https://dryrun-data.dahl.dev/gaia-5m.parquet";

const SCENARIOS: Scenario[] = [
  {
    id: "filter-b",
    title: "Filter column",
    summary: "b = 1",
    rows: "5M",
    fileSize: "93.2 MB",
    sql: `SELECT b
FROM '${GAIA_PATH}'
WHERE b = 1;`,
  },
  {
    id: "count-all",
    title: "Count rows",
    summary: "count(*)",
    rows: "5M",
    fileSize: "93.2 MB",
    sql: `SELECT count(*)
FROM '${GAIA_PATH}';`,
  },
  {
    id: "project-b",
    title: "Project one",
    summary: "SELECT b",
    rows: "5M",
    fileSize: "93.2 MB",
    sql: `SELECT b
FROM '${GAIA_PATH}';`,
  },
  {
    id: "full-scan",
    title: "Full scan",
    summary: "SELECT *",
    rows: "5M",
    fileSize: "93.2 MB",
    sql: `SELECT *
FROM '${GAIA_PATH}';`,
  },
];

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
  const [sql, setSql] = useState(SCENARIOS[0].sql);
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
  const [trafficEvents, setTrafficEvents] = useState<NetworkTrafficEvent[]>([]);
  const runId = useRef(0);

  const selectedScenario = SCENARIOS.find(
    (scenario) => scenario.id === selectedScenarioId,
  );
  const latestResult = query.result;

  useEffect(() => {
    let mounted = true;
    const unsubscribeTraffic = subscribeNetworkTraffic((event) => {
      setTrafficEvents((previous) => [...previous, event].slice(-8));
    });

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
      unsubscribeTraffic();
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
  }, [engine.ready, sql]);

  function selectScenario(scenario: Scenario) {
    setSelectedScenarioId(scenario.id);
    markQueryPending(scenario.sql);
    setSql(scenario.sql);
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

      <section className="query-section" aria-label="SQL dryrun call">
        <div className="duckdb-call">
          <div className="call-line">
            <code>SELECT * FROM dryrun(&apos;</code>
          </div>
          <textarea
            value={sql}
            onChange={(event) => {
              setSelectedScenarioId("custom");
              markQueryPending(event.target.value);
              setSql(event.target.value);
            }}
            spellCheck={false}
            aria-label="SQL string passed to dryrun"
          />
          <div className="call-line end">
            <code>&apos;);</code>
            <span className="live-indicator">
              {query.running ? "running" : engine.ready ? "live" : "waiting"}
            </span>
          </div>
        </div>
      </section>

      <section className="result-section" aria-label="Dryrun result">
        <div className="result-heading">
          <div>
            <h2>Dry run result</h2>
            <p>{selectedScenario?.title ?? "Custom SQL"}</p>
          </div>
          <span>{query.parquetPath ?? GAIA_PATH}</span>
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
          <ResultList result={latestResult} trafficEvents={trafficEvents} />
        ) : (
          <EmptyPanel engineReady={engine.ready} />
        )}
      </section>

      <footer className="footer">
        <span>built by Alex</span>
        <a href="https://dahl.dev">dahl.dev</a>
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
          <span className="scenario-kicker">Gaia 5M</span>
          <strong>{scenario.title}</strong>
        </span>
        <span className="scenario-facts">
          <span>{scenario.rows} rows</span>
          <span>{scenario.fileSize}</span>
        </span>
      </button>
    </article>
  );
}

function ResultList({
  result,
  trafficEvents,
}: {
  result: DryrunRow;
  trafficEvents: NetworkTrafficEvent[];
}) {
  return (
    <div className="result-list">
      <Metric
        label="estimated_compute_bytes"
        value={formatBytes(result.estimated_compute_bytes)}
      />
      <Metric
        label="rowgroups"
        value={formatNumber(result.estimated_row_groups)}
      />
      <Metric label="files" value={formatNumber(result.estimated_files)} />
      <Metric label="confidence" value={result.confidence ?? "unknown"} />
      <ParquetFooterReadMetric events={trafficEvents} />
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

function ParquetFooterReadMetric({ events }: { events: NetworkTrafficEvent[] }) {
  const latestGet = events.findLast((event) => event.method === "GET");

  return (
    <div className="metric result-row">
      <span>Parquet footer read</span>
      <strong>
        {latestGet
          ? formatBytes(latestGet.responseBytes)
          : "waiting for first Parquet footer request"}
      </strong>
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
