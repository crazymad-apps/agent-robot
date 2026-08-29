import { spawn } from "node:child_process";
import { createHash, X509Certificate } from "node:crypto";
import { readFileSync } from "node:fs";
import tls from "node:tls";

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const INSTALLATION_ID = "fa0177a11da74ff089ec0de200000001";
const SCENARIOS = [
  "certificate_rejected",
  "upgrade_rejected",
  "fragment_control_binary_close",
  "oversized_text",
  "partial_frame_timeout",
  "masked_server_frame",
];

function argument(name) {
  const index = process.argv.indexOf(name);
  if (index < 0 || index + 1 >= process.argv.length) {
    throw new Error(`missing required argument: ${name}`);
  }
  return process.argv[index + 1];
}

const certificatePath = argument("--certificate");
const privateKeyPath = argument("--private-key");
const certificate = readFileSync(certificatePath);
const privateKey = readFileSync(privateKeyPath);
const fingerprint = createHash("sha256")
  .update(new X509Certificate(certificate).raw)
  .digest("hex");

let advertisement;
let activeSocket;
let scenarioIndex = 0;
let shuttingDown = false;

function cleanup() {
  if (shuttingDown) {
    return;
  }
  shuttingDown = true;
  if (advertisement !== undefined && advertisement.exitCode === null) {
    advertisement.kill("SIGTERM");
  }
  if (activeSocket !== undefined && !activeSocket.destroyed) {
    activeSocket.destroy();
  }
  server.close();
}

function fail(error) {
  const message = error instanceof Error ? error.stack ?? error.message : String(error);
  process.stderr.write(`transport_fault_host_failed ${message}\n`);
  process.exitCode = 1;
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

function finishScenario(socket) {
  if (socket !== activeSocket) {
    return;
  }
  activeSocket = undefined;
  process.stdout.write(`transport_fault_scenario_passed ${SCENARIOS[scenarioIndex]}\n`);
  scenarioIndex += 1;
  if (!socket.destroyed) {
    socket.end();
    setTimeout(() => socket.destroy(), 250).unref();
  }
  if (scenarioIndex === SCENARIOS.length) {
    process.stdout.write("transport_fault_host_passed\n");
    cleanup();
  }
}

function websocketAccept(clientKey) {
  return createHash("sha1").update(clientKey + GUID).digest("base64");
}

function readUpgrade(socket, acceptIsValid, ready) {
  let received = Buffer.alloc(0);
  let upgraded = false;
  const onData = (chunk) => {
    received = Buffer.concat([received, chunk]);
    if (received.length > 4096) {
      fail(new Error("WebSocket Upgrade request exceeded 4096 bytes"));
      return;
    }
    const headerEnd = received.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
      return;
    }
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
      fail(new Error("Upgrade request must contain exactly one Sec-WebSocket-Key"));
      return;
    }
    const clientKey = keyHeaders[0].slice(keyHeaders[0].indexOf(":") + 1).trim();
    const accept = acceptIsValid ? websocketAccept(clientKey) : "invalid-websocket-accept";
    socket.write(
      "HTTP/1.1 101 Switching Protocols\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        `Sec-WebSocket-Accept: ${accept}\r\n` +
        "\r\n",
    );
    upgraded = true;
    ready(received.subarray(headerEnd + 4));
  };
  socket.on("data", onData);
  return () => upgraded;
}

