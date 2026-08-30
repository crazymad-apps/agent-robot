import { spawn } from "node:child_process";
import {
  createHash,
  createPublicKey,
  verify,
  X509Certificate,
} from "node:crypto";
import { readFileSync } from "node:fs";
import tls from "node:tls";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const INSTALLATION_ID = "fa0177a11da74ff089ec0de200000003";
const DEVICE_ID = "device-auth-validation";
const PUBLIC_KEY = "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg";
const CAPABILITIES = {
  input_text: false,
  input_pcm16_16k_mono: true,
  output_text: false,
  output_pcm16_16k_mono: true,
  playback_cancel: true,
  display_status: true,
  display_transcript: false,
};

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index < 0 || index + 1 >= process.argv.length) {
    throw new Error(`missing required argument: ${name}`);
  }
  return process.argv[index + 1];
}

function base64UrlDecode(value) {
  if (typeof value !== "string" || value.includes("=")) {
    throw new Error("base64url value is invalid");
  }
  return Buffer.from(value.replaceAll("-", "+").replaceAll("_", "/"), "base64");
}

function appendTranscript(parts, value) {
  const bytes = Buffer.isBuffer(value) ? value : Buffer.from(value, "utf8");
  const length = Buffer.alloc(8);
  length.writeBigUInt64BE(BigInt(bytes.length));
  parts.push(length, bytes);
}

function authenticationTranscript(hello, connectionId, hostNonce) {
  const parts = [];
  appendTranscript(parts, "ez-assistant-auth-v1");
  appendTranscript(parts, Buffer.from([0, 1]));
  appendTranscript(parts, Buffer.from([0, 0]));
  appendTranscript(parts, connectionId);
  appendTranscript(parts, hostNonce);
  appendTranscript(parts, DEVICE_ID);
  appendTranscript(parts, hello.payload.device_nonce);
  appendTranscript(parts, Buffer.from([0, 1, 0, 1, 1, 1, 0]));
  appendTranscript(parts, "audio");
  return Buffer.concat(parts);
}

function websocketAccept(clientKey) {
  return createHash("sha1").update(clientKey + GUID).digest("base64");
}

function serverTextFrame(document) {
  const body = Buffer.from(JSON.stringify(document), "utf8");
  if (body.length <= 125) return Buffer.concat([Buffer.from([0x81, body.length]), body]);
  if (body.length > 0xffff) throw new Error("server envelope exceeds validation capacity");
  const header = Buffer.alloc(4);
  header[0] = 0x81;
  header[1] = 126;
  header.writeUInt16BE(body.length, 2);
  return Buffer.concat([header, body]);
}

function readClientTextFrame(buffer) {
  if (buffer.length < 2) return undefined;
  const first = buffer[0];
  const second = buffer[1];
  if (first !== 0x81 || (second & 0x80) === 0) {
    throw new Error("client frame must be one final masked text frame");
  }
  let length = second & 0x7f;
  let offset = 2;
  if (length === 126) {
    if (buffer.length < 4) return undefined;
    length = buffer.readUInt16BE(2);
    offset = 4;
  } else if (length === 127) {
    throw new Error("client frame unexpectedly uses 64-bit length");
  }
  if (buffer.length < offset + 4 + length) return undefined;
  const mask = buffer.subarray(offset, offset + 4);
  offset += 4;
  const payload = Buffer.from(buffer.subarray(offset, offset + length));
  for (let index = 0; index < payload.length; index += 1) {
    payload[index] ^= mask[index % 4];
  }
  return {
    document: JSON.parse(payload.toString("utf8")),
    remaining: buffer.subarray(offset + length),
  };
}

function envelope(messageId, type, payload) {
  return { protocol_major: 1, protocol_minor: 0, message_id: messageId, type, payload };
}

function verifyHello(hello, connectionId, hostNonce) {
  if (
    hello.protocol_major !== 1 ||
    hello.protocol_minor !== 0 ||
    hello.type !== "hello" ||
    hello.payload?.device_id !== DEVICE_ID ||
    hello.payload?.output_preference !== "audio" ||
    JSON.stringify(hello.payload?.capabilities) !== JSON.stringify(CAPABILITIES) ||
    typeof hello.payload?.device_nonce !== "string" ||
    typeof hello.payload?.client_version !== "string"
  ) {
    throw new Error("signed hello payload is invalid");
  }
  const publicKeyDer = Buffer.concat([
    Buffer.from("302a300506032b6570032100", "hex"),
    base64UrlDecode(PUBLIC_KEY),
  ]);
  const publicKey = createPublicKey({ key: publicKeyDer, format: "der", type: "spki" });
  if (!verify(
    null,
    authenticationTranscript(hello, connectionId, hostNonce),
    publicKey,
    base64UrlDecode(hello.payload.signature),
  )) {
    throw new Error("signed hello signature is invalid");
  }
}

const certificate = readFileSync(argument("--certificate"));
const privateKey = readFileSync(argument("--private-key"));
const fingerprint = createHash("sha256")
  .update(new X509Certificate(certificate).raw)
  .digest("hex");

let advertisement;
let server;
let activeSocket;
let completed = false;
let connectionCount = 0;

