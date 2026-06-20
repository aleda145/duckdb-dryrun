import {
  AsyncDuckDB,
  ConsoleLogger,
  type AsyncDuckDBConnection,
} from '@duckdb/duckdb-wasm'
import ehWorker from '@duckdb/duckdb-wasm/dist/duckdb-browser-eh.worker.js?url'

const DUCKDB_WASM_VERSION = '1.33.1-dev45.0'
const DUCKDB_EH_WASM_URL = `https://cdn.jsdelivr.net/npm/@duckdb/duckdb-wasm@${DUCKDB_WASM_VERSION}/dist/duckdb-eh.wasm`
const DRYRUN_EXTENSION_VERSION = 'v1.5.1'
const DRYRUN_EXTENSION_PLATFORM = 'wasm_eh'

export type EngineStatus =
  | 'Preparing DuckDB-Wasm'
  | 'Loading Parquet support'
  | 'Loading dryrun extension'
  | 'Ready'

export type DryrunRow = {
  estimated_bytes: number | bigint | null
  estimated_metadata_bytes: number | bigint | null
  estimated_files: number | bigint | null
  estimated_row_groups: number | bigint | null
  total_row_groups: number | bigint | null
  confidence: string | null
  notes: string | null
}

export type ParquetMetadataRow = {
  file_name: string | null
  file_size_bytes: number | bigint | null
  num_rows: number | bigint | null
  num_row_groups: number | bigint | null
}

type DryrunEngine = {
  db: AsyncDuckDB
  conn: AsyncDuckDBConnection
  extensionRepository: string
}

let enginePromise: Promise<DryrunEngine> | null = null

export function getDryrunEngine(
  onStatus?: (status: EngineStatus) => void,
): Promise<DryrunEngine> {
  if (!enginePromise) {
    enginePromise = createDryrunEngine(onStatus)
  }
  return enginePromise
}

export async function runDryrun(sql: string): Promise<DryrunRow> {
  const { conn } = await getDryrunEngine()
  const table = await conn.query(
    `SELECT * FROM dryrun('${escapeSqlString(sql)}')`,
  )
  const rows = tableToRecords<DryrunRow>(table)
  if (!rows[0]) {
    throw new Error('dryrun returned no estimate rows')
  }
  return rows[0]
}

export async function readParquetMetadata(
  parquetPath: string,
): Promise<ParquetMetadataRow | null> {
  const { conn } = await getDryrunEngine()
  const table = await conn.query(`
    SELECT file_name, file_size_bytes, num_rows, num_row_groups
    FROM parquet_file_metadata('${escapeSqlString(parquetPath)}')
    LIMIT 1
  `)
  return tableToRecords<ParquetMetadataRow>(table)[0] ?? null
}

export function extractParquetPaths(sql: string): string[] {
  const paths: string[] = []

  for (let index = 0; index < sql.length; index += 1) {
    if (sql[index] !== "'") {
      continue
    }

    let literal = ''
    index += 1
    for (; index < sql.length; index += 1) {
      const char = sql[index]
      if (char === "'" && sql[index + 1] === "'") {
        literal += "'"
        index += 1
      } else if (char === "'") {
        break
      } else {
        literal += char
      }
    }

    if (literal.toLowerCase().includes('.parquet')) {
      paths.push(literal)
    }
  }

  return Array.from(new Set(paths))
}

export function escapeSqlString(value: string): string {
  return value.replaceAll("'", "''")
}

async function createDryrunEngine(
  onStatus?: (status: EngineStatus) => void,
): Promise<DryrunEngine> {
  onStatus?.('Preparing DuckDB-Wasm')

  const worker = createDuckDBWorker()
  const db = new AsyncDuckDB(new ConsoleLogger(), worker)

  await db.instantiate(DUCKDB_EH_WASM_URL)
  await db.open({
    allowUnsignedExtensions: true,
    filesystem: {
      allowFullHTTPReads: false,
      reliableHeadRequests: true,
    },
    query: {
      castBigIntToDouble: true,
    },
  })

  const conn = await db.connect()

  await conn.query('SET builtin_httpfs = false')
  await conn.query('LOAD httpfs')

  onStatus?.('Loading Parquet support')
  await conn.query('INSTALL parquet')
  await conn.query('LOAD parquet')

  const extensionRepository = buildExtensionRepositoryUrl()
  const extensionUrl = buildExtensionUrl(extensionRepository)
  onStatus?.('Loading dryrun extension')
  await assertWasmExtensionAvailable(extensionUrl)
  await conn.query(
    `SET custom_extension_repository='${escapeSqlString(extensionRepository)}'`,
  )
  await conn.query('INSTALL dryrun')
  await conn.query('LOAD dryrun')

  onStatus?.('Ready')

  return { db, conn, extensionRepository }
}

function createDuckDBWorker(): Worker {
  return new Worker(ehWorker)
}

function buildExtensionRepositoryUrl(): string {
  const basePath = import.meta.env.BASE_URL.endsWith('/')
    ? import.meta.env.BASE_URL
    : `${import.meta.env.BASE_URL}/`
  const url = new URL(`${basePath}extensions`, window.location.origin)
  return url.toString().replace(/\/$/, '')
}

function buildExtensionUrl(repositoryUrl: string): string {
  return `${repositoryUrl}/${DRYRUN_EXTENSION_VERSION}/${DRYRUN_EXTENSION_PLATFORM}/dryrun.duckdb_extension.wasm`
}

async function assertWasmExtensionAvailable(extensionUrl: string) {
  let response: Response
  try {
    response = await fetch(extensionUrl, {
      headers: {
        Range: 'bytes=0-3',
      },
    })
  } catch (error) {
    throw new Error(
      `Could not fetch dryrun WASM extension at ${extensionUrl}: ${normalizeError(error)}`,
      { cause: error },
    )
  }

  if (!response.ok && response.status !== 206) {
    throw new Error(
      `Missing dryrun WASM extension at ${extensionUrl} (${response.status} ${response.statusText}). Copy the matching dryrun.duckdb_extension.wasm artifact into web/public/extensions/${DRYRUN_EXTENSION_VERSION}/${DRYRUN_EXTENSION_PLATFORM}.`,
    )
  }

  const bytes = new Uint8Array(await response.arrayBuffer())
  const hasWasmMagic =
    bytes.length >= 4 &&
    bytes[0] === 0x00 &&
    bytes[1] === 0x61 &&
    bytes[2] === 0x73 &&
    bytes[3] === 0x6d

  if (!hasWasmMagic) {
    const preview = new TextDecoder().decode(bytes.slice(0, 80)).trim()
    const hint = preview.startsWith('<')
      ? 'The dev server returned HTML, which usually means the extension file is missing.'
      : 'The file is not raw WebAssembly. If it is Brotli/gzip-compressed, serve it with the matching Content-Encoding header or copy the raw .wasm side module.'
    throw new Error(
      `Invalid dryrun WASM extension at ${extensionUrl}. ${hint}`,
    )
  }
}

function tableToRecords<T>(table: { toArray(): unknown[] }): T[] {
  return table.toArray().map((row) => {
    if (row && typeof row === 'object' && 'toJSON' in row) {
      const maybeJson = row.toJSON
      if (typeof maybeJson === 'function') {
        return maybeJson.call(row) as T
      }
    }
    return row as T
  })
}

function normalizeError(error: unknown): string {
  if (error instanceof Error) {
    return error.message
  }
  return String(error)
}

export { DRYRUN_EXTENSION_VERSION }
