use std::{
    env,
    future::Future,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::Duration,
};

use agent_openai_compatible::{
    BearerCredential, ChatProtocolAdapter, OpenAiChatCompletionsService, TransportTimeouts,
};
use agent_sdk::{
    Agent, AgentBuilder, AllowAllAuthorizer, ContextWindowEvaluator, ExecutionBudget,
    ExecutionInput, ExecutionOutcome, ModelRequestConfig, SystemPromptSnapshot,
};
use agent_types::{
    AssistantPart, ConversationMessage, ConversationSnapshot, MessageId, PartId, ProviderId,
    TextPart, UserMessage, UserPart,
};
use async_trait::async_trait;
use thiserror::Error;
use tokio::time::timeout;
use tokio_util::sync::CancellationToken;

const DEFAULT_BASE_URL: &str = "https://dashscope.aliyuncs.com/compatible-mode/v1";
const DEFAULT_MODEL: &str = "qwen-plus";
const DEFAULT_CONTEXT_WINDOW_TOKENS: u64 = 32_768;
const DEFAULT_TIMEOUT_SECONDS: u64 = 20;
const MAX_OUTPUT_TOKENS: u32 = 96;

pub struct AgentReply {
    pub text: String,
}

#[derive(Debug, Error)]
pub enum AgentReplyError {
    #[error("missing environment variable {0}")]
    MissingEnvironment(&'static str),
    #[error("agent context window must be a positive integer")]
    InvalidContextWindow,
    #[error("agent timeout must be between 1 and 60 seconds")]
    InvalidTimeout,
    #[error("agent provider configuration failed: {0}")]
    Provider(String),
    #[error("agent construction failed: {0}")]
    Build(String),
    #[error("agent execution failed: {0}")]
    Execution(String),
    #[error("agent execution was cancelled")]
    Cancelled,
    #[error("agent requested context compaction")]
    CompactionRequired,
    #[error("agent requested runtime continuation")]
    ContinuationRequired,
    #[error("agent execution timed out")]
    Timeout,
    #[error("agent returned no user-facing text")]
    EmptyReply,
}

#[async_trait]
pub trait AgentReplyProvider: Send + Sync {
    async fn reply(&self, transcript: &str) -> Result<AgentReply, AgentReplyError>;
}

#[derive(Clone)]
pub struct AgentProvider {
    pub reply: Arc<dyn AgentReplyProvider>,
    pub description: Arc<str>,
}

pub struct DashScopeSdkAgent {
    agent: Agent,
    timeout: Duration,
    next_message: AtomicU64,
    model: String,
}

impl DashScopeSdkAgent {
    pub fn from_environment() -> Result<Self, AgentReplyError> {
        let api_key = env::var("DASHSCOPE_API_KEY")
            .ok()
            .filter(|value| !value.trim().is_empty())
            .ok_or(AgentReplyError::MissingEnvironment("DASHSCOPE_API_KEY"))?;
        let base_url =
            optional_env("DASHSCOPE_LLM_BASE_URL").unwrap_or_else(|| DEFAULT_BASE_URL.to_owned());
        let model = optional_env("DASHSCOPE_LLM_MODEL").unwrap_or_else(|| DEFAULT_MODEL.to_owned());
        let context_window_tokens = optional_env("DASHSCOPE_LLM_CONTEXT_WINDOW_TOKENS")
            .map(|value| value.parse::<u64>())
            .transpose()
            .map_err(|_| AgentReplyError::InvalidContextWindow)?
            .unwrap_or(DEFAULT_CONTEXT_WINDOW_TOKENS);
        if context_window_tokens == 0 {
            return Err(AgentReplyError::InvalidContextWindow);
        }
        let timeout_seconds = optional_env("VOICE_DEMO_AGENT_TIMEOUT_SECONDS")
            .map(|value| value.parse::<u64>())
            .transpose()
            .map_err(|_| AgentReplyError::InvalidTimeout)?
            .unwrap_or(DEFAULT_TIMEOUT_SECONDS);
        if !(1..=60).contains(&timeout_seconds) {
            return Err(AgentReplyError::InvalidTimeout);
        }

        let provider = OpenAiChatCompletionsService::new(
            base_url,
            BearerCredential::new(api_key),
            model.clone(),
            context_window_tokens,
            ChatProtocolAdapter::openai_compatible(
                ProviderId::new("dashscope")
                    .map_err(|error| AgentReplyError::Provider(error.to_string()))?,
            ),
            TransportTimeouts {
                connect: Duration::from_secs(5),
                request: Duration::from_secs(timeout_seconds),
            },
        )
        .map_err(|error| AgentReplyError::Provider(error.to_string()))?;

        let mut model_request = ModelRequestConfig::default();
        model_request.generation.temperature = Some(0.4);
        model_request.generation.max_output_tokens = Some(MAX_OUTPUT_TOKENS);
        let agent = AgentBuilder::new(
            Arc::new(provider),
            SystemPromptSnapshot::new(vec![
                "你是桌面语音助手的链路验证版本。直接用自然、简短的中文回答用户，不使用 Markdown，不复述问题，回复最多 36 个汉字。"
                    .to_owned(),
            ]),
            Arc::new(
                ContextWindowEvaluator::new(0.8)
                    .map_err(|error| AgentReplyError::Build(error.to_string()))?,
            ),
        )
        .model_request(model_request)
        .budget(ExecutionBudget {
            max_steps: Some(1),
            max_tool_calls: Some(0),
        })
        .build()
        .map_err(|error| AgentReplyError::Build(error.to_string()))?;

        Ok(Self {
            agent,
            timeout: Duration::from_secs(timeout_seconds),
            next_message: AtomicU64::new(1),
            model,
        })
    }

