use std::{env, sync::Arc, time::Duration};

use async_trait::async_trait;
use base64::{Engine as _, engine::general_purpose::STANDARD as BASE64};
use reqwest::{Client, StatusCode};
use serde_json::{Value, json};
use thiserror::Error;

const DEFAULT_BASE_URL: &str = "https://dashscope.aliyuncs.com";
const DEFAULT_ASR_MODEL: &str = "qwen-audio-3.0-asr-flash";
const DEFAULT_TTS_MODEL: &str = "qwen-audio-3.0-tts-flash";
const DEFAULT_TTS_VOICE: &str = "longanhuan_v3.6";
const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);

#[derive(Debug)]
pub struct AsrResult {
    pub transcript: String,
    pub request_id: Option<String>,
}

#[derive(Debug)]
pub struct TtsResult {
    pub pcm: Vec<u8>,
    pub request_id: Option<String>,
}

#[derive(Debug, Error)]
pub enum SpeechError {
    #[error("missing environment variable {0}")]
    MissingEnvironment(&'static str),
    #[error("invalid PCM16 byte length")]
    InvalidPcmLength,
    #[error("provider request failed: {0}")]
    Transport(#[from] reqwest::Error),
    #[error("provider returned HTTP {0}")]
    Http(StatusCode),
    #[error("provider response did not contain {0}")]
    MissingResponseField(&'static str),
    #[error("provider returned an invalid TTS audio URL")]
    InvalidAudioUrl,
    #[error("provider returned empty audio")]
    EmptyAudio,
}

#[async_trait]
pub trait AsrProvider: Send + Sync {
    async fn transcribe(&self, pcm_s16le: &[u8]) -> Result<AsrResult, SpeechError>;
}

#[async_trait]
pub trait TtsProvider: Send + Sync {
    async fn synthesize(&self, text: &str) -> Result<TtsResult, SpeechError>;
}

#[derive(Clone)]
pub struct SpeechProviders {
    pub asr: Arc<dyn AsrProvider>,
    pub tts: Arc<dyn TtsProvider>,
    pub description: Arc<str>,
}

#[derive(Clone)]
pub struct DashScopeSpeech {
    client: Client,
    api_key: Arc<str>,
    base_url: Arc<str>,
    asr_model: Arc<str>,
    tts_model: Arc<str>,
    tts_voice: Arc<str>,
}

impl DashScopeSpeech {
    pub fn from_environment() -> Result<Self, SpeechError> {
        let api_key = env::var("DASHSCOPE_API_KEY")
            .ok()
            .filter(|value| !value.trim().is_empty())
            .ok_or(SpeechError::MissingEnvironment("DASHSCOPE_API_KEY"))?;
        let base_url = env::var("DASHSCOPE_BASE_URL")
            .unwrap_or_else(|_| DEFAULT_BASE_URL.to_owned())
            .trim_end_matches('/')
            .to_owned();
        let asr_model =
            env::var("DASHSCOPE_ASR_MODEL").unwrap_or_else(|_| DEFAULT_ASR_MODEL.to_owned());
        let tts_model =
            env::var("DASHSCOPE_TTS_MODEL").unwrap_or_else(|_| DEFAULT_TTS_MODEL.to_owned());
        let tts_voice =
            env::var("DASHSCOPE_TTS_VOICE").unwrap_or_else(|_| DEFAULT_TTS_VOICE.to_owned());
        let client = Client::builder().timeout(REQUEST_TIMEOUT).build()?;

        Ok(Self {
            client,
            api_key: Arc::from(api_key),
            base_url: Arc::from(base_url),
            asr_model: Arc::from(asr_model),
            tts_model: Arc::from(tts_model),
            tts_voice: Arc::from(tts_voice),
        })
    }

    pub fn providers(self) -> SpeechProviders {
        let description: Arc<str> = Arc::from(format!(
            "dashscope asr={} tts={} voice={}",
            self.asr_model, self.tts_model, self.tts_voice
        ));
        let provider = Arc::new(self);
        SpeechProviders {
            asr: provider.clone(),
            tts: provider,
            description,
        }
    }

    fn endpoint(&self, path: &str) -> String {
        format!("{}{}", self.base_url, path)
    }

    fn authorization(&self) -> String {
        format!("Bearer {}", self.api_key)
    }
}

#[async_trait]
impl AsrProvider for DashScopeSpeech {
    async fn transcribe(&self, pcm_s16le: &[u8]) -> Result<AsrResult, SpeechError> {
        let wav = pcm16_mono_wav(pcm_s16le, 16_000)?;
        let audio = format!("data:audio/wav;base64,{}", BASE64.encode(wav));
        let payload = json!({
            "model": self.asr_model.as_ref(),
            "input": {
                "messages": [{
                    "role": "user",
                    "content": [{
                        "type": "input_audio",
                        "input_audio": { "data": audio }
                    }]
                }]
            },
            "parameters": {
                "format": "wav",
                "sample_rate": "16000"
            }
        });
        let response = self
            .client
            .post(self.endpoint("/api/v1/services/aigc/multimodal-generation/generation"))
            .header("Authorization", self.authorization())
            .header("X-DashScope-SSE", "disable")
            .json(&payload)
            .send()
            .await?;
        let status = response.status();
        if !status.is_success() {
            return Err(SpeechError::Http(status));
        }
        let body: Value = response.json().await?;
        let transcript = first_non_empty_string(&body, &["/output/text", "/output/output/text"])
            .ok_or(SpeechError::MissingResponseField("ASR transcript"))?;
        Ok(AsrResult {
            transcript,
            request_id: request_id(&body),
        })
    }
}

#[async_trait]
impl TtsProvider for DashScopeSpeech {
    async fn synthesize(&self, text: &str) -> Result<TtsResult, SpeechError> {
        let payload = json!({
            "model": self.tts_model.as_ref(),
            "input": {
                "text": text,
                "voice": self.tts_voice.as_ref(),
                "format": "pcm",
                "sample_rate": 16000,
                "language_hints": ["zh"]
            }
        });
        let response = self
            .client
            .post(self.endpoint("/api/v1/services/audio/tts/SpeechSynthesizer"))
            .header("Authorization", self.authorization())
            .json(&payload)
            .send()
            .await?;
        let status = response.status();
        if !status.is_success() {
            return Err(SpeechError::Http(status));
        }
        let body: Value = response.json().await?;
        let audio_url = first_non_empty_string(&body, &["/output/audio/url"])
            .ok_or(SpeechError::MissingResponseField("TTS audio URL"))?;
        let audio_url = secure_dashscope_audio_url(&audio_url)?;
        let audio_response = self.client.get(audio_url).send().await?;
        let status = audio_response.status();
        if !status.is_success() {
            return Err(SpeechError::Http(status));
        }
        let pcm = audio_response.bytes().await?.to_vec();
        if pcm.is_empty() {
            return Err(SpeechError::EmptyAudio);
        }
        if !pcm.len().is_multiple_of(2) {
            return Err(SpeechError::InvalidPcmLength);
        }
        Ok(TtsResult {
            pcm,
            request_id: request_id(&body),
        })
    }
}

fn first_non_empty_string(body: &Value, pointers: &[&str]) -> Option<String> {
    pointers.iter().find_map(|pointer| {
        body.pointer(pointer)
            .and_then(Value::as_str)
            .map(str::trim)
            .filter(|value| !value.is_empty())
            .map(ToOwned::to_owned)
    })
}

fn request_id(body: &Value) -> Option<String> {
    first_non_empty_string(
        body,
        &[
            "/request_id",
            "/output/request_id",
            "/output/output/request_id",
        ],
    )
}

fn secure_dashscope_audio_url(value: &str) -> Result<reqwest::Url, SpeechError> {
    let mut url = reqwest::Url::parse(value).map_err(|_| SpeechError::InvalidAudioUrl)?;
    let trusted_host = url
        .host_str()
        .is_some_and(|host| host.ends_with(".oss-cn-beijing.aliyuncs.com"));
    if !trusted_host {
        return Err(SpeechError::InvalidAudioUrl);
    }
    match url.scheme() {
        "https" => Ok(url),
        "http" => {
            url.set_scheme("https")
                .map_err(|_| SpeechError::InvalidAudioUrl)?;
            Ok(url)
        }
        _ => Err(SpeechError::InvalidAudioUrl),
    }
}

fn pcm16_mono_wav(pcm: &[u8], sample_rate_hz: u32) -> Result<Vec<u8>, SpeechError> {
    if !pcm.len().is_multiple_of(2) {
        return Err(SpeechError::InvalidPcmLength);
    }
    let data_length = u32::try_from(pcm.len()).map_err(|_| SpeechError::InvalidPcmLength)?;
    let riff_length = data_length
        .checked_add(36)
        .ok_or(SpeechError::InvalidPcmLength)?;
    let byte_rate = sample_rate_hz * 2;
    let mut wav = Vec::with_capacity(44 + pcm.len());
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&riff_length.to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16_u32.to_le_bytes());
    wav.extend_from_slice(&1_u16.to_le_bytes());
    wav.extend_from_slice(&1_u16.to_le_bytes());
    wav.extend_from_slice(&sample_rate_hz.to_le_bytes());
    wav.extend_from_slice(&byte_rate.to_le_bytes());
    wav.extend_from_slice(&2_u16.to_le_bytes());
    wav.extend_from_slice(&16_u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&data_length.to_le_bytes());
    wav.extend_from_slice(pcm);
    Ok(wav)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wraps_pcm_in_canonical_mono_wav() {
        let wav = pcm16_mono_wav(&[1, 2, 3, 4], 16_000).expect("valid PCM");
        assert_eq!(&wav[0..4], b"RIFF");
        assert_eq!(u32::from_le_bytes(wav[4..8].try_into().unwrap()), 40);
        assert_eq!(&wav[8..12], b"WAVE");
        assert_eq!(u32::from_le_bytes(wav[24..28].try_into().unwrap()), 16_000);
        assert_eq!(u16::from_le_bytes(wav[34..36].try_into().unwrap()), 16);
        assert_eq!(u32::from_le_bytes(wav[40..44].try_into().unwrap()), 4);
        assert_eq!(&wav[44..], &[1, 2, 3, 4]);
    }

    #[test]
    fn rejects_odd_pcm_byte_count() {
        assert!(matches!(
            pcm16_mono_wav(&[1, 2, 3], 16_000),
            Err(SpeechError::InvalidPcmLength)
        ));
    }

    #[test]
    fn parses_both_observed_asr_response_shapes() {
        let direct = json!({"output": {"text": "你好"}});
        let nested = json!({"output": {"output": {"text": "世界"}}});
        let pointers = ["/output/text", "/output/output/text"];
        assert_eq!(
            first_non_empty_string(&direct, &pointers).as_deref(),
            Some("你好")
        );
        assert_eq!(
            first_non_empty_string(&nested, &pointers).as_deref(),
            Some("世界")
        );
    }

    #[test]
    fn upgrades_trusted_dashscope_audio_url_to_https() {
        let url = secure_dashscope_audio_url(
            "http://dashscope-result-bj.oss-cn-beijing.aliyuncs.com/audio.pcm?token=hidden",
        )
        .expect("trusted URL");
        assert_eq!(url.scheme(), "https");
        assert_eq!(
            url.host_str(),
            Some("dashscope-result-bj.oss-cn-beijing.aliyuncs.com")
        );
    }

    #[test]
    fn rejects_untrusted_audio_url() {
        assert!(matches!(
            secure_dashscope_audio_url("http://127.0.0.1/private"),
            Err(SpeechError::InvalidAudioUrl)
        ));
    }
}
