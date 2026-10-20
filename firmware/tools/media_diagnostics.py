#!/usr/bin/env python3
"""Collect allowlisted media metadata; never persist raw serial/Host output.

Snapshots describe software progress, not audible completion or leak-free acceptance.
The output directory must not already exist. Serial capture does not reset the device.
"""

import argparse
import collections
import json
import os
from pathlib import Path
import re
import sys
import time


EVENTS = frozenset("""
terminal_boot runtime_resource_snapshot runtime_task_stack_snapshot
audio_resource_snapshot voice_resource_snapshot audio_playback audio_capture
voice_input playback_rebuffer playback_end_received audio_ingress_rejected
terminal_event_queue_overflow audio_ingress_failed audio_cancel_failed
voice_input_operation_failed terminal_interaction_failed
device_connection_state_changed device_connection_failure
speech_request_started speech_request_finished speech_request_rejected
speech_service_health speech_delivery_started playback_reservation playback_admission
playback_queue playback_discarded playback_started playback_first_frame playback_sent
voice_capture_started voice_segment_started voice_segment_finished voice_turn_ready
voice_input_accepted voice_input_rejected
""".split())
NUMBERS = frozenset("""
ts_ms stream_id connection_epoch queue_ms elapsed_ms input_bytes pcm_bytes in_flight
command_depth depth targets frames samples segments recognized type result
received_frames consumed_samples sample_count queued_frames rebuffer_count
internal_free_bytes internal_minimum_free_bytes internal_largest_block_bytes
psram_free_bytes psram_minimum_free_bytes psram_largest_block_bytes minimum_free_bytes
playback_state frame_high_watermark stream_high_watermark rebuffer_total completed
cancelled failed capture_state state accepted retransmissions rejected expired
dropped_frames dropped count last_result
block failure selection transport reconnect_delay_ms
""".split())
ENUMS = frozenset("""
ok ready unavailable degraded starting disabled accepted cancelled completed failed
capacity_exceeded interrupted busy timeout invalid_input provider_error unauthorized rejected
empty_transcript reserved attached discarded finished asr tts Ready Degraded Unavailable
Starting Disabled unknown none error
authentication invalid_transcript invalid_audio output_too_large provider_failed
replaced revoked shutdown interrupted_by_input connected connecting disconnected
discovering backoff authenticated online idle
""".split())
ENUM_FIELDS = frozenset(("stage", "result", "capability", "asr", "tts", "status", "reason", "state"))
TASKS = frozenset(("audio_io", "voice_input", "terminal_lifecycle", "terminal_lifecy",
                   "device_connection", "device_connecti", "terminal_ui", "terminal_button"))
IDS = frozenset(("request", "run", "input", "device"))
ANSI = re.compile(r"\x1b\[[0-9;]*m")
PAIR = re.compile(r"(?:^|\s)([a-z_]+)=([^\s]+)")
UPTIME = re.compile(r"^[IWEVD] \((\d{1,16})\)")
MAX_LINE = 4096


def parse_line(line):
    """Return only typed metadata; arbitrary values, identifiers and text are dropped."""
    if len(line) > MAX_LINE:
        return None
    line = ANSI.sub("", line).strip()
    values = dict(PAIR.findall(line))
    if values.get("event") not in EVENTS:
        return None
    record = {"event": values["event"]}
    uptime = UPTIME.match(line)
    if uptime:
        record["uptime_ms"] = int(uptime[1])
    for key, value in values.items():
        if key in NUMBERS and re.fullmatch(r"[0-9]{1,20}", value):
            record[key] = int(value)
        elif key in ENUM_FIELDS and value in ENUMS:
            record[key] = value
        elif key in IDS and re.fullmatch(r"[a-f0-9]{12}", value):
            record[key] = value
        elif key == "task" and value in TASKS:
            record[key] = value
    return record


class Summary:
    """Constant-size aggregates; no list of requests or audio buffers is retained."""

    def __init__(self):
        self.counts = collections.Counter()
        self.metrics = {}
        self.previous_uptime = None
        self.uptime_resets = 0

    def observe(self, record):
        event = record["event"]
        self.counts[event] += 1
        uptime = record.get("uptime_ms")
        if uptime is not None:
            if self.previous_uptime is not None and uptime < self.previous_uptime:
                self.uptime_resets += 1
            self.previous_uptime = uptime
        for field, value in record.items():
            if not isinstance(value, int) or field in ("ts_ms", "uptime_ms", "stream_id", "connection_epoch"):
                continue
            key = ".".join((event, record.get("task", record.get("capability", "all")), field))
            metric = self.metrics.setdefault(key, {"samples": 0, "first": value, "last": value,
                                                    "min": value, "max": value})
            metric.update(samples=metric["samples"] + 1, last=value,
                          min=min(metric["min"], value), max=max(metric["max"], value))

    def result(self):
        return {"event_counts": dict(self.counts), "metrics": self.metrics,
                "observed_uptime_resets": self.uptime_resets,
                "acceptance": "manual_verification_required",
                "limitations": ["Software metadata does not prove audible completion.",
                                "Compare idle resource samples within one boot; peaks are not leak evidence.",
                                "No transcript, PCM, credentials, raw identifiers or unknown fields retained."]}


def collect(source, destination, deadline=None):
    summary = Summary()
    discarding = False
    with (destination / "events.jsonl").open("x", encoding="utf-8") as output:
        os.chmod(output.name, 0o600)
        try:
            while deadline is None or time.monotonic() < deadline:
                line = source.readline(MAX_LINE + 1)
                if not line:
                    if deadline is not None:
                        continue
                    break
                if isinstance(line, bytes):
                    line = line.decode("utf-8", errors="replace")
                if discarding or len(line) > MAX_LINE:
                    discarding = not line.endswith("\n")
                    continue
                record = parse_line(line)
                if record:
                    summary.observe(record)
                    output.write(json.dumps(record, ensure_ascii=True) + "\n")
                    output.flush()
        finally:
            with (destination / "summary.json").open("x", encoding="utf-8") as report:
                os.chmod(report.name, 0o600)
                json.dump(summary.result(), report, indent=2)
                report.write("\n")
    return summary.result()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--serial", help="serial device; requires ESP-IDF Python/pyserial")
    inputs.add_argument("--stdin", action="store_true", help="filter foreground Host stderr/stdout")
    inputs.add_argument("--file", type=Path, help="filter an existing log without modifying it")
    parser.add_argument("--duration", type=float, default=1800, help="serial seconds, 1..43200")
    parser.add_argument("--output-dir", required=True, type=Path, help="new private directory")
    args = parser.parse_args()
    if not 1 <= args.duration <= 43200:
        parser.error("duration must be 1..43200 seconds")
    serial_source = None
    try:
        if args.serial:
            import serial
            serial_source = serial.Serial(port=None, baudrate=115200, timeout=1, exclusive=True)
            serial_source.dtr = False
            serial_source.rts = False
            serial_source.port = args.serial
            serial_source.open()
        args.output_dir.mkdir(mode=0o700, parents=False, exist_ok=False)
        if serial_source is not None:
            collect(serial_source, args.output_dir, time.monotonic() + args.duration)
        elif args.stdin:
            collect(sys.stdin, args.output_dir)
        else:
            with args.file.open(encoding="utf-8", errors="replace") as source:
                collect(source, args.output_dir)
    except KeyboardInterrupt:
        pass
    finally:
        if serial_source is not None:
            serial_source.close()


if __name__ == "__main__":
    main()