    pub fn provider(self) -> AgentProvider {
        let description: Arc<str> = Arc::from(format!(
            "agent-sdk ephemeral provider=dashscope model={}",
            self.model
        ));
        AgentProvider {
            reply: Arc::new(self),
            description,
        }
    }
}

#[async_trait]
impl AgentReplyProvider for DashScopeSdkAgent {
    async fn reply(&self, transcript: &str) -> Result<AgentReply, AgentReplyError> {
        let sequence = self.next_message.fetch_add(1, Ordering::Relaxed);
        let input = ExecutionInput {
            conversation: ConversationSnapshot::new(vec![ConversationMessage::User(UserMessage {
                origin: Default::default(),
                transcript_visibility: Default::default(),
                id: MessageId::new(format!("voice_user_{sequence}"))
                    .map_err(|error| AgentReplyError::Build(error.to_string()))?,
                parts: vec![UserPart::Text(TextPart {
                    id: PartId::new(format!("voice_user_{sequence}_text"))
                        .map_err(|error| AgentReplyError::Build(error.to_string()))?,
                    text: transcript.to_owned(),
                })],
            })]),
        };
        let cancellation = CancellationToken::new();
        let execution =
            self.agent
                .start_ephemeral(input, cancellation.clone(), Arc::new(AllowAllAuthorizer));
        let outcome = await_completion(execution.completion, &cancellation, self.timeout).await?;
        let message = match outcome {
            ExecutionOutcome::Completed { message, .. } => message,
            ExecutionOutcome::Failed { error, .. } => {
                return Err(AgentReplyError::Execution(error.to_string()));
            }
            ExecutionOutcome::Cancelled { .. } => return Err(AgentReplyError::Cancelled),
            ExecutionOutcome::CompactionRequired { .. } => {
                return Err(AgentReplyError::CompactionRequired);
            }
            ExecutionOutcome::ContinuationRequired { .. } => {
                return Err(AgentReplyError::ContinuationRequired);
            }
        };
        let text = collect_user_facing_text(message.parts);
        if text.is_empty() {
            return Err(AgentReplyError::EmptyReply);
        }
        Ok(AgentReply { text })
    }
}

async fn await_completion(
    completion: impl Future<Output = ExecutionOutcome>,
    cancellation: &CancellationToken,
    duration: Duration,
) -> Result<ExecutionOutcome, AgentReplyError> {
    match timeout(duration, completion).await {
        Ok(outcome) => Ok(outcome),
        Err(_) => {
            cancellation.cancel();
            Err(AgentReplyError::Timeout)
        }
    }
}

fn collect_user_facing_text(parts: Vec<AssistantPart>) -> String {
    parts
        .into_iter()
        .filter_map(|part| match part {
            AssistantPart::Text(text) => Some(text.text),
            AssistantPart::Reasoning(_)
            | AssistantPart::ToolCall(_)
            | AssistantPart::ProviderState(_) => None,
        })
        .collect::<Vec<_>>()
        .join("\n")
        .trim()
        .to_owned()
}

fn optional_env(name: &str) -> Option<String> {
    env::var(name).ok().filter(|value| !value.trim().is_empty())
}

#[cfg(test)]
mod tests {
    use std::future::pending;

    use agent_types::{ReasoningPart, TextPart};

    use super::*;

    #[test]
    fn collects_only_user_facing_text_parts() {
        let text = collect_user_facing_text(vec![
            AssistantPart::Reasoning(ReasoningPart {
                id: PartId::new("reasoning").expect("reasoning id"),
                text: "private reasoning".to_owned(),
            }),
            AssistantPart::Text(TextPart {
                id: PartId::new("answer-1").expect("text id"),
                text: " 第一段 ".to_owned(),
            }),
            AssistantPart::Text(TextPart {
                id: PartId::new("answer-2").expect("text id"),
                text: "第二段".to_owned(),
            }),
        ]);

        assert_eq!(text, "第一段 \n第二段");
        assert!(!text.contains("private reasoning"));
    }

    #[tokio::test]
    async fn timeout_cancels_agent_execution() {
        let cancellation = CancellationToken::new();
        let result = await_completion(
            pending::<ExecutionOutcome>(),
            &cancellation,
            Duration::from_millis(1),
        )
        .await;

        assert!(matches!(result, Err(AgentReplyError::Timeout)));
        assert!(cancellation.is_cancelled());
    }
}
