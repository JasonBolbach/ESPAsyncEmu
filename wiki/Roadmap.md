# Roadmap

```mermaid
flowchart LR
    p1["Phase 1<br/>Event loop"]:::done --> p2["Phase 2<br/>ESPAsyncTCP"]:::done
    p2 --> p3["Phase 3<br/>ESPAsyncUDP"]:::done
    p3 --> p4["Phase 4<br/>Examples + integrations"]:::next
    p4 --> p5["Phase 5<br/>ESPAsyncWebServer layer"]:::later
    classDef done fill:#cfe8cf,stroke:#4a8a4a,color:#000
    classDef next fill:#fff1c2,stroke:#b8920a,color:#000
    classDef later fill:#e6e6e6,stroke:#888,color:#000
```

## Done

- Event loop: `poll`, `pollFor`, `run`, `stop`, `post`, `reset`
- `AsyncClient` / `AsyncServer` (ESPAsyncTCP)
- `AsyncUDP` / `AsyncUDPPacket` / `AsyncUDPMessage` (ESPAsyncUDP)
- `IPAddress`
- Tests for all of the above, clean under ASan/UBSan

## Next

- `examples/`: TCP echo server, UDP ping/pong
- `integrations/`: hooking `poll()` into a GUI frame loop (SDL / ImGui)
- Wiki: keep pages in sync as features land

## Later

- ESPAsyncWebServer-style layer on top of `AsyncServer`
- Optional: IPv6, `isBroadcast()` support (needs `recvmsg` with packet info)

## Known limitations

See the "Differences" and "Limitations" sections of [ESPAsyncTCP](ESPAsyncTCP.md) and [ESPAsyncUDP](ESPAsyncUDP.md). Not yet verified: lwip error code numbering against real headers.
