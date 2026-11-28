pub mod camera;
pub mod driver;
pub mod mediapipe;
pub mod phone;
pub mod telemetry;

/// Enlarges a UDP socket's receive buffer. The driver bursts a whole encoded
/// frame (~30+ datagrams, several hundred KB on IDRs) within a millisecond;
/// OS-default buffers are far smaller, so bursts overflowed and the kernel
/// silently dropped mid-frame datagrams — the decoder lost trailing slices and
/// drew concealment stripes in the bottom of the frame. std has no
/// set_recv_buffer_size, so this goes through the raw syscall on each OS.
/// Returns the size the OS reports after the attempt (doubled values are
/// normal), or None when unsupported.
pub fn set_recv_buffer_size(sock: &std::net::UdpSocket, bytes: i32) -> Option<i32> {
    #[cfg(unix)]
    {
        use std::os::fd::AsRawFd;
        #[cfg(target_os = "macos")]
        const SOL_SOCKET: i32 = 0xffff;
        #[cfg(target_os = "macos")]
        const SO_RCVBUF: i32 = 0x1002;
        #[cfg(not(target_os = "macos"))]
        const SOL_SOCKET: i32 = 1;
        #[cfg(not(target_os = "macos"))]
        const SO_RCVBUF: i32 = 8;
        extern "C" {
            fn setsockopt(fd: i32, level: i32, name: i32, value: *const std::ffi::c_void, len: u32) -> i32;
        }
        let rc = unsafe {
            setsockopt(sock.as_raw_fd(), SOL_SOCKET, SO_RCVBUF, &bytes as *const i32 as *const _, 4)
        };
        if rc != 0 {
            return None;
        }
        get_recv_buffer_size(sock)
    }
    #[cfg(windows)]
    {
        use std::os::windows::io::AsRawSocket;
        const SOL_SOCKET: i32 = 0xffff;
        const SO_RCVBUF: i32 = 0x1002;
        extern "system" {
            fn setsockopt(s: usize, level: i32, name: i32, value: *const u8, len: i32) -> i32;
        }
        let rc = unsafe {
            setsockopt(sock.as_raw_socket() as usize, SOL_SOCKET, SO_RCVBUF, &bytes as *const i32 as *const u8, 4)
        };
        if rc != 0 {
            return None;
        }
        get_recv_buffer_size(sock)
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = (sock, bytes);
        None
    }
}

/// Reads SO_RCVBUF back; used to verify the bump took effect.
pub fn get_recv_buffer_size(sock: &std::net::UdpSocket) -> Option<i32> {
    #[cfg(unix)]
    {
        use std::os::fd::AsRawFd;
        #[cfg(target_os = "macos")]
        const SOL_SOCKET: i32 = 0xffff;
        #[cfg(target_os = "macos")]
        const SO_RCVBUF: i32 = 0x1002;
        #[cfg(not(target_os = "macos"))]
        const SOL_SOCKET: i32 = 1;
        #[cfg(not(target_os = "macos"))]
        const SO_RCVBUF: i32 = 8;
        extern "C" {
            fn getsockopt(fd: i32, level: i32, name: i32, value: *mut std::ffi::c_void, len: *mut u32) -> i32;
        }
        let mut out: i32 = 0;
        let mut len: u32 = 4;
        let rc = unsafe {
            getsockopt(sock.as_raw_fd(), SOL_SOCKET, SO_RCVBUF, &mut out as *mut i32 as *mut _, &mut len)
        };
        if rc == 0 { Some(out) } else { None }
    }
    #[cfg(windows)]
    {
        use std::os::windows::io::AsRawSocket;
        const SOL_SOCKET: i32 = 0xffff;
        const SO_RCVBUF: i32 = 0x1002;
        extern "system" {
            fn getsockopt(s: usize, level: i32, name: i32, value: *mut u8, len: *mut i32) -> i32;
        }
        let mut out: i32 = 0;
        let mut len: i32 = 4;
        let rc = unsafe {
            getsockopt(sock.as_raw_socket() as usize, SOL_SOCKET, SO_RCVBUF, &mut out as *mut i32 as *mut u8, &mut len)
        };
        if rc == 0 { Some(out) } else { None }
    }
    #[cfg(not(any(unix, windows)))]
    {
        let _ = sock;
        None
    }
}

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

    // The bridge's defense against frame-tail loss: the bump must succeed and
    // be readable back. Linux clamps to net.core.rmem_max (~208 KB default,
    // reported doubled), Windows does not clamp — both must apply the request.
    #[test]
    fn recv_buffer_bump_applies_and_reads_back() {
        let s = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        let before = get_recv_buffer_size(&s);
        let after = set_recv_buffer_size(&s, 4 << 20);
        assert!(after.is_some(), "setsockopt(SO_RCVBUF) must succeed on this OS");
        let got = get_recv_buffer_size(&s).expect("SO_RCVBUF readable back");
        assert!(got > 0, "SO_RCVBUF must be positive");
        if let Some(b) = before {
            assert!(got >= b, "bump must not shrink the buffer (before={b}, after={got})");
        }
    }
}

