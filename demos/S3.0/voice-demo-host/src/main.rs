mod agent;
mod speech;

use std::{env, net::SocketAddr, time::Instant};

use agent::{AgentProvider, DashScopeSdkAgent};
use axum::{
    Json, Router,
    extract::{
        State,
        ws::{Message, WebSocket, WebSocketUpgrade},
    },
    response::Response,
    routing::get,
};
use serde::Deserialize;
use serde_json::{Value, json};
use speech::{DashScopeSpeech, SpeechProviders};

const DEMO_PROTOCOL_MAJOR: u16 = 0;
const DEMO_PROTOCOL_MINOR: u16 = 1;
const PCM_SAMPLE_RATE_HZ: u32 = 16_000;
const PCM_CHANNELS: u8 = 1;
const PCM_BITS_PER_SAMPLE: u8 = 16;
const DEFAULT_BIND: &str = "0.0.0.0:8787";
const DEFAULT_MAX_UTTERANCE_BYTES: usize = 16_000 * 2 * 60;
const MAX_PLAYBACK_BYTES: usize = 16_000 * 2 * 10;
const MAX_TTS_REPLY_CHARS: usize = 36;
const PLAYBACK_CHUNK_BYTES: usize = 640;

#[derive(Clone)]
struct AppState {
    max_utterance_bytes: usize,
    agent: AgentProvider,
    speech: SpeechProviders,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum ControlMessage {
    Hello {
        protocol_major: u16,
        protocol_minor: u16,
        device_id: String,
    },
    ListenStart {
        utterance_id: String,
    },
    ListenStop {
        utterance_id: String,
    },
    PlaybackCancel {
        audio_stream_id: String,
    },
    Ping {
        #[serde(default)]
        nonce: Option<String>,
    },
}

struct ActiveUtterance {
    utterance_id: String,
    pcm: Vec<u8>,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    dotenvy::dotenv().ok();
    let bind = env::var("VOICE_DEMO_BIND").unwrap_or_else(|_| DEFAULT_BIND.to_owned());
    let address: SocketAddr = bind.parse()?;
    let speech = DashScopeSpeech::from_environment()?.providers();
    let agent = DashScopeSdkAgent::from_environment()?.provider();
    let state = AppState {
        max_utterance_bytes: DEFAULT_MAX_UTTERANCE_BYTES,
        agent,
        speech,
    };
    println!("Voice Demo Host listening on ws://{address}/ws");
    println!("Authentication is disabled for this development-only demo.");
    println!("agent: {}", state.agent.description);
    println!("speech: {}", state.speech.description);

    let app = Router::new()
        .route("/health", get(health))
        .route("/ws", get(upgrade_websocket))
        .with_state(state);
    let listener = tokio::net::TcpListener::bind(address).await?;

