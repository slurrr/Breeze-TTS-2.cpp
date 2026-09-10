# WebSocket API

The HTTP endpoint already streams audio and already stops generating when the
client disconnects, so the socket exists for the things chunked HTTP cannot do:
feeding text in as it arrives, retuning delivery partway through, interrupting
without tearing the connection down, and holding a session open across many
lines.

cpp-httplib has no WebSocket support and does not hand over the socket after an
upgrade, so this is RFC 6455 implemented directly on its own listener. That is
why it is a second port rather than a route.

## Connecting

The port defaults to the HTTP port plus one. `--ws-port` moves it, `--ws-port -1`
turns it off. `GET /health` reports where it ended up so clients can find it
without being told:

```json
{"status":"ok","sample_rate":24000,"ws_port":8081}
```

```
ws://127.0.0.1:8081
```

**Text frames are JSON control messages, binary frames are audio.** WebSocket
already distinguishes the two at the framing layer, so audio needs no envelope
and no base64. The audio is headerless signed 16 bit little endian mono PCM at
the rate given in the `ready` message, exactly like the HTTP body.

## Messages you send

| `type` | Fields | Meaning |
| --- | --- | --- |
| `start` | `voice_id`, `use_default_voice`, `instruction`, `ref_text`, `cfg_scale`, `seed`, `temperature`, `top_k`, `split_chars`, `stream_chars` | Opens a session. Encodes the reference once for all of it. Set `use_default_voice` to `0` to force voice design. |
| `text` | `text` | Adds text. Completed text is spoken according to the session's streaming buffer policy. |
| `flush` | `text` | Speaks what is buffered even without a sentence ending. |
| `instruction` | `instruction` | Changes delivery from the next piece onwards. |
| `cancel` | | Stops mid sentence and throws away anything buffered. |
| `end` | `text` | Adds any last text, speaks everything, then reports `done`. |

`start` takes a `voice_id`, not a file. Reference clips are uploaded over HTTP to
`POST /v1/voices`, which hands back an id. See [voices.md](voices.md). Leaving
`voice_id` out uses the provider's persisted default voice when one is selected,
or voice design driven by `instruction` when no default is set.

## Messages you receive

| `type` | Fields | Meaning |
| --- | --- | --- |
| `ready` | `sample_rate`, `format` | Connection is up. Send `start`. |
| `started` | `voice_id` | Session is open. |
| `speaking` | `text` | The piece about to be generated. |
| `queued` | | Another connection holds the GPU. Waiting, not refused. |
| `instruction_set` | | The new delivery applies to text sent from here on. |
| `cancelled` | | Generation was interrupted. |
| `done` | | Everything buffered has been spoken. |
| `error` | `message` | Something was wrong with the request. |

## A session

```json
-> {"type":"start","voice_id":"harbour","seed":7}
<- {"type":"started","voice_id":"harbour"}
-> {"type":"text","text":"The harbour was quiet that morning. "}
<- {"type":"speaking","text":"The harbour was quiet that morning."}
<- <binary audio>
-> {"type":"instruction","instruction":"Speak in an urgent, alarmed whisper."}
<- {"type":"instruction_set"}
-> {"type":"end","text":"Then the alarm went off."}
<- {"type":"speaking","text":"Then the alarm went off."}
<- <binary audio>
<- {"type":"done"}
```

## Feeding text as it arrives

Send an LLM's output verbatim as `text` messages. Breeze owns the provider-side
buffer and chooses complete text pieces; clients should not tokenize or split the
text themselves.

`stream_chars` controls the latency/continuity tradeoff. It defaults to the
server's `--stream-chars` setting:

- `0` drains each completed sentence for the lowest text-to-audio latency.
- A positive value waits for roughly that much text, then drains at the latest
  useful sentence boundary. This gives each model pass several sentences of
  context and substantially better paragraph-level prosody while remaining
  incremental. The value is capped at `split_chars`.

`split_chars` is the hard model-safe piece budget. If streamed text reaches that
budget without useful punctuation, Breeze breaks it on a space. `flush` forces
all currently buffered text, and `end` always drains the complete remainder
before `done`. The WebSocket budget falls back to 600 when `split_chars` is `0`,
because an open-ended stream still needs a safe maximum.

While there is no reference clip, the opening piece doubles as the reference for
everything after it, so it is capped near a normal clip length. A long first
piece makes the model start skipping sentences later.

## Changing delivery partway

An `instruction` message applies to every piece queued **after** it. Each piece
remembers the instruction that was live when it was queued, so pieces already
waiting, and the one being generated, finish with the delivery they were sent
under. That means a direction dropped between two sentences lands on exactly the
sentence that follows it, even when text is arriving faster than the GPU is
speaking it and the queue is several pieces deep.

The effect is real but it is a nudge, not a transformation. Same sentence, same
seed, measured on the isolated piece:

| Instruction | RMS | Spectral centroid |
| --- | --- | --- |
| `Speak clearly and naturally.` | 0.1438 | 1733 Hz |
| `Speak in a breathy, hushed whisper.` | 0.1126 | 1895 Hz |

Quieter and brighter, which is what a whisper should do. With a cloned reference
the model deliberately protects the reference's timbre, so instructions move
delivery rather than rebuild the voice. Voice design sessions, which have no
reference, respond more strongly.

## Interrupting

`cancel` stops the current piece part way through rather than after it finishes,
and clears anything still buffered. The audio already sent stays valid, so a
client can keep and play what it received.

The session stays open. Send more text and it carries on with the same voice.

## Concurrency

Generation runs one at a time on the GPU. Unlike the HTTP endpoints, which answer
`409` when busy, a socket session **waits its turn** and reports `queued` while it
does. Each piece takes the lock separately, so two connections interleave between
pieces instead of one blocking the other for an entire passage.

Each connection reads and generates on separate threads, which is what lets
`cancel` and `instruction` arrive while audio is still being produced.

## Security

There is no authentication, no origin check and no rate limit on the socket. It
binds to `--host`, which defaults to `127.0.0.1`. Binding it to `0.0.0.0` hands
anyone on the network unrestricted use of the GPU. Put it behind a reverse proxy
that terminates TLS and handles auth before exposing it.

## Client sketch

```python
import asyncio, json, websockets

async def speak(lines):
    async with websockets.connect("ws://127.0.0.1:8081", max_size=None) as ws:
        await ws.recv()  # ready
        await ws.send(json.dumps({"type": "start", "voice_id": "harbour", "seed": 7}))

        async def send():
            for line in lines:
                await ws.send(json.dumps({"type": "text", "text": line}))
            await ws.send(json.dumps({"type": "end", "text": ""}))

        asyncio.create_task(send())
        audio = bytearray()
        async for msg in ws:
            if isinstance(msg, bytes):
                audio.extend(msg)
            elif json.loads(msg)["type"] in ("done", "error"):
                break
        return bytes(audio)
```
