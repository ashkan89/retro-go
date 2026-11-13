/* Browser regression tests with an isolated, in-memory device API. */
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const http = require("node:http");
const zlib = require("node:zlib");
const { chromium } = require("playwright");
const AxeBuilder = require("@axe-core/playwright").default;
const root = path.resolve(__dirname, "../..");
const output = path.join(root, "build/webui/preview");
fs.mkdirSync(output, { recursive: true });
const pageBytes = zlib.gzipSync(
  fs.readFileSync(path.join(root, "launcher/main/webui/index.html")),
);
const status = {
  success: true,
  name: "Retro-Go",
  target: "ESP32-P4-N16R32-ST7796",
  version: "5.8.0",
  build: "Nov 13 2026",
  root: "/sd",
  uptime: 8241,
  heap_free: 215040,
  heap_total: 524288,
  psram_free: 24500000,
  psram_total: 33554432,
  cpu_mhz: 400,
  storage_ready: true,
  storage_free: 12348030976,
  volume: 50,
  brightness: 70,
  log_level: 3,
  theme: "classic",
  ssid: "Home network",
  ip: "192.168.1.42",
  rssi: -52,
  channel: 6,
  network_state: 3,
  ap_mode: false,
  battery_present: true,
  battery: 84,
  battery_volts: 4.05,
  charging: false,
  can_install: true,
  apps: ["factory", "launcher", "retro-core", "gbsp"],
};
const entry = (name, dir = false, size = 1024) => ({
  name,
  is_dir: dir,
  size: dir ? 0 : size,
  mtime: 1794528000,
});
const folders = new Map([
  ["/sd/retro-go/firmware", [entry("update.img", false, 4096)]],
  [
    "/sd",
    [
      entry("roms", true),
      entry("retro-go", true),
      entry("music", true),
      entry("Rock & Roll's #1%.nes"),
      entry("<img src=x onerror=alert(1)>"),
    ],
  ],
  ["/sd/roms", [entry("nes", true), entry("gba", true), entry("msx", true)]],
  [
    "/sd/roms/nes",
    [
      entry("Example.nes"),
      entry("Adventure.nes", false, 2048),
      entry("Backups", true),
    ],
  ],
  ["/sd/roms/gba", []],
  ["/sd/roms/msx", []],
  ["/sd/retro-go", [entry("saves", true), entry("themes", true)]],
  ["/sd/retro-go/saves", [entry("Progress.sav")]],
  ["/sd/retro-go/themes", [entry("classic", true)]],
  ["/sd/music", []],
]);
const calls = [];
let offline = false,
  uploads = 0,
  maxUploads = 0,
  failOnce = true;
