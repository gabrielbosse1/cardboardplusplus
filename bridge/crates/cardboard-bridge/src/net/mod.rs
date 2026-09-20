pub mod camera;
pub mod driver;
pub mod mediapipe;
pub mod phone;
pub mod telemetry;
// UDP/TCP plane (mirrors CardboardWire.h): video, discovery/control,
// telemetry, camera, MediaPipe sidecar, and sensor forward.
pub const VIDEO_PORT: u16 = 42069;
pub const DRIVER_DISCOVERY_PORT: u16 = 42070;
pub const TELEMETRY_PORT: u16 = 42071;
pub const CAMERA_PORT: u16 = 42072;
pub const MEDIAPIPE_PORT: u16 = 42073;
pub const SENSOR_PORT: u16 = 42074;
/// Encoder chosen in the bridge Stream tab and pushed to the driver via
/// BRIDGE_CFG. Auto lets the driver pick; Gpu/Cpu are UI-shorthand aliases.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum EncoderChoice {
    Auto,
    Amf,
    Nvenc,
    Qsv,
    Libx264,
    Gpu,
    Cpu,
}
impl From<i32> for EncoderChoice {
    /// Maps the Stream-tab picker index to a variant. Out-of-range indexes
    /// fall back to Gpu so a stale UI selection never breaks the BRIDGE_CFG push.
    fn from(index: i32) -> Self {
        match index {
            1 => EncoderChoice::Cpu,
            2 => EncoderChoice::Nvenc,
            3 => EncoderChoice::Qsv,
            4 => EncoderChoice::Libx264,
            0 => EncoderChoice::Gpu,
            _ => {
                eprintln!("[bridge] unknown encoder index {index}, falling back to gpu");
                EncoderChoice::Gpu
            }
        }
    }
}
impl EncoderChoice {
    /// Parses a config-file/REST encoder name (case-insensitive, accepts both
    /// short and ffmpeg names). Unknown names fall back to Auto.
    pub fn from_name(name: &str) -> Self {
        match name.trim().to_ascii_lowercase().as_str() {
            "amf" | "h264_amf" => EncoderChoice::Amf,
            "nvenc" | "h264_nvenc" => EncoderChoice::Nvenc,
            "qsv" | "h264_qsv" => EncoderChoice::Qsv,
            "libx264" => EncoderChoice::Libx264,
            "gpu" => EncoderChoice::Gpu,
            "cpu" => EncoderChoice::Cpu,
            _ => {
                eprintln!("[bridge] unknown encoder name {name:?}, falling back to auto");
                EncoderChoice::Auto
            }
        }
    }
    /// Wire name sent to the driver in BRIDGE_CFG (ffmpeg encoder id or alias).
    pub fn as_str(&self) -> &'static str {
        match self {
            EncoderChoice::Auto => "auto",
            EncoderChoice::Amf => "h264_amf",
            EncoderChoice::Nvenc => "h264_nvenc",
            EncoderChoice::Qsv => "h264_qsv",
            EncoderChoice::Libx264 => "libx264",
            EncoderChoice::Gpu => "gpu",
            EncoderChoice::Cpu => "cpu",
        }
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn encoder_picker_index_maps_to_variants() {
        assert_eq!(EncoderChoice::from(0), EncoderChoice::Gpu);
        assert_eq!(EncoderChoice::from(1), EncoderChoice::Cpu);
        assert_eq!(EncoderChoice::from(99), EncoderChoice::Gpu);
    }
    #[test]
    fn encoder_names_are_lenient_and_case_insensitive() {
        assert_eq!(EncoderChoice::from_name("nvenc"), EncoderChoice::Nvenc);
        assert_eq!(EncoderChoice::from_name("h264_AMF"), EncoderChoice::Amf);
        assert_eq!(EncoderChoice::from_name(" h264_qsv "), EncoderChoice::Qsv);
        assert_eq!(EncoderChoice::from_name("libx264"), EncoderChoice::Libx264);
        assert_eq!(EncoderChoice::from_name("GPU"), EncoderChoice::Gpu);
        assert_eq!(EncoderChoice::from_name("cpu"), EncoderChoice::Cpu);
        assert_eq!(EncoderChoice::from_name("totally-unknown"), EncoderChoice::Auto);
    }
    #[test]
    fn encoder_as_str_produces_the_driver_wire_names() {
        assert_eq!(EncoderChoice::Nvenc.as_str(), "h264_nvenc");
        assert_eq!(EncoderChoice::Auto.as_str(), "auto");
        assert_eq!(EncoderChoice::Gpu.as_str(), "gpu");
        assert_eq!(EncoderChoice::Cpu.as_str(), "cpu");
    }
}