#[cfg(test)]
mod encoder_choice_tests {
    use super::EncoderChoice;

    #[test]
    fn encoder_from_name_accepts_short_and_ffmpeg_names() {
        // E13: config files and the REST API may use either spelling; parsing
        // must be case-insensitive and whitespace-tolerant.
        assert_eq!(EncoderChoice::from_name("amf"), EncoderChoice::Amf);
        assert_eq!(EncoderChoice::from_name("H264_AMF"), EncoderChoice::Amf);
        assert_eq!(EncoderChoice::from_name(" nvenc "), EncoderChoice::Nvenc);
        assert_eq!(EncoderChoice::from_name("h264_nvenc"), EncoderChoice::Nvenc);
        assert_eq!(EncoderChoice::from_name("h264_qsv"), EncoderChoice::Qsv);
        assert_eq!(EncoderChoice::from_name("QSV"), EncoderChoice::Qsv);
        assert_eq!(EncoderChoice::from_name("libx264"), EncoderChoice::Libx264);
        assert_eq!(EncoderChoice::from_name("gpu"), EncoderChoice::Gpu);
        assert_eq!(EncoderChoice::from_name("CPU"), EncoderChoice::Cpu);
        assert_eq!(EncoderChoice::from_name("auto"), EncoderChoice::Auto);
    }
    #[test]
    fn encoder_from_name_unknown_or_empty_falls_back_to_auto() {
        // E17: a typo in the config must degrade to auto, never panic and
        // never leave the BRIDGE_CFG push unsent.
        assert_eq!(EncoderChoice::from_name("nonsense-codec"), EncoderChoice::Auto);
        assert_eq!(EncoderChoice::from_name(""), EncoderChoice::Auto);
        assert_eq!(EncoderChoice::from_name("  "), EncoderChoice::Auto);
    }
    #[test]
    fn encoder_picker_index_maps_ui_order_and_out_of_range_falls_back_to_gpu() {
        // E18: the Stream-tab picker index (stale UI selections included)
        // must never break the BRIDGE_CFG push: out-of-range -> gpu.
        assert_eq!(EncoderChoice::from(0), EncoderChoice::Gpu);
        assert_eq!(EncoderChoice::from(1), EncoderChoice::Cpu);
        assert_eq!(EncoderChoice::from(2), EncoderChoice::Nvenc);
        assert_eq!(EncoderChoice::from(3), EncoderChoice::Qsv);
        assert_eq!(EncoderChoice::from(4), EncoderChoice::Libx264);
        assert_eq!(EncoderChoice::from(9), EncoderChoice::Gpu);
        assert_eq!(EncoderChoice::from(-3), EncoderChoice::Gpu);
    }
    #[test]
    fn encoder_wire_names_roundtrip_through_from_name() {
        // BRIDGE_CFG wire names must stay ffmpeg ids/aliases the driver parses.
        for (choice, wire) in [
            (EncoderChoice::Auto, "auto"),
            (EncoderChoice::Amf, "h264_amf"),
            (EncoderChoice::Nvenc, "h264_nvenc"),
            (EncoderChoice::Qsv, "h264_qsv"),
            (EncoderChoice::Libx264, "libx264"),
            (EncoderChoice::Gpu, "gpu"),
            (EncoderChoice::Cpu, "cpu"),
        ] {
            assert_eq!(choice.as_str(), wire);
            assert_eq!(EncoderChoice::from_name(wire), choice);
        }
    }
}
