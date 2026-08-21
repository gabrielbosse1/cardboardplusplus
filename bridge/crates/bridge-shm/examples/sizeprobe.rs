fn main() {
    use bridge_shm::protocol::payload::*;
    macro_rules! p {
        ($t:ty, $name:expr) => {
            println!("{} {}a {}s", $name, std::mem::align_of::<$t>(), std::mem::size_of::<$t>());
        };
    }
    p!(TextureSetCreated, "TextureSetCreated");
    p!(FrameSubmitted, "FrameSubmitted");
    p!(CapReported, "CapReported");
    p!(Pose, "Pose");
    p!(ControllerInput, "ControllerInput");
    p!(Telemetry, "Telemetry");
}