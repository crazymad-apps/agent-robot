import { spawn } from "node:child_process";
import { createHash, X509Certificate } from "node:crypto";
import { readFileSync } from "node:fs";
import tls from "node:tls";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const INSTALLATION_ID = "fa0177a11da74ff089ec0de200000002";
const PHASE_COUNT = 2;

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index < 0 || index + 1 >= process.argv.length) {
    throw new Error(`missing required argument: ${name}`);
  }
  return process.argv[index + 1];
}

const certificate = readFileSync(argument("--certificate"));
const privateKey = readFileSync(argument("--private-key"));
const fingerprint = createHash("sha256")
  .update(new X509Certificate(certificate).raw)
  .digest("hex");

let advertisement;
let activeServer;
let activeSocket;
let previousPort;
let phase = 0;
let shuttingDown = false;

function cleanup() {
  if (shuttingDown) return;
  shuttingDown = true;
  if (advertisement !== undefined && advertisement.exitCode === null) {
    advertisement.kill("SIGTERM");
  }
  if (activeSocket !== undefined && !activeSocket.destroyed) {
    activeSocket.destroy();
  }
  if (activeServer !== undefined) {
    activeServer.close();
  }
}

function fail(error) {
  const message = error instanceof Error ? error.stack ?? error.message : String(error);
  process.stderr.write(`device_connection_recovery_host_failed ${message}\n`);
  process.exitCode = 1;
  clearTimeout(overallTimeout);
  cleanup();
}

process.on("SIGINT", () => {
  cleanup();
  process.exit(130);
});
process.on("SIGTERM", () => {
  cleanup();
  process.exit(143);
});
process.on("uncaughtException", fail);
process.on("unhandledRejection", fail);

const overallTimeout = setTimeout(
  () => fail(new Error("recovery validation exceeded 90 seconds")),
  90000,
);

function websocketAccept(clientKey) {
  return createHash("sha1").update(clientKey + GUID).digest("base64");
}

function serverTextFrame(payload) {
  const body = Buffer.from(payload, "utf8");
  if (body.length > 125) {
    throw new Error("validation control envelope exceeds short-frame capacity");
  }
  return Buffer.concat([Buffer.from([0x81, body.length]), body]);
}

function challengeEnvelope(currentPhase) {
  return JSON.stringify({
    protocol_major: 1,
    protocol_minor: 0,
    message_id: `recovery-auth-challenge-${currentPhase + 1}`,
    type: "auth_challenge",
    payload: {},
  });
}

function stopAdvertisement() {
  if (advertisement !== undefined && advertisement.exitCode === null) {
    advertisement.kill("SIGTERM");
  }
  advertisement = undefined;
}

function transitionToNextPhase(socket, server) {
  stopAdvertisement();
  if (!socket.destroyed) socket.destroy();
  activeSocket = undefined;
  server.close(() => {
    activeServer = undefined;
    phase += 1;
    setTimeout(() => startPhase(), 1500);
  });
}

function finishValidation(socket) {
  setTimeout(() => {
    process.stdout.write("device_connection_recovery_host_passed\n");
    clearTimeout(overallTimeout);
    if (!socket.destroyed) socket.end();
    cleanup();
  }, 1000);
}

function handleClient(socket, server) {
  if (activeSocket !== undefined) {
    socket.destroy();
    fail(new Error("concurrent Device WSS connection"));
    return;
  }
  activeSocket = socket;
  socket.setNoDelay(true);
  socket.setTimeout(20000, () => fail(new Error(`phase ${phase + 1} timed out`)));
  socket.on("error", (error) => {
    if (!shuttingDown && error.code !== "ECONNRESET") fail(error);
  });

  let received = Buffer.alloc(0);
  const onData = (chunk) => {
    received = Buffer.concat([received, chunk]);
    if (received.length > 4096) {
      fail(new Error("WebSocket Upgrade request exceeded 4096 bytes"));
      return;
    }
    const headerEnd = received.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;
    socket.off("data", onData);
    const header = received.subarray(0, headerEnd + 4).toString("latin1");
    const lines = header.split("\r\n");
    if (lines[0] !== "GET /device HTTP/1.1") {
      fail(new Error(`unexpected request line: ${lines[0]}`));
      return;
    }
    const keyHeaders = lines
      .slice(1)
      .filter((line) => line.toLowerCase().startsWith("sec-websocket-key:"));
    if (keyHeaders.length !== 1) {
      fail(new Error("Upgrade request must contain one Sec-WebSocket-Key"));
      return;
    }
    const clientKey = keyHeaders[0].slice(keyHeaders[0].indexOf(":") + 1).trim();
    const upgradeResponse =
      "HTTP/1.1 101 Switching Protocols\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        `Sec-WebSocket-Accept: ${websocketAccept(clientKey)}\r\n` +
        "\r\n";
    socket.write(
      Buffer.concat([
        Buffer.from(upgradeResponse, "latin1"),
        serverTextFrame(challengeEnvelope(phase)),
      ]),
      () => {
        process.stdout.write(`device_connection_recovery_phase_passed phase=${phase + 1}\n`);
        if (phase + 1 < PHASE_COUNT) {
          setTimeout(() => transitionToNextPhase(socket, server), 500);
        } else {
          finishValidation(socket);
        }
      },
    );
  };
  socket.on("data", onData);
}

function advertise(port) {
  advertisement = spawn(
    "/usr/bin/dns-sd",
    [
      "-R",
      "Agent Robot Device Recovery",
      "_ez-assistant._tcp",
      "local",
      String(port),
      "protocol_major=1",
      "protocol_minor=0",
      "path=/device",
      `installation_id=${INSTALLATION_ID}`,
      `certificate_fingerprint=${fingerprint}`,
      "pairing_available=true",
    ],
    { stdio: "ignore" },
  );
  advertisement.on("error", fail);
  process.stdout.write(
    `device_connection_recovery_host_ready phase=${phase + 1} port=${port} fingerprint=${fingerprint}\n`,
  );
}

function startPhase() {
  if (shuttingDown) return;
  const server = tls.createServer({ cert: certificate, key: privateKey }, (socket) =>
    handleClient(socket, server),
  );
  activeServer = server;
  server.on("tlsClientError", (error) => {
    if (!shuttingDown && error.code !== "ECONNRESET") fail(error);
  });
  server.on("error", fail);
  server.listen(0, "0.0.0.0", () => {
    const address = server.address();
    if (address === null || typeof address === "string") {
      fail(new Error("TLS server did not expose an IPv4 port"));
      return;
    }
    if (previousPort === address.port) {
      server.close(() => setTimeout(() => startPhase(), 100));
      return;
    }
    previousPort = address.port;
    advertise(address.port);
  });
}

startPhase();