    axum::serve(listener, app).await?;
    Ok(())
}

async fn health(State(state): State<AppState>) -> Json<Value> {
    Json(json!({
        "status": "ready",
        "demo_protocol": {
            "major": DEMO_PROTOCOL_MAJOR,
            "minor": DEMO_PROTOCOL_MINOR,
        },
        "authentication": "disabled",
        "agent": state.agent.description.as_ref(),
        "speech": state.speech.description.as_ref(),
    }))
}

async fn upgrade_websocket(websocket: WebSocketUpgrade, State(state): State<AppState>) -> Response {
    websocket.on_upgrade(move |socket| handle_socket(socket, state))
}

async fn handle_socket(mut socket: WebSocket, state: AppState) {
    let mut active: Option<ActiveUtterance> = None;

    while let Some(result) = socket.recv().await {
        let message = match result {
            Ok(message) => message,
            Err(error) => {
                eprintln!("WebSocket receive failed: {error}");
                return;
            }
        };

        let send_result = match message {
            Message::Text(text) => {
                handle_control(&mut socket, &mut active, &state, text.as_str()).await
            }
            Message::Binary(pcm) => {
                handle_pcm(&mut socket, &mut active, &state, pcm.as_ref()).await
            }
            Message::Ping(payload) => socket.send(Message::Pong(payload)).await,
            Message::Pong(_) => Ok(()),
            Message::Close(_) => return,
        };

        if let Err(error) = send_result {
            eprintln!("WebSocket send failed: {error}");
            return;
        }
    }
}

async fn handle_control(
    socket: &mut WebSocket,
    active: &mut Option<ActiveUtterance>,
    state: &AppState,
    text: &str,
) -> Result<(), axum::Error> {
    let message = match serde_json::from_str::<ControlMessage>(text) {
        Ok(message) => message,
        Err(_) => {
            return send_json(
                socket,
                json!({
                    "type": "error",
                    "code": "invalid_control_message",
                    "message": "control message is not valid demo protocol JSON",
                }),
            )
            .await;
        }
    };

    match message {
        ControlMessage::Hello {
            protocol_major,
            protocol_minor,
            device_id,
        } => {
            if protocol_major != DEMO_PROTOCOL_MAJOR {
                return send_json(
                    socket,
                    json!({
                        "type": "error",
                        "code": "protocol_major_mismatch",
                        "supported_protocol_major": DEMO_PROTOCOL_MAJOR,
                    }),
                )
                .await;
            }
            send_json(
                socket,
                json!({
                    "type": "hello_ack",
                    "protocol_major": DEMO_PROTOCOL_MAJOR,
                    "protocol_minor": DEMO_PROTOCOL_MINOR.min(protocol_minor),
                    "device_id": device_id,
                    "authentication": "disabled",
                    "audio": pcm_format(),
                    "max_utterance_bytes": state.max_utterance_bytes,
                }),
            )
            .await
        }
        ControlMessage::ListenStart { utterance_id } => {
            if active.is_some() {
                return send_error(socket, "utterance_already_active", Some(&utterance_id)).await;
            }
            *active = Some(ActiveUtterance {
                utterance_id: utterance_id.clone(),
                pcm: Vec::new(),
            });
            send_json(
                socket,
                json!({
                    "type": "state_changed",
                    "state": "listening",
                    "utterance_id": utterance_id,
                }),
            )
            .await
        }
        ControlMessage::ListenStop { utterance_id } => {
            let Some(current) = active.as_ref() else {
                return send_error(socket, "no_active_utterance", Some(&utterance_id)).await;
            };
            if current.utterance_id != utterance_id {
                return send_error(socket, "utterance_id_mismatch", Some(&utterance_id)).await;
            }

            let completed = active.take().expect("active utterance was checked above");
            if completed.pcm.is_empty() {
                send_error(socket, "empty_utterance", Some(&utterance_id)).await?;
                return send_idle(socket).await;
            }
            process_speech(socket, completed, state).await
        }
        ControlMessage::PlaybackCancel { audio_stream_id } => {
            send_json(
                socket,
                json!({
                    "type": "playback_end",
                    "audio_stream_id": audio_stream_id,
                    "reason": "cancelled",
                }),
            )
            .await?;
            send_idle(socket).await
        }
        ControlMessage::Ping { nonce } => {
            send_json(socket, json!({ "type": "pong", "nonce": nonce })).await
        }
    }
}

async fn handle_pcm(
    socket: &mut WebSocket,
    active: &mut Option<ActiveUtterance>,
    state: &AppState,
    pcm: &[u8],
) -> Result<(), axum::Error> {
    let Some(current) = active.as_mut() else {
        return send_error(socket, "pcm_without_active_utterance", None).await;
    };
    if current.pcm.len().saturating_add(pcm.len()) > state.max_utterance_bytes {
        let utterance_id = current.utterance_id.clone();
        *active = None;
        send_error(socket, "utterance_too_large", Some(&utterance_id)).await?;
        return send_idle(socket).await;
    }
    current.pcm.extend_from_slice(pcm);
    Ok(())
}

async fn process_speech(
    socket: &mut WebSocket,
    completed: ActiveUtterance,
    state: &AppState,
) -> Result<(), axum::Error> {
    send_json(
        socket,
        json!({
            "type": "state_changed",
            "state": "processing",
            "stage": "asr",
            "utterance_id": completed.utterance_id,
        }),
    )
    .await?;

    let asr_started = Instant::now();
    let asr = match state.speech.asr.transcribe(&completed.pcm).await {
        Ok(result) => result,
        Err(error) => {
            eprintln!("ASR failed: {error}");
            send_error(socket, "asr_failed", Some(&completed.utterance_id)).await?;
            return send_idle(socket).await;
        }
    };
    println!(
        "ASR completed: utterance={} chars={} elapsed_ms={} request_id={}",
        completed.utterance_id,
        asr.transcript.chars().count(),
        asr_started.elapsed().as_millis(),
        asr.request_id.as_deref().unwrap_or("unavailable")
    );
    send_json(
        socket,
        json!({
            "type": "transcript",
            "utterance_id": completed.utterance_id,
            "text": asr.transcript,
        }),
    )
    .await?;

    send_json(
        socket,
        json!({
            "type": "state_changed",
            "state": "processing",
            "stage": "agent",
            "utterance_id": completed.utterance_id,
        }),
    )
    .await?;
    let agent_started = Instant::now();
    let agent_reply = match state.agent.reply.reply(&asr.transcript).await {
        Ok(result) => result,
        Err(error) => {
            eprintln!("Agent failed: {error}");
            send_error(socket, "agent_failed", Some(&completed.utterance_id)).await?;
            return send_idle(socket).await;
        }
    };
    let reply = bounded_agent_reply(&agent_reply.text);
    println!(
        "Agent completed: utterance={} chars={} spoken_chars={} elapsed_ms={}",
        completed.utterance_id,
        agent_reply.text.chars().count(),
        reply.chars().count(),
        agent_started.elapsed().as_millis(),
    );
    send_json(
        socket,
        json!({
            "type": "assistant_text",
            "utterance_id": completed.utterance_id,
            "text": reply,
        }),
    )
    .await?;
    send_json(
        socket,
        json!({
            "type": "state_changed",
            "state": "processing",
            "stage": "tts",
            "utterance_id": completed.utterance_id,
        }),
    )
    .await?;

    let tts_started = Instant::now();
    let tts = match state.speech.tts.synthesize(&reply).await {
        Ok(result) => result,
        Err(error) => {
            eprintln!("TTS failed: {error}");
            send_error(socket, "tts_failed", Some(&completed.utterance_id)).await?;
            return send_idle(socket).await;
        }
    };
    if tts.pcm.len() > MAX_PLAYBACK_BYTES {
        eprintln!(
            "TTS audio exceeds firmware buffer: bytes={} limit={}",
            tts.pcm.len(),
            MAX_PLAYBACK_BYTES
        );
        send_error(socket, "tts_audio_too_large", Some(&completed.utterance_id)).await?;
        return send_idle(socket).await;
    }
    println!(
        "TTS completed: utterance={} bytes={} elapsed_ms={} request_id={}",
        completed.utterance_id,
        tts.pcm.len(),
        tts_started.elapsed().as_millis(),
        tts.request_id.as_deref().unwrap_or("unavailable")
    );

    let audio_stream_id = format!("tts-{}", completed.utterance_id);
    send_json(
        socket,
        json!({
            "type": "playback_start",
            "audio_stream_id": audio_stream_id,
            "utterance_id": completed.utterance_id,
            "audio": pcm_format(),
            "byte_length": tts.pcm.len(),
        }),
    )
    .await?;
    for chunk in tts.pcm.chunks(PLAYBACK_CHUNK_BYTES) {
        socket.send(Message::Binary(chunk.to_vec().into())).await?;
    }
    send_json(
        socket,
        json!({
            "type": "playback_end",
            "audio_stream_id": audio_stream_id,
            "reason": "completed",
        }),
    )
    .await?;
    send_idle(socket).await
}

fn bounded_agent_reply(reply: &str) -> String {
    reply.chars().take(MAX_TTS_REPLY_CHARS).collect()
}

async fn send_idle(socket: &mut WebSocket) -> Result<(), axum::Error> {
    send_json(socket, json!({ "type": "state_changed", "state": "idle" })).await
}

async fn send_error(
    socket: &mut WebSocket,
    code: &str,
    utterance_id: Option<&str>,
) -> Result<(), axum::Error> {
    send_json(
        socket,
        json!({
            "type": "error",
            "code": code,
            "utterance_id": utterance_id,
        }),
    )
    .await
}

async fn send_json(socket: &mut WebSocket, value: Value) -> Result<(), axum::Error> {
    socket.send(Message::Text(value.to_string().into())).await
}

fn pcm_format() -> Value {
    json!({
        "encoding": "pcm_s16le",
        "sample_rate_hz": PCM_SAMPLE_RATE_HZ,
        "channels": PCM_CHANNELS,
        "bits_per_sample": PCM_BITS_PER_SAMPLE,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_minimal_hello() {
        let message = serde_json::from_str::<ControlMessage>(
            r#"{"type":"hello","protocol_major":0,"protocol_minor":1,"device_id":"board-1"}"#,
        )
        .expect("hello should parse");

        assert!(matches!(
            message,
            ControlMessage::Hello {
                protocol_major: 0,
                protocol_minor: 1,
                device_id,
            } if device_id == "board-1"
        ));
    }

    #[test]
    fn pcm_format_matches_firmware_baseline() {
        assert_eq!(pcm_format()["encoding"], "pcm_s16le");
        assert_eq!(pcm_format()["sample_rate_hz"], 16_000);
        assert_eq!(pcm_format()["channels"], 1);
        assert_eq!(PLAYBACK_CHUNK_BYTES, 640);
    }

    #[test]
    fn agent_reply_is_bounded_by_characters() {
        let reply = bounded_agent_reply(&"你".repeat(100));
        assert_eq!(reply.chars().count(), MAX_TTS_REPLY_CHARS);
    }
}