const server = http.createServer(async (req, res) => {
  if (req.url === "/") {
    res.writeHead(200, {
      "Content-Type": "text/html; charset=utf-8",
      "Content-Encoding": "gzip",
      "Content-Security-Policy":
        "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' blob: data:; connect-src 'self'; object-src 'none'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'",
    });
    return res.end(pageBytes);
  }
  if (req.url === "/api/status") {
    res.writeHead(offline ? 503 : 200, { "Content-Type": "application/json" });
    return res.end(offline ? "Device offline" : JSON.stringify(status));
  }
  if (req.url === "/favicon.ico") {
    res.writeHead(204);
    return res.end();
  }
  let body = "";
  for await (const chunk of req) body += chunk;
  if (req.method === "POST" && req.url === "/api") {
    assert.equal(req.headers["x-retrogo"], "1");
    const data = JSON.parse(body);
    calls.push(data);
    const response = { success: true };
    if (data.cmd === "list") {
      if (!folders.has(data.arg1)) {
        response.success = false;
        response.error = "Folder does not exist";
      } else {
        const offset = data.offset || 0,
          all = folders.get(data.arg1),
          limit = data.arg1 === "/sd/paged" ? 2 : 128;
        response.files = all.slice(offset, offset + limit);
        response.truncated = all.length > offset + limit;
        response.limit = limit;
        response.offset = offset;
        response.next_offset = offset + response.files.length;
      }
    } else if (data.cmd === "mkdir") {
      folders.set(data.arg1, folders.get(data.arg1) || []);
    } else if (data.cmd === "rename") {
      const oldParent = path.posix.dirname(data.arg1),
        newParent = path.posix.dirname(data.arg2),
        list = folders.get(oldParent);
      const index = list.findIndex(
        (f) => f.name === path.posix.basename(data.arg1),
      );
      assert(index >= 0);
      const file = list.splice(index, 1)[0];
      file.name = path.posix.basename(data.arg2);
      folders.get(newParent).push(file);
    } else if (data.cmd === "delete") {
      const list = folders.get(path.posix.dirname(data.arg1));
      list.splice(
        list.findIndex((f) => f.name === path.posix.basename(data.arg1)),
        1,
      );
    } else if (data.cmd === "settings") {
      status[data.arg1] = data.value;
    }
    res.writeHead(200, { "Content-Type": "application/json" });
    return res.end(JSON.stringify(response));
  }
  if (req.method === "PUT") {
    assert.equal(req.headers["x-retrogo"], "1");
    uploads++;
    maxUploads = Math.max(maxUploads, uploads);
    calls.push({
      cmd: "upload",
      uri: req.url,
      overwrite: req.headers["x-retrogo-overwrite"],
    });
    const file = decodeURIComponent(req.url),
      list = folders.get(path.posix.dirname(file));
    setTimeout(() => {
      uploads--;
      if (file.endsWith("fail.bin") && failOnce) {
        failOnce = false;
        res.writeHead(500);
        return res.end("Simulated write failure");
      }
      const old = list.findIndex((f) => f.name === path.posix.basename(file));
      if (old >= 0 && req.headers["x-retrogo-overwrite"] !== "1") {
        res.writeHead(409);
        return res.end("File exists");
      }
      if (old >= 0) list.splice(old, 1);
      list.push(
        entry(path.posix.basename(file), false, Buffer.byteLength(body)),
      );
      res.writeHead(200);
      res.end("OK");
    }, 200);
    return;
  }
  res.writeHead(200, {
    "Content-Type": "application/octet-stream",
    "Content-Disposition": "attachment",
  });
  res.end("example file");
});