function serverFrame(opcode, final, payload) {
  const body = Buffer.from(payload);
  let header;
  if (body.length <= 125) {
    header = Buffer.from([(final ? 0x80 : 0) | opcode, body.length]);
  } else if (body.length <= 0xffff) {
    header = Buffer.alloc(4);
    header[0] = (final ? 0x80 : 0) | opcode;
    header[1] = 126;
    header.writeUInt16BE(body.length, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = (final ? 0x80 : 0) | opcode;
    header[1] = 127;
    header.writeBigUInt64BE(BigInt(body.length), 2);
  }
  return Buffer.concat([header, body]);
}

class ClientFrameParser {
  constructor(onFrame) {
    this.buffer = Buffer.alloc(0);
    this.onFrame = onFrame;
  }

  push(chunk) {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (this.buffer.length >= 2) {
      const final = (this.buffer[0] & 0x80) !== 0;
      const opcode = this.buffer[0] & 0x0f;
      if ((this.buffer[0] & 0x70) !== 0 || (this.buffer[1] & 0x80) === 0) {
        throw new Error("client frame is not a canonical masked RFC 6455 frame");
      }
      const marker = this.buffer[1] & 0x7f;
      let headerLength = 2;
      let payloadLength = marker;
      if (marker === 126) {
        if (this.buffer.length < 4) return;
        payloadLength = this.buffer.readUInt16BE(2);
        headerLength = 4;
      } else if (marker === 127) {
        if (this.buffer.length < 10) return;
        const decoded = this.buffer.readBigUInt64BE(2);
        if (decoded > BigInt(Number.MAX_SAFE_INTEGER)) {
          throw new Error("client frame length exceeds safe integer range");
        }
        payloadLength = Number(decoded);
        headerLength = 10;
      }
      const frameLength = headerLength + 4 + payloadLength;
      if (this.buffer.length < frameLength) return;
      const mask = this.buffer.subarray(headerLength, headerLength + 4);
      const payload = Buffer.alloc(payloadLength);
      for (let index = 0; index < payloadLength; index += 1) {
        payload[index] = this.buffer[headerLength + 4 + index] ^ mask[index % 4];
      }
      this.buffer = this.buffer.subarray(frameLength);
      this.onFrame({ final, opcode, payload });
    }
  }
}

function handleCertificateRejected(socket) {
  let applicationBytes = 0;
  socket.on("data", (chunk) => {
    applicationBytes += chunk.length;
  });
  socket.once("close", () => {
    if (applicationBytes !== 0) {
      fail(new Error(`certificate rejection sent ${applicationBytes} application bytes`));
      return;
    }
    finishScenario(socket);
  });
}

function handleUpgradeRejected(socket) {
  const upgraded = readUpgrade(socket, false, () => socket.end());
  socket.once("close", () => {
    if (!upgraded()) {
      fail(new Error("client closed before bad Upgrade response was injected"));
      return;
    }
    finishScenario(socket);
  });
}

function handleFragmentControlBinaryClose(socket) {
  let pongReceived = false;
  const closePayload = Buffer.concat([Buffer.from([0x03, 0xe9]), Buffer.from("restart")]);
  const parser = new ClientFrameParser(({ final, opcode, payload }) => {
    if (!final) {
      fail(new Error("client control frame must not be fragmented"));
      return;
    }
    if (opcode === 0x0a) {
      if (pongReceived || payload.toString("utf8") !== "probe") {
        fail(new Error("client Pong payload did not match injected Ping"));
        return;
      }
      pongReceived = true;
      const binary = Buffer.alloc(656);
      for (let index = 0; index < binary.length; index += 1) {
        binary[index] = index % 251;
      }
      socket.write(
        Buffer.concat([
          serverFrame(0x00, true, Buffer.alloc(32768, 0x42)),
          serverFrame(0x02, true, binary),
          serverFrame(0x08, true, closePayload),
        ]),
      );
      return;
    }
    if (opcode === 0x08) {
      if (!pongReceived || !payload.equals(closePayload)) {
        fail(new Error("client Close did not echo the injected code and reason"));
        return;
      }
      finishScenario(socket);
      return;
    }
    fail(new Error(`unexpected client opcode ${opcode}`));
  });
  readUpgrade(socket, true, (remaining) => {
    socket.on("data", (chunk) => parser.push(chunk));
    if (remaining.length !== 0) parser.push(remaining);
    socket.write(
      Buffer.concat([
        serverFrame(0x01, false, Buffer.alloc(32768, 0x41)),
        serverFrame(0x09, true, Buffer.from("probe")),
      ]),
    );
  });
}

function finishAfterInjectedClose(socket, inject) {
  let injected = false;
  readUpgrade(socket, true, () => {
    injected = true;
    inject();
  });
  socket.once("close", () => {
    if (!injected) {
      fail(new Error("client closed before frame fault was injected"));
      return;
    }
    finishScenario(socket);
  });
}

function handleScenario(socket, scenario) {
  if (scenario === "certificate_rejected") {
    handleCertificateRejected(socket);
  } else if (scenario === "upgrade_rejected") {
    handleUpgradeRejected(socket);
  } else if (scenario === "fragment_control_binary_close") {
    handleFragmentControlBinaryClose(socket);
  } else if (scenario === "oversized_text") {
    finishAfterInjectedClose(socket, () => {
      const header = Buffer.alloc(10);
      header[0] = 0x81;
      header[1] = 127;
      header.writeBigUInt64BE(65537n, 2);
      socket.write(header);
    });
  } else if (scenario === "partial_frame_timeout") {
    finishAfterInjectedClose(socket, () => socket.write(Buffer.from([0x81, 0x05, 0x61, 0x62])));
  } else if (scenario === "masked_server_frame") {
    finishAfterInjectedClose(socket, () => socket.write(Buffer.from([0x81, 0x80])));
  } else {
    fail(new Error(`unknown scenario ${scenario}`));
  }
}

const server = tls.createServer({ cert: certificate, key: privateKey }, (socket) => {
  if (activeSocket !== undefined || scenarioIndex >= SCENARIOS.length) {
    socket.destroy();
    fail(new Error("concurrent or unexpected TLS connection"));
    return;
  }
  activeSocket = socket;
  socket.setNoDelay(true);
  socket.setTimeout(20000, () => fail(new Error(`scenario timed out: ${SCENARIOS[scenarioIndex]}`)));
  socket.on("error", (error) => {
    if (!shuttingDown && error.code !== "ECONNRESET") fail(error);
  });
  handleScenario(socket, SCENARIOS[scenarioIndex]);
});

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
  advertisement = spawn(
    "/usr/bin/dns-sd",
    [
      "-R",
      "Agent Robot Transport Fault",
      "_ez-assistant._tcp",
      "local",
      String(address.port),
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
  process.stdout.write(`transport_fault_host_ready port=${address.port} fingerprint=${fingerprint}\n`);
});
