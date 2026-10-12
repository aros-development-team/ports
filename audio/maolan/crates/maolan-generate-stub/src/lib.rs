//! The types Maolan exchanges with the `maolan-generate` process, without the Burn models
//! behind them. On AROS there is no GPU compute and no `socketpair`, so the generation
//! subprocess cannot be spawned; this crate keeps the GUI's generation dialogs compiling
//! and they report the failure when used.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;

pub const DEFAULT_MAX_PROMPT_TOKENS: usize = 128;
pub const DEFAULT_CFG_SCALE: f32 = 1.5;
pub const IPC_MODE_ENV: &str = "MAOLAN_BURN_SOCKETPAIR";

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum BackendChoice {
    Cpu,
    #[default]
    Vulkan,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub enum ModelChoice {
    #[serde(rename = "happy-new-year")]
    #[default]
    HappyNewYear,
    #[serde(rename = "RL")]
    Rl,
    #[serde(rename = "acestep-turbo")]
    AceStepTurbo,
    #[serde(rename = "acestep-sft")]
    AceStepSft,
    #[serde(rename = "text-to-midi")]
    TextToMidi,
    #[serde(rename = "midi-llm")]
    MidiLlm,
}

#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub enum AceStepLmSize {
    #[serde(rename = "0.6B")]
    #[default]
    B0_6,
    #[serde(rename = "1.7B")]
    B1_7,
    #[serde(rename = "4B")]
    B4,
}

fn default_output_path() -> PathBuf {
    PathBuf::from("output.wav")
}
fn default_ode_steps() -> usize {
    10
}
fn default_topk() -> usize {
    50
}
fn default_temperature() -> f32 {
    1.0
}
fn default_midi_length_seconds() -> f32 {
    30.0
}
fn default_midi_max_tokens() -> usize {
    1024
}
fn default_midi_top_p() -> f32 {
    0.95
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct GenerateRequest {
    #[serde(default)]
    pub model: ModelChoice,
    pub prompt: String,
    #[serde(default)]
    pub model_dir: Option<PathBuf>,
    #[serde(default = "default_output_path")]
    pub output_path: PathBuf,
    #[serde(default)]
    pub inspect_only: bool,
    pub backend: BackendChoice,
    pub cfg_scale: f32,
    #[serde(alias = "seconds_total", alias = "max_audio_length_ms")]
    pub length: usize,
    #[serde(default = "default_ode_steps")]
    pub ode_steps: usize,
    #[serde(default)]
    pub lyrics: Option<String>,
    #[serde(default)]
    pub tags: Option<String>,
    #[serde(default = "default_topk")]
    pub topk: usize,
    #[serde(default = "default_temperature")]
    pub temperature: f32,
    #[serde(default)]
    pub decode_only: bool,
    #[serde(default)]
    pub frames_json: Option<PathBuf>,
    #[serde(default)]
    pub decode_threads: Option<usize>,
    #[serde(default)]
    pub decoder_seed: u64,
    #[serde(default)]
    pub bpm: Option<f32>,
    #[serde(default)]
    pub key_scale: Option<String>,
    #[serde(default)]
    pub time_signature: Option<String>,
    #[serde(default)]
    pub acestep_lm: AceStepLmSize,
    #[serde(default = "default_midi_length_seconds")]
    pub midi_length_seconds: f32,
    #[serde(default)]
    pub midi_seed: u64,
    #[serde(default = "default_midi_max_tokens")]
    pub midi_max_tokens: usize,
    #[serde(default = "default_midi_top_p")]
    pub midi_top_p: f32,
    #[serde(default, skip)]
    pub output_path_explicit: bool,
}

pub type CliOptions = GenerateRequest;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct GenerateResponseHeader {
    pub backend: BackendChoice,
    pub channels: usize,
    pub frames: usize,
    pub guidance_scale: f32,
    pub prompt_tokens: i64,
    pub sample_rate_hz: u32,
    pub length: usize,
    pub steps: usize,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct GenerateError {
    pub error: String,
}

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct GenerateProgress {
    pub phase: String,
    pub progress: f32,
    pub operation: String,
}