function cleanup() {
  if (advertisement !== undefined && advertisement.exitCode === null) advertisement.kill("SIGTERM");
  if (activeSocket !== undefined && !activeSocket.destroyed) activeSocket.destroy();
  if (server !== undefined) server.close();
}

function fail(error) {
  const message = error instanceof Error ? error.stack ?? error.message : String(error);
  process.stderr.write(`device_authentication_host_failed ${message}\n`);
  process.exitCode = 1;
  clearTimeout(overallTimeout);
  cleanup();
}

process.on("SIGINT", () => { cleanup(); process.exit(130); });
process.on("SIGTERM", () => { cleanup(); process.exit(143); });
process.on("uncaughtException", fail);
process.on("unhandledRejection", fail);

const overallTimeout = setTimeout(
  () => fail(new Error("authentication validation exceeded 60 seconds")),
  60000,
);

function handleClient(socket) {
  if (activeSocket !== undefined && !activeSocket.destroyed) {
    throw new Error("concurrent Device WSS connection");
  }
  activeSocket = socket;
  connectionCount += 1;
  if (connectionCount > 2) throw new Error("unexpected additional Device connection");
  const connectionNumber = connectionCount;
  const connectionId = `connection-auth-validation-${connectionNumber}`;
  const hostNonce = `host-auth-validation-nonce-${connectionNumber}`;
  socket.setNoDelay(true);
  socket.setTimeout(30000, () => fail(new Error("Device authentication timed out")));
  socket.on("error", (error) => {
    if (!completed && error.code !== "ECONNRESET") fail(error);
  });
  let received = Buffer.alloc(0);
  let upgraded = false;
  let helloHandled = false;
  socket.on("data", (chunk) => {
    received = Buffer.concat([received, chunk]);
    if (!upgraded) {
      const headerEnd = received.indexOf("\r\n\r\n");
      if (headerEnd < 0) return;
      const header = received.subarray(0, headerEnd + 4).toString("latin1");
      const lines = header.split("\r\n");
      if (lines[0] !== "GET /device HTTP/1.1") throw new Error("unexpected Upgrade path");
      const keyLine = lines.find((line) => line.toLowerCase().startsWith("sec-websocket-key:"));
      if (keyLine === undefined) throw new Error("Sec-WebSocket-Key is missing");
      const clientKey = keyLine.slice(keyLine.indexOf(":") + 1).trim();
      const response =
        "HTTP/1.1 101 Switching Protocols\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        `Sec-WebSocket-Accept: ${websocketAccept(clientKey)}\r\n\r\n`;
      received = received.subarray(headerEnd + 4);
      upgraded = true;
      socket.write(Buffer.concat([
        Buffer.from(response, "latin1"),
        serverTextFrame(envelope(`auth-challenge-${connectionNumber}`, "auth_challenge", {
          connection_id: connectionId,
          nonce: hostNonce,
          server_time_ms: Date.now(),
        })),
      ]));
    }
    if (helloHandled) return;
    const frame = readClientTextFrame(received);
    if (frame === undefined) return;
    received = frame.remaining;
    helloHandled = true;
    verifyHello(frame.document, connectionId, hostNonce);
    process.stdout.write(`device_authentication_hello_verified attempt=${connectionNumber}\n`);
    if (connectionNumber === 1) {
      socket.write(serverTextFrame(envelope("authentication-failed-1", "error", {
        code: "authentication_failed",
        correlation_message_id: null,
        recoverable: false,
      })));
      return;
    }
    socket.write(serverTextFrame(envelope("hello-ack-2", "hello_ack", {
      device_id: DEVICE_ID,
      connection_id: connectionId,
      capabilities: CAPABILITIES,
      output_preference: "audio",
    })));
    setTimeout(() => {
      socket.write(serverTextFrame(envelope("device-revoked-2", "error", {
        code: "device_revoked",
        correlation_message_id: null,
        recoverable: false,
      })));
    }, 500);
  });
  socket.on("close", () => {
    if (activeSocket === socket) activeSocket = undefined;
    if (completed) return;
    if (connectionNumber === 1) {
      process.stdout.write("device_authentication_pending_rejected\n");
      return;
    }
    completed = true;
    process.stdout.write("device_authentication_host_passed\n");
    clearTimeout(overallTimeout);
    cleanup();
  });
}

server = tls.createServer({ cert: certificate, key: privateKey }, handleClient);
server.on("tlsClientError", (error) => {
  if (!completed && error.code !== "ECONNRESET") fail(error);
});
server.on("error", fail);
server.listen(0, "0.0.0.0", () => {
  const address = server.address();
  if (address === null || typeof address === "string") throw new Error("TLS port unavailable");
  advertisement = spawn(
    "/usr/bin/dns-sd",
    [
      "-R",
      "Agent Robot Device Authentication",
      "_ez-assistant._tcp",
      "local",
      String(address.port),
      "protocol_major=1",
      "protocol_minor=0",
      "path=/device",
      `installation_id=${INSTALLATION_ID}`,
      `certificate_fingerprint=${fingerprint}`,
      "pairing_available=false",
    ],
    { stdio: "ignore" },
  );
  advertisement.on("error", fail);
  process.stdout.write(
    `device_authentication_host_ready port=${address.port} fingerprint=${fingerprint}\n`,
  );
});
