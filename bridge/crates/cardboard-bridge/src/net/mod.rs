//! Network-facing workers: the driver control link, the phone telemetry link
//! and the binary wire formats they speak. Ports here are the locked bridge
//! contract — the driver discovery socket (42070), the telemetry uplink
//! (42071) and the untouched-by-design video stream (42069).

pub mod camera;
pub mod driver;
pub mod mediapipe;
pub mod phone;
pub mod telemetry;

/// Encoded video goes PC -> phone directly; the bridge never touches it.
pub const VIDEO_PORT: u16 = 42069;
/// Discovery/heartbeat link to the SteamVR driver (BRIDGE_HELLO <-> BRIDGE_ACK).
pub const DRIVER_DISCOVERY_PORT: u16 = 42070;
/// Telemetry uplink from the phone (gyro / hand / ping / hello frames).
pub const TELEMETRY_PORT: u16 = 42071;
/// JPEG camera frames from the phone for MediaPipe hand detection.
pub const CAMERA_PORT: u16 = 42072;
/// TCP port for the Python MediaPipe hand-landmark server.
pub const MEDIAPIPE_PORT: u16 = 42073;
/// UDP port for bridge → driver sensor data forwarding (binary, same format as phone→bridge).
pub const SENSOR_PORT: u16 = 42074;

/// Which encoder the driver should use. The UI only exposes two: GPU (the
/// driver probes AMF → NVENC → QSV with a libx264 fallback) and CPU (forced
/// libx264). Named backends stay for the REST API and old configs.
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
    /// Maps the UI's encoder picker index (0 = GPU, 1 = CPU) to a variant.
    /// Unknown indices fall back to GPU (and log it) so a stale config
    /// never bricks the session.
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
    /// Parse the text name accepted by the REST API / config file. Input is
    /// case-insensitive and tolerates the `h264_` prefix; anything unknown
    /// falls back to auto so a typo never bricks the session.
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

    /// The exact string sent to the driver inside BRIDGE_CFG.
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