async function main() {
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = "http://127.0.0.1:" + server.address().port;
  const browser = await chromium.launch({
    headless: true,
    ...(process.env.WEBUI_CHROMIUM
      ? { executablePath: process.env.WEBUI_CHROMIUM }
      : {}),
  });
  const context = await browser.newContext({
    viewport: { width: 1440, height: 1100 },
    reducedMotion: "reduce",
  });
  const page = await context.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  const nav = (label) =>
    page
      .locator("#nav")
      .getByRole("button", { name: label, exact: true })
      .click();
  try {
    await page.goto(address);
    await page.getByText("● Connected", { exact: true }).waitFor();
    const initialAudit = await new AxeBuilder({ page })
      .withTags(["wcag2a", "wcag2aa", "wcag21aa"])
      .analyze();
    assert.deepEqual(
      initialAudit.violations.map((v) => ({
        id: v.id,
        nodes: v.nodes.map((n) => n.target),
      })),
      [],
      "Desktop accessibility",
    );
    await page.screenshot({
      path: path.join(output, "overview-desktop.png"),
      fullPage: true,
    });
    assert.match(await page.locator("[data-live=storage]").textContent(), /GB/);
    await nav("Game library");
    await page
      .getByRole("button", {
        name: "NES Nintendo Entertainment System Open game folder",
      })
      .waitFor();
    await page
      .getByRole("button", {
        name: "NES Nintendo Entertainment System Open game folder",
      })
      .click();
    await page
      .getByRole("button", { name: "Example.nes", exact: true })
      .waitFor();
    await page.getByLabel("Filter files on this page").fill("adventure");
    assert.equal(await page.locator("#file-rows tr").count(), 1);
    await page.getByLabel("Filter files on this page").fill("");
    await page
      .getByRole("button", { name: "Rename or move Example.nes", exact: true })
      .click();
    await page.locator("#dialog-input").fill("Renamed.nes");
    await page.getByRole("button", { name: "Save", exact: true }).click();
    await page
      .getByRole("button", { name: "Renamed.nes", exact: true })
      .waitFor();
    await page
      .getByRole("button", { name: "Delete Renamed.nes", exact: true })
      .click();
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    assert(folders.get("/sd/roms/nes").some((f) => f.name === "Renamed.nes"));
    await page
      .getByRole("button", { name: "Delete Renamed.nes", exact: true })
      .click();
    await page
      .getByRole("button", { name: "Delete permanently", exact: true })
      .click();
    await page
      .getByRole("button", { name: "Renamed.nes", exact: true })
      .waitFor({ state: "detached" });
    await page.getByRole("button", { name: "New folder", exact: true }).click();
    await page.locator("#dialog-input").fill("../outside");
    await page.getByRole("button", { name: "Create", exact: true }).click();
    assert.equal(await page.locator("#dialog").evaluate((d) => d.open), true);
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    await page
      .getByRole("button", { name: "SD card", exact: true })
      .first()
      .click();
    await page
      .getByRole("button", { name: "Rock & Roll's #1%.nes", exact: true })
      .waitFor();
    assert.equal(await page.locator("#file-rows img").count(), 0);
    const downloadPromise = page.waitForEvent("download");
    await page
      .getByRole("button", { name: "Rock & Roll's #1%.nes", exact: true })
      .click();
    await downloadPromise;
    folders.set("/sd/paged", [
      entry("one"),
      entry("two"),
      entry("three"),
      entry("four"),
      entry("five"),
    ]);
    await page.evaluate(() => loadFiles("/sd/paged"));
    await page.getByRole("button", { name: "Next page", exact: true }).click();
    await page.getByRole("button", { name: "three", exact: true }).waitFor();
    await page.getByRole("button", { name: "Next page", exact: true }).click();
    await page.getByRole("button", { name: "five", exact: true }).waitFor();
    assert.equal(
      await page
        .getByRole("button", { name: "Next page", exact: true })
        .count(),
      0,
    );
    await page
      .getByRole("button", { name: "Previous page", exact: true })
      .click();
    await page.getByRole("button", { name: "three", exact: true }).waitFor();
    await nav("Device");
    await page.getByLabel("Audio volume").fill("35");
    await page
      .locator(".settings-row")
      .first()
      .getByRole("button", { name: "Apply", exact: true })
      .click();
    assert.equal(status.volume, 35);
    await page
      .getByRole("button", { name: "Restart device", exact: true })
      .click();
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    assert(!calls.some((c) => c.cmd === "restart"));
    await nav("Network");
    assert.equal(
      await page.locator("[data-live=ssid]").textContent(),
      "Home network",
    );
    await nav("Firmware");
    await page.getByLabel("Staged firmware image").selectOption("update.img");
    await page
      .getByRole("button", { name: "Request installation", exact: true })
      .click();
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    assert(!calls.some((c) => c.cmd === "install"));
    await page
      .getByRole("button", { name: "Request installation", exact: true })
      .click();
    await page.locator("#dialog-confirm").click();
    await page.getByText("Confirm on the console to validate and install. Cancel there to return to the web manager.", {exact:true}).waitFor();
    assert(
      calls.some(
        (c) =>
          c.cmd === "install" && c.arg1 === "/sd/retro-go/firmware/update.img",
      ),
    );
    const diagnosticPromise = page.waitForEvent("download");
    await page
      .getByRole("button", { name: "Export diagnostic report" })
      .click();
    const diagnostic = await diagnosticPromise;
    assert.match(diagnostic.suggestedFilename(), /retro-go-diagnostics/);
    await nav("Files & saves");
    await page.getByRole("button", { name: "Games", exact: true }).click();
    await page.getByRole("button", { name: "nes", exact: true }).waitFor();
    await page.getByRole("button", { name: "Upload", exact: true }).click();
    await page.locator("#upload-input").setInputFiles([
      {
        name: "first.bin",
        mimeType: "application/octet-stream",
        buffer: Buffer.from("first"),
      },
      {
        name: "fail.bin",
        mimeType: "application/octet-stream",
        buffer: Buffer.from("second"),
      },
    ]);
    await page.getByText("Uploaded successfully", { exact: true }).waitFor();
    await page.getByText("Simulated write failure", { exact: true }).waitFor();
    assert.equal(maxUploads, 1);
    await page.getByRole("button", { name: "Retry", exact: true }).click();
    await page.waitForFunction(() =>
      state.queue.every((t) => t.status === "done"),
    );
    assert.equal(maxUploads, 1);
    assert(
      calls
        .filter((c) => c.cmd === "upload")
        .every((c) => c.uri.startsWith("/sd/roms/")),
    );
    await nav("Files & saves");
    await page.getByRole("button", { name: "Upload", exact: true }).click();
    await page
      .locator("#upload-input")
      .setInputFiles([
        {
          name: "first.bin",
          mimeType: "application/octet-stream",
          buffer: Buffer.from("replacement"),
        },
      ]);
    await page.getByRole("button", { name: "Replace…", exact: true }).click();
    await page.getByRole("button", { name: "Cancel", exact: true }).click();
    assert(!calls.some((c) => c.cmd === "upload" && c.overwrite === "1"));
    await page.getByRole("button", { name: "Replace…", exact: true }).click();
    await page
      .getByRole("button", { name: "Replace file", exact: true })
      .click();
    await page.waitForFunction(() =>
      state.queue.every((t) => t.status === "done"),
    );
    assert(calls.some((c) => c.cmd === "upload" && c.overwrite === "1"));
    await nav("Overview");
    await page.locator("#theme").click();
    await page.screenshot({
      path: path.join(output, "overview-dark.png"),
      fullPage: true,
    });
    await page.setViewportSize({ width: 390, height: 844 });
    await page.screenshot({
      path: path.join(output, "overview-mobile.png"),
      fullPage: true,
    });
    for (const name of [
      "Files & saves",
      "Device",
      "Network",
      "Firmware",
      "Transfers",
      "Game library",
      "Overview",
    ]) {
      await nav(name);
      assert(
        await page.evaluate(
          () => document.documentElement.scrollWidth <= innerWidth,
        ),
        "Page overflow: " + name,
      );
      const audit = await new AxeBuilder({ page })
        .withTags(["wcag2a", "wcag2aa", "wcag21aa"])
        .analyze();
      assert.deepEqual(
        audit.violations.map((v) => ({
          id: v.id,
          nodes: v.nodes.map((n) => n.target),
        })),
        [],
        "Accessibility: " + name,
      );
    }
    status.storage_ready = false;
    await page.evaluate(() => refreshStatus());
    await nav("Game library");
    await page.getByRole("heading", { name: "Insert an SD card" }).waitFor();
    assert.equal(await page.locator("#storage-banner").isVisible(), true);
    await nav("Device");
    assert.equal(
      await page
        .locator(".settings-row")
        .first()
        .getByRole("button", { name: "Apply", exact: true })
        .isEnabled(),
      true,
    );
    offline = true;
    await page.evaluate(() => refreshStatus());
    assert.equal(await page.locator("#offline").isVisible(), true);
    assert.equal(
      await page
        .locator(".settings-row")
        .first()
        .getByRole("button", { name: "Apply", exact: true })
        .isEnabled(),
      false,
    );
    offline = false;
    status.storage_ready = true;
    await page.getByRole("button", { name: "Retry", exact: true }).click();
    await page.getByText("● Connected", { exact: true }).waitFor();
    assert.deepEqual(errors, []);
    console.log(
      "PASS: navigation, file filtering/rename/delete, escaped filenames, pagination, settings, exports, sequential uploads/retry, dark/mobile layouts, no-SD and reconnect states",
    );
    console.log("Screenshots: " + output);
  } catch (error) {
    console.error("Browser errors:", errors);
    console.error(
      "Current route:",
      await page.locator("#page-title").textContent(),
    );
    await page.screenshot({
      path: path.join(output, "failure.png"),
      fullPage: true,
    });
    throw error;
  } finally {
    await browser.close();
    await new Promise((resolve) => server.close(resolve));
  }
}
main().catch((error) => {
  console.error(error);
  server.close();
  process.exitCode = 1;
});
