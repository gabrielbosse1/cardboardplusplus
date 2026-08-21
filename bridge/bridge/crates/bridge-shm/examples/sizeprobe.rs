fn main() {
    use bridge_shm::protocol::payload::*;
    let t = |a:Type| {};
    println!("TextureSetCreated {} {}", std::mem::size_of::<TextureSetCreated>(), std::mem::align_of::<TextureSetCreated>());
    println!("FrameSubmitted {} {}", std::mem::size_of::<FrameSubmitted>(), std::mem::align_of::<FrameSubmitted>());
    println!("CapReported {} {}", std::mem::size_of::<CapReported>(), std::mem::align_of::<CapReported>());
    println!("Pose {} {}", std::mem::size_of::<Pose>(), std::mem::align_of::<Pose>());
    println!("ControllerInput {} {}", std::mem::size_of::<ControllerInput>(), std::mem::align_of::<ControllerInput>());
    println!("Telemetry {} {}", std::mem::size_of::<Telemetry>(), std::mem::align_of::<Telemetry>());
}
