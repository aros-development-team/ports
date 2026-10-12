//! The `maolan-pitch` API without the Burn/wgpu models behind it. AROS has no GPU compute
//! and no network model download; the neural pitch detector and the vocoder report that
//! they are unavailable, and Maolan falls back to its classic pitch detector.

use anyhow::{Result, bail};
use std::path::{Path, PathBuf};

pub const HF_REPO_OWNER: &str = "maolandaw";
pub const HF_REPO_NAME: &str = "FCPE-burn";
pub const HF_REPO_FILE: &str = "fcpe.bpk";
pub const VOCODER_HF_REPO_NAME: &str = "PC-NSF-HiFiGAN-burn";
pub const VOCODER_HF_REPO_FILE: &str = "pc_nsf_hifigan.bpk";
pub const VOCODER_BPK_ENV: &str = "PC_NSF_HIFIGAN_BPK";

const UNAVAILABLE: &str = "neural pitch processing (Burn/wgpu) is not available on AROS";

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PitchDetectorKind {
    Classic,
    Neural,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct F0Frame {
    pub time_samples: usize,
    pub frequency: Option<f32>,
    pub confidence: f32,
}

/// Never constructed: the loaders fail before one exists.
#[derive(Debug)]
pub struct FcpeWeights {
    _private: (),
}

#[derive(Debug)]
pub struct FcpeDetector {
    _weights: FcpeWeights,
    threshold: f32,
}

impl FcpeDetector {
    pub fn new(weights: FcpeWeights) -> Self {
        Self { _weights: weights, threshold: 0.05 }
    }

    pub fn with_threshold(mut self, threshold: f32) -> Self {
        self.threshold = threshold;
        self
    }

    pub fn kind(&self) -> PitchDetectorKind {
        PitchDetectorKind::Neural
    }

    pub fn detect_f0(&self, _audio: &[f32], _sample_rate: u32, mut progress: impl FnMut(f32)) -> Vec<F0Frame> {
        progress(1.0);
        Vec::new()
    }
}

pub fn load_fcpe_weights(_path: &Path) -> Result<FcpeWeights> {
    bail!("{UNAVAILABLE}")
}

pub fn ensure_fcpe_model(_cache_dir: &Path) -> Result<PathBuf> {
    bail!("{UNAVAILABLE}")
}

#[derive(Debug)]
pub struct PcNsfHifiGanWeights {
    _private: (),
}

#[derive(Debug)]
pub struct PcNsfHifiGan {
    _weights: PcNsfHifiGanWeights,
}

impl PcNsfHifiGan {
    pub fn new(weights: PcNsfHifiGanWeights) -> Self {
        Self { _weights: weights }
    }

    pub fn synthesize(&self, _mel: &[Vec<f32>], _f0_hz: &[f32]) -> Vec<f32> {
        Vec::new()
    }
}

pub fn load_pc_nsf_hifigan_weights(_path: &Path) -> Result<PcNsfHifiGanWeights> {
    bail!("{UNAVAILABLE}")
}

pub fn ensure_pc_nsf_hifigan_model(_cache_dir: &Path) -> Result<PathBuf> {
    if let Ok(env_path) = std::env::var(VOCODER_BPK_ENV) {
        let env_path = PathBuf::from(env_path);
        if env_path.exists() {
            return Ok(env_path);
        }
    }
    bail!("{UNAVAILABLE}")
}

pub mod resample {
    use anyhow::Result;

    /// Linear resampling; good enough for the paths that reach it on AROS.
    pub fn resample_to(audio: &[f32], from_rate: u32, target_rate: u32) -> Result<Vec<f32>> {
        if from_rate == target_rate || audio.is_empty() || from_rate == 0 || target_rate == 0 {
            return Ok(audio.to_vec());
        }
        let ratio = from_rate as f64 / target_rate as f64;
        let out_len = ((audio.len() as f64) / ratio).floor() as usize;
        let mut out = Vec::with_capacity(out_len);
        for i in 0..out_len {
            let pos = i as f64 * ratio;
            let idx = pos.floor() as usize;
            let frac = (pos - idx as f64) as f32;
            let a = audio[idx.min(audio.len() - 1)];
            let b = audio[(idx + 1).min(audio.len() - 1)];
            out.push(a + (b - a) * frac);
        }
        Ok(out)
    }
}

pub mod vmel {
    pub const SAMPLE_RATE: u32 = 44100;
    pub const N_FFT: usize = 2048;
    pub const WIN_SIZE: usize = 2048;
    pub const HOP_SIZE: usize = 512;
    pub const N_MELS: usize = 128;
    pub const FMIN: f32 = 40.0;
    pub const FMAX: f32 = 16000.0;
    pub const CLIP_VAL: f32 = 1e-5;

    /// No mel spectrogram without the vocoder to consume it: an empty result makes the
    /// caller report that resynthesis is unavailable.
    pub fn wav_to_mel(_audio: &[f32]) -> Vec<Vec<f32>> {
        Vec::new()
    }
}
