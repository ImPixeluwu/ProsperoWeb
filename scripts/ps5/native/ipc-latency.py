#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarises the WebKit IPC hop timings in a ProsperoWeb log.txt.

usage: ipc-latency.py log.txt

Reads the "webkit: ipc fd N send|written|received|dispatch ... t=MS" lines
(patch 0018; t is CLOCK_MONOTONIC in ms, comparable across processes) and
prints, per process role and descriptor:
  - send -> dispatch of the reply side is not paired here; instead
  - received -> dispatch: how long a message waits for the receiving run loop,
  - send -> received across processes, paired by message name in order
    (UI -> child and child -> UI).
"""
import collections
import re
import statistics
import sys

LINE = re.compile(r"\[\s*\d+ (\w+)\] webkit: ipc fd (\d+) (send|received|dispatch) (\S+).* t=([\d.]+)")


def summary(values):
    if not values:
        return "-"
    values = sorted(values)
    return (f"n={len(values)} median {statistics.median(values):.1f} ms, "
            f"p90 {values[int(len(values) * 0.9) - 1 if len(values) > 1 else 0]:.1f} ms, "
            f"max {values[-1]:.1f} ms")


def main():
    events = collections.defaultdict(list)  # (role, fd, kind) -> [(name, t)]
    for line in open(sys.argv[1], errors="replace"):
        m = LINE.search(line)
        if m:
            role, fd, kind, name, t = m.groups()
            events[(role, int(fd), kind)].append((name, float(t)))

    print("received -> dispatched (run loop latency):")
    for (role, fd, kind), items in sorted(events.items()):
        if kind != "received":
            continue
        dispatched = collections.defaultdict(collections.deque)
        for name, t in events.get((role, fd, "dispatch"), []):
            dispatched[name].append(t)
        waits = []
        for name, t in items:
            if dispatched[name]:
                waits.append(dispatched[name].popleft() - t)
        print(f"  {role} fd {fd}: {summary(waits)}")

    print("send -> received (across processes, by message name):")
    for direction, sender_is_ui in (("UI -> children", True), ("children -> UI", False)):
        sent = collections.defaultdict(collections.deque)
        received = []
        for (role, fd, kind), items in events.items():
            is_ui = role == "UI"
            if kind == "send" and is_ui == sender_is_ui:
                for name, t in items:
                    sent[name].append(t)
            if kind == "received" and is_ui != sender_is_ui:
                received.extend(items)
        delays = []
        for name, t in sorted(received, key=lambda item: item[1]):
            while sent[name] and sent[name][0] > t:
                break
            if sent[name] and sent[name][0] <= t:
                delays.append(t - sent[name].popleft())
        print(f"  {direction}: {summary(delays)}")


main()
