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

/// Which encoder the driver should use, in the same order the UI exposes them
/// (index 0 = auto, then AMF, NVENC, QSV, libx264).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum EncoderChoice {
    Auto,
    Amf,
    Nvenc,
    Qsv,
    Libx264,
}

impl From<i32> for EncoderChoice {
    /// Maps the UI's encoder picker index (0..=4) to an encoder variant.
    fn from(index: i32) -> Self {
        match index {
            1 => EncoderChoice::Amf,
            2 => EncoderChoice::Nvenc,
            3 => EncoderChoice::Qsv,
            4 => EncoderChoice::Libx264,
            _ => EncoderChoice::Auto,
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
            _ => EncoderChoice::Auto,
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
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn encoder_picker_index_maps_to_variants() {
        assert_eq!(EncoderChoice::from(0), EncoderChoice::Auto);
        assert_eq!(EncoderChoice::from(1), EncoderChoice::Amf);
        assert_eq!(EncoderChoice::from(2), EncoderChoice::Nvenc);
        assert_eq!(EncoderChoice::from(3), EncoderChoice::Qsv);
        assert_eq!(EncoderChoice::from(4), EncoderChoice::Libx264);
        assert_eq!(EncoderChoice::from(99), EncoderChoice::Auto);
    }

    #[test]
    fn encoder_names_are_lenient_and_case_insensitive() {
        assert_eq!(EncoderChoice::from_name("nvenc"), EncoderChoice::Nvenc);
        assert_eq!(EncoderChoice::from_name("h264_AMF"), EncoderChoice::Amf);
        assert_eq!(EncoderChoice::from_name(" h264_qsv "), EncoderChoice::Qsv);
        assert_eq!(EncoderChoice::from_name("libx264"), EncoderChoice::Libx264);
        assert_eq!(EncoderChoice::from_name("totally-unknown"), EncoderChoice::Auto);
    }

    #[test]
    fn encoder_as_str_produces_the_driver_wire_names() {
        assert_eq!(EncoderChoice::Nvenc.as_str(), "h264_nvenc");
        assert_eq!(EncoderChoice::Auto.as_str(), "auto");
    }
}