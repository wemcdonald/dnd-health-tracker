#!/usr/bin/env node
/**
 * provision.mjs — send a provisioning command to the device over USB-serial.
 *
 * Usage:
 *   node provision.mjs name <slug>
 *   node provision.mjs wifi <ssid> <psk> [<ssid> <psk> ...]
 *   node provision.mjs show
 *   node provision.mjs reboot
 *
 * Values are percent-encoded so spaces/specials survive the space-delimited line;
 * the firmware URL-decodes each token. name/wifi save to flash and reboot.
 *
 * USB-CDC on some hosts is flaky, so we: settle briefly after opening, send a
 * leading newline to clear any partial line on the device, and treat the device
 * rebooting (port drop) right after a save command as success.
 */
import fs from "node:fs";

const args = process.argv.slice(2);
if (args.length < 1) {
  console.error("usage: provision.mjs <name|wifi|show|reboot> [args...]");
  process.exit(2);
}

const devs = fs.readdirSync("/dev").filter((f) => f.startsWith("cu.usbmodem"));
if (devs.length === 0) {
  console.error("error: no /dev/cu.usbmodem* found — is the board plugged in via USB?");
  process.exit(1);
}
const dev = "/dev/" + devs[0];

const [cmd, ...params] = args;
const line = [cmd, ...params.map((s) => encodeURIComponent(s))].join(" ") + "\n";
const reboots = cmd === "name" || cmd === "wifi" || cmd === "reboot";

const fd = fs.openSync(dev, "r+");
let buf = "";
let wrote = false;
let done = false;

const rs = fs.createReadStream(null, { fd, autoClose: false });

const finish = (msg, code) => {
  if (done) return;
  done = true;
  console.log(msg);
  try { rs.destroy(); } catch {}
  try { fs.closeSync(fd); } catch {}
  process.exit(code);
};

rs.on("data", (d) => {
  buf += d;
  if (/\bOK\b/.test(buf))   setTimeout(() => finish(buf.trim(), 0), 150);
  else if (/\bERR\b/.test(buf)) setTimeout(() => finish(buf.trim(), 1), 150);
  else if (/no config|^slug=/m.test(buf)) setTimeout(() => finish(buf.trim(), 0), 150);
});
rs.on("error", () => {
  // Port dropped. After a save command that's the expected reboot = success.
  if (wrote && reboots) finish(buf.trim() || "sent — device rebooting to apply", 0);
});

// Settle the CDC, flush any partial line, then send the command.
setTimeout(() => {
  try {
    fs.writeSync(fd, "\n");
    fs.writeSync(fd, line);
    wrote = true;
  } catch (e) {
    finish("write failed: " + e.message, 1);
  }
}, 300);

// Cap the wait. A connected device can take a couple seconds to drain input.
setTimeout(() => {
  if (reboots) finish(buf.trim() || "no confirmation — re-run, or check the device web page", 0);
  else finish(buf.trim() || "(no response)", 0);
}, 9000);
