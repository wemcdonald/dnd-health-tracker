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
 * Values are percent-encoded so spaces/specials survive the space-delimited
 * line; the firmware URL-decodes each token. The device replies "OK ..."/"ERR
 * ..." then (for name/wifi) reboots to apply.
 */
import fs from "node:fs";

const args = process.argv.slice(2);
if (args.length < 1) {
  console.error("usage: provision.mjs <name|wifi|show|reboot> [args...]");
  process.exit(2);
}

// Find the device serial port.
const devs = fs.readdirSync("/dev").filter((f) => f.startsWith("cu.usbmodem"));
if (devs.length === 0) {
  console.error("error: no /dev/cu.usbmodem* found — is the board plugged in via USB?");
  process.exit(1);
}
const dev = "/dev/" + devs[0];

// Build the line: command + percent-encoded params.
const [cmd, ...params] = args;
const line = [cmd, ...params.map((s) => encodeURIComponent(s))].join(" ") + "\n";

const fd = fs.openSync(dev, "r+");
fs.writeSync(fd, line);

let buf = "";
const rs = fs.createReadStream(null, { fd, autoClose: false });
let done = false;
const finish = (code) => {
  if (done) return;
  done = true;
  const reply = buf.trim();
  console.log(reply || "(no response — command sent)");
  try { rs.destroy(); } catch {}
  try { fs.closeSync(fd); } catch {}
  // OK/“rebooting” still counts as success.
  process.exit(code ?? (/\bERR\b/.test(reply) ? 1 : 0));
};

rs.on("data", (d) => {
  buf += d;
  // The device reboots right after "OK ..."; grab the reply and stop.
  if (/\bOK\b|\bERR\b|no config|^slug=/m.test(buf)) setTimeout(() => finish(), 200);
});
rs.on("error", () => finish());        // port dropped (device rebooted) after we got the reply
// 'show' doesn't reboot; cap the wait. Connected devices can take a few seconds
// to drain input (poll cycle), so allow a generous window.
setTimeout(() => finish(), 9000);
