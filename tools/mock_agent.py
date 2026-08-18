#!/usr/bin/env python3
"""mock_agent.py — 本地模拟远端 Agent，用于终端固件联调。

行为：
  - 监听 0.0.0.0:8080/ws
  - 收到终端 hello 后，回复 state=connected 与一条 text 消息
  - 收到终端音频(二进制)后，打印长度；收到 audio_end 后回复一段 state=speaking

依赖：pip install websockets
用法：python3 tools/mock_agent.py [--host 0.0.0.0] [--port 8080]
"""
import argparse
import asyncio
import json
import logging

import websockets

log = logging.getLogger("mock_agent")


async def handler(ws):
    log.info("client connected: %s", ws.remote_address)
    try:
        async for raw in ws:
            if isinstance(raw, bytes):
                log.info("audio frame: %d bytes", len(raw))
                continue

            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                log.warning("non-json text: %r", raw[:200])
                continue

            mtype = msg.get("type")
            log.info("signal: %s", mtype)

            if mtype == "hello":
                await ws.send(json.dumps({"type": "state", "state": "connected"}))
                await ws.send(json.dumps({
                    "type": "text",
                    "text": f"你好 {msg.get('device', 'device')}，我是远端 Agent，联调成功。",
                }))
            elif mtype == "audio_start":
                pass
            elif mtype == "audio_end":
                # 模拟 Agent 开始回复
                await ws.send(json.dumps({"type": "state", "state": "thinking"}))
                await ws.send(json.dumps({"type": "state", "state": "speaking"}))
                await ws.send(json.dumps({
                    "type": "text",
                    "text": "（模拟回复）已收到你的语音。",
                }))
                await ws.send(json.dumps({"type": "audio_end"}))
                await ws.send(json.dumps({"type": "state", "state": "connected"}))
    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        log.info("client disconnected")


async def main(host: str, port: int):
    async with websockets.serve(handler, host, port):
        log.info("mock agent listening on ws://%s:%d/ws", host, port)
        await asyncio.Future()  # run forever


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="mock remote agent for agent-robot")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(main(args.host, args.port))
    except KeyboardInterrupt:
        pass
