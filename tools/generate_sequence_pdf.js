const fs = require("fs");
const path = require("path");

const outputPath = path.resolve(process.cwd(), "docs", "code-flow-sequence-diagram.pdf");

const pages = [
  [
    "WEEK8 TEAM5 DB API CODE FLOW",
    "",
    "This PDF summarizes which entry point runs, how the server starts,",
    "and how one request travels across our files.",
    "",
    "1. ENTRY POINTS",
    " - src/app/main.c               : local SQL CLI program",
    " - src/app/sqlapi_server_main.c: API server entry point",
    "",
    "When we run ./build/bin/sqlapi_server, the active main() is",
    "src/app/sqlapi_server_main.c, not src/app/main.c.",
    "",
    "2. BIG PICTURE",
    "",
    "Client",
    "  -> sqlapi_server_main.c",
    "     -> server.c",
    "        -> accept thread",
    "           -> task_queue.c",
    "        -> worker_pool.c",
    "           -> worker thread",
    "              -> http/router.c",
    "                 -> api/health_handler.c",
    "                 -> api/query_handler.c",
    "                    -> service/db_service.c",
    "                       -> engine/sql_engine_adapter.c",
    "                          -> engine/engine_lock_manager.c",
    "                          -> parser / executor / storage / index",
    "",
    "Key idea:",
    "The app layer starts the runtime, server.c orchestrates threads and sockets,",
    "and the engine layer reuses the existing SQL processor.",
  ],
  [
    "3. SERVER START SEQUENCE",
    "",
    "User",
    "  -> ./build/bin/sqlapi_server --host ... --port ...",
    "",
    "sqlapi_server_main.c",
    "  -> sqlapi_server_config_set_defaults()",
    "  -> parse argv",
    "  -> sqlapi_server_create(server.c)",
    "",
    "server.c create stage",
    "  -> validate config",
    "  -> init state_mutex",
    "  -> init task queue",
    "  -> init table index registry",
    "  -> init engine lock manager",
    "  -> connect DbService adapter config",
    "",
    "sqlapi_server_main.c",
    "  -> sqlapi_server_start(server.c)",
    "",
    "server.c start stage",
    "  -> network init",
    "  -> open listen socket",
    "  -> start worker pool",
    "  -> create accept thread",
    "",
    "sqlapi_server_main.c",
    "  -> sqlapi_server_wait(server.c)",
    "  -> wait until accept thread and workers stop",
    "",
    "4. GET /health SEQUENCE",
    "",
    "Client -> accept thread -> task queue -> worker thread",
    "worker thread -> http_read_request() -> router.c",
    "router.c -> api_handle_health(health_handler.c)",
    "health_handler.c -> build JSON with worker_count and queue_depth",
    "worker thread -> http_response_send() -> close socket",
  ],
  [
    "5. POST /query SEQUENCE",
    "",
    "Client -> accept thread -> task queue -> worker thread",
    "worker thread -> http_read_request() -> router.c",
    "router.c -> api_handle_query(query_handler.c)",
    "",
    "query_handler.c",
    "  -> check Content-Length",
    "  -> check Content-Type",
    "  -> parse JSON body",
    "  -> extract sql field",
    "  -> db_service_execute_sql(db_service.c)",
    "",
    "db_service.c",
    "  -> sql_engine_adapter_execute(sql_engine_adapter.c)",
    "",
    "sql_engine_adapter.c",
    "  -> lex_sql()",
    "  -> parse_statement()",
    "  -> load_schema()",
    "  -> engine_lock_manager_acquire()",
    "  -> execute_statement()",
    "  -> engine_lock_manager_release()",
    "  -> return result",
    "",
    "6. student INSERT LOCK FLOW",
    "",
    "accept thread",
    "  -> queue mutex lock",
    "  -> push request to queue",
    "  -> queue mutex unlock",
    "",
    "worker thread",
    "  -> queue mutex lock",
    "  -> pop request from queue",
    "  -> queue mutex unlock",
    "",
    "sql_engine_adapter.c",
    "  -> detect table name: student",
    "  -> ask engine_lock_manager for student mutex",
    "",
    "engine_lock_manager.c",
    "  -> manager mutex lock",
    "  -> find/create student entry",
    "  -> manager mutex unlock",
    "  -> student table mutex lock",
    "",
    "sql_engine_adapter.c",
    "  -> execute INSERT safely",
    "  -> release student table mutex",
  ],
];

function escapePdfText(text) {
  return text.replace(/\\/g, "\\\\").replace(/\(/g, "\\(").replace(/\)/g, "\\)");
}

function buildContent(lines) {
  const fontSize = 11;
  const leading = 14;
  const startX = 50;
  const startY = 760;
  const body = [];
  body.push("BT");
  body.push(`/F1 ${fontSize} Tf`);
  body.push(`${leading} TL`);
  body.push(`${startX} ${startY} Td`);
  for (let i = 0; i < lines.length; i++) {
    body.push(`(${escapePdfText(lines[i])}) Tj`);
    if (i < lines.length - 1) {
      body.push("T*");
    }
  }
  body.push("ET");
  return body.join("\n");
}

function makePdf(pageLines) {
  const objects = [];

  function addObject(str) {
    objects.push(str);
    return objects.length;
  }

  const fontId = addObject("<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>");
  const contentIds = [];
  for (const lines of pageLines) {
    const content = buildContent(lines);
    const stream = `<< /Length ${Buffer.byteLength(content, "utf8")} >>\nstream\n${content}\nendstream`;
    contentIds.push(addObject(stream));
  }

  const pagesRootId = addObject("<< /Type /Pages /Count 0 /Kids [] >>");
  const pageIds = [];
  for (const contentId of contentIds) {
    const pageId = addObject(
      `<< /Type /Page /Parent ${pagesRootId} 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 ${fontId} 0 R >> >> /Contents ${contentId} 0 R >>`
    );
    pageIds.push(pageId);
  }

  objects[pagesRootId - 1] = `<< /Type /Pages /Count ${pageIds.length} /Kids [${pageIds.map((id) => `${id} 0 R`).join(" ")}] >>`;

  const catalogId = addObject(`<< /Type /Catalog /Pages ${pagesRootId} 0 R >>`);

  let pdf = "%PDF-1.4\n";
  const offsets = [0];

  for (let i = 0; i < objects.length; i++) {
    offsets.push(Buffer.byteLength(pdf, "utf8"));
    pdf += `${i + 1} 0 obj\n${objects[i]}\nendobj\n`;
  }

  const xrefOffset = Buffer.byteLength(pdf, "utf8");
  pdf += `xref\n0 ${objects.length + 1}\n`;
  pdf += "0000000000 65535 f \n";
  for (let i = 1; i < offsets.length; i++) {
    pdf += `${String(offsets[i]).padStart(10, "0")} 00000 n \n`;
  }
  pdf += `trailer\n<< /Size ${objects.length + 1} /Root ${catalogId} 0 R >>\n`;
  pdf += `startxref\n${xrefOffset}\n%%EOF`;
  return pdf;
}

fs.mkdirSync(path.dirname(outputPath), { recursive: true });
fs.writeFileSync(outputPath, makePdf(pages), "binary");
console.log(outputPath);
