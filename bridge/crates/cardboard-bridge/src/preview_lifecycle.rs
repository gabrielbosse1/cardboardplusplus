//! Bridge preview lifecycle: when the viewer shows live video and when it must
//! go back to the idle screen. Pure logic (no I/O, no threads) so every
//! transition is unit-tested; the supervisor in core.rs feeds it real events
//! and executes the commands it returns.
//!
//! ```text
//!                      DecoderReady
//!   Stopping(Startup) ─────────────► Idle ◄──────────────────────────────┐
//!         ▲                           │ Traffic                          │
//!         │                           ▼                                  │ DecoderReady
//!         │                       Connecting ──Picture──► Streaming      │
//!         │                           │                       │          │
//!         │   Goodbye (STREAM_END) or │ no Traffic for         │         │
//!         │   STREAM_TIMEOUT          ▼ STREAM_TIMEOUT         ▼         │
//!         │                      Stopping(Goodbye | Timeout) ────────────┘
//!         │                           [Teardown + restart decoder]
//!         │
//!   any live state ── DecoderExited, or no Picture for DECODE_TIMEOUT ──►
//!   Error(fault) [Teardown, no restart] ── RETRY_BACKOFF ──► [Restart]
//!         ── DecoderReady ──► Idle      ── StartFailed ──► Error (wait again)
//! ```
//!
//! Every edge into Stopping or Error carries `Command::Teardown`: detach and
//! kill the decoder (flush), drop queued and published frames, clear the
//! viewer to the idle screen. No terminal path can leave a frozen frame.
use std::time::{Duration, Instant};

/// No driver datagram for this long ends the stream (driver or SteamVR died,
/// network gone). Covers an encoder re-init pause without flapping.
pub const STREAM_TIMEOUT: Duration = Duration::from_millis(2000);
/// Datagrams keep arriving but the decoder produced nothing for this long:
/// the decoder is wedged or the stream is undecodable. Keyframe requests make
/// a healthy stream recover in well under a second.
pub const DECODE_TIMEOUT: Duration = Duration::from_secs(5);
/// Wait before restarting a failed decoder, so a broken ffmpeg cannot spin.
pub const RETRY_BACKOFF: Duration = Duration::from_secs(2);

/// Why a stream ended normally.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StopReason {
    /// Bridge start: no decoder yet.
    Startup,
    /// The driver sent STREAM_END.
    Goodbye,
    /// Datagrams stopped for STREAM_TIMEOUT.
    Timeout,
}

/// Why the preview failed.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fault {
    /// ffmpeg exited while it was supposed to be running.
    DecoderExited,
    /// Datagrams arrived but no picture came out for DECODE_TIMEOUT.
    DecodeFailed,
    /// ffmpeg could not be started (missing from PATH, spawn error).
    DecoderUnavailable,
    /// UDP 42069 could not be bound.
    SocketUnavailable,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum State {
    /// Decoder ready, no stream. Viewer shows the idle screen.
    Idle,
    /// Driver datagrams arriving, no picture decoded yet (waiting for a
    /// keyframe). Viewer still shows the idle screen.
    Connecting,
    /// Pictures are being decoded and shown.
    Streaming,
    /// Tearing down / (re)starting the decoder. Viewer shows the idle screen.
    Stopping(StopReason),
    /// Decoder or socket failed; retried after RETRY_BACKOFF. Viewer shows
    /// the idle screen.
    Error(Fault),
}

impl State {
    /// Stable name for GET /status, GET /preview and the hub.
    pub fn name(&self) -> &'static str {
        match self {
            State::Idle => "idle",
            State::Connecting => "connecting",
            State::Streaming => "streaming",
            State::Stopping(_) => "stopping",
            State::Error(_) => "error",
        }
    }

    /// One-line description for the log.
    pub fn describe(&self) -> String {
        match self {
            State::Stopping(r) => format!("stopping ({r:?})"),
            State::Error(f) => format!("error ({f:?})"),
            s => s.name().to_string(),
        }
    }
}

/// Real events, as observed by the supervisor.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Input {
    /// Driver stream datagrams arrived since the last check.
    Traffic,
    /// The current decoder produced a picture since the last check.
    Picture,
    /// STREAM_END from the driver.
    Goodbye,
    /// The current decoder's output closed (ffmpeg exited or crashed).
    DecoderExited,
    /// A fresh decoder is running.
    DecoderReady,
    /// Starting the decoder (or binding the socket) failed.
    StartFailed(Fault),
    /// Timer tick; timeouts are evaluated here.
    Tick,
}

/// Work the supervisor must do for a transition.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Command {
    /// Detach and kill the decoder, drop every queued and published frame,
    /// clear the viewer to the idle screen. With `restart`, start a fresh
    /// decoder right away and report DecoderReady / StartFailed.
    Teardown { restart: bool },
    /// Start the decoder again (Error retry); report DecoderReady /
    /// StartFailed.
    Restart,
}

pub struct Lifecycle {
    state: State,
    entered: Instant,
    last_traffic: Option<Instant>,
    last_picture: Option<Instant>,
}

impl Lifecycle {
    /// Starts in Stopping(Startup): the supervisor starts the decoder and
    /// reports DecoderReady or StartFailed.
    pub fn new(now: Instant) -> Self {
        Self { state: State::Stopping(StopReason::Startup), entered: now, last_traffic: None, last_picture: None }
    }

    pub fn state(&self) -> State {
        self.state
    }

    /// Applies one input. Returns the command the transition requires.
    pub fn step(&mut self, input: Input, now: Instant) -> Option<Command> {
        use State::*;
        match (self.state, input) {
            // Teardown/restart outcomes.
            (Stopping(_) | Error(_), Input::DecoderReady) => {
                self.enter(Idle, now);
                None
            }
            (Stopping(_) | Error(_), Input::StartFailed(fault)) => {
                // Re-entering Error restarts the backoff timer.
                self.enter(Error(fault), now);
                None
            }
            (Error(_), Input::Tick) if now.duration_since(self.entered) >= RETRY_BACKOFF => {
                // Restart the backoff clock so a slow restart cannot be
                // re-issued every tick; the outcome arrives as an input.
                self.entered = now;
                Some(Command::Restart)
            }
            // Nothing else matters while tearing down or waiting to retry:
            // there is no decoder to feed and the viewer is already idle.
            (Stopping(_) | Error(_), _) => None,

            // The decoder died under us: always a fault, from any live state.
            (_, Input::DecoderExited) => self.fail(Fault::DecoderExited, now),

            (Idle, Input::Traffic) => {
                self.last_traffic = Some(now);
                self.last_picture = None;
                self.enter(Connecting, now);
                None
            }
            (Idle, Input::Picture) => {
                // Only possible when traffic and its first picture land in the
                // same check; treat it as both.
                self.last_traffic = Some(now);
                self.last_picture = Some(now);
                self.enter(Streaming, now);
                None
            }
            (Idle, _) => None,

            (Connecting | Streaming, Input::Traffic) => {
                self.last_traffic = Some(now);
                None
            }
            (Connecting, Input::Picture) => {
                self.last_picture = Some(now);
                self.enter(Streaming, now);
                None
            }
            (Streaming, Input::Picture) => {
                self.last_picture = Some(now);
                None
            }
            (Connecting | Streaming, Input::Goodbye) => self.stop(StopReason::Goodbye, now),
            (Connecting | Streaming, Input::Tick) => {
                let traffic = self.last_traffic.unwrap_or(self.entered);
                if now.duration_since(traffic) >= STREAM_TIMEOUT {
                    return self.stop(StopReason::Timeout, now);
                }
                // Since the last picture, or since Connecting began.
                let picture = self.last_picture.unwrap_or(self.entered);
                if now.duration_since(picture) >= DECODE_TIMEOUT {
                    return self.fail(Fault::DecodeFailed, now);
                }
                None
            }
            (Connecting | Streaming, Input::DecoderReady | Input::StartFailed(_)) => None,
        }
    }

    fn enter(&mut self, state: State, now: Instant) {
        self.state = state;
        self.entered = now;
    }

    fn stop(&mut self, reason: StopReason, now: Instant) -> Option<Command> {
        self.enter(State::Stopping(reason), now);
        self.last_traffic = None;
        self.last_picture = None;
        Some(Command::Teardown { restart: true })
    }

    fn fail(&mut self, fault: Fault, now: Instant) -> Option<Command> {
        self.enter(State::Error(fault), now);
        self.last_traffic = None;
        self.last_picture = None;
        Some(Command::Teardown { restart: false })
    }
}

#[cfg(test)]
mod tests {
    // Every transition, both timeouts at their exact boundary, and the two
    // acceptance paths: goodbye and kill mid-stream return to Idle and a new
    // stream starts right after. Test names read as the spec.
    use super::*;

    const MS: Duration = Duration::from_millis(1);

    struct Sim {
        lc: Lifecycle,
        now: Instant,
    }

    impl Sim {
        /// A lifecycle whose decoder started fine: Idle.
        fn ready() -> Sim {
            let now = Instant::now();
            let mut lc = Lifecycle::new(now);
            assert_eq!(lc.state(), State::Stopping(StopReason::Startup));
            assert_eq!(lc.step(Input::DecoderReady, now), None);
            assert_eq!(lc.state(), State::Idle);
            Sim { lc, now }
        }
        fn at(&mut self, ms: u64, input: Input) -> Option<Command> {
            self.now += ms * MS;
            self.lc.step(input, self.now)
        }
        fn state(&self) -> State {
            self.lc.state()
        }
        /// Idle -> Connecting -> Streaming.
        fn stream(&mut self) {
            assert_eq!(self.at(0, Input::Traffic), None);
            assert_eq!(self.state(), State::Connecting);
            assert_eq!(self.at(30, Input::Picture), None);
            assert_eq!(self.state(), State::Streaming);
        }
    }

    #[test]
    fn startup_failure_is_an_error_that_retries_after_backoff() {
        let now = Instant::now();
        let mut lc = Lifecycle::new(now);
        assert_eq!(lc.step(Input::StartFailed(Fault::DecoderUnavailable), now), None);
        assert_eq!(lc.state(), State::Error(Fault::DecoderUnavailable));
        assert_eq!(lc.step(Input::Tick, now + RETRY_BACKOFF - MS), None);
        assert_eq!(lc.step(Input::Tick, now + RETRY_BACKOFF), Some(Command::Restart));
        // Not re-issued while the restart is in flight.
        assert_eq!(lc.step(Input::Tick, now + RETRY_BACKOFF + 50 * MS), None);
        assert_eq!(lc.step(Input::DecoderReady, now + RETRY_BACKOFF + 60 * MS), None);
        assert_eq!(lc.state(), State::Idle);
    }

    #[test]
    fn traffic_then_first_picture_goes_idle_connecting_streaming() {
        let mut s = Sim::ready();
        s.stream();
        assert_eq!(s.at(16, Input::Traffic), None);
        assert_eq!(s.at(0, Input::Picture), None);
        assert_eq!(s.at(10, Input::Tick), None);
        assert_eq!(s.state(), State::Streaming);
    }

    #[test]
    fn goodbye_mid_stream_tears_down_and_returns_to_idle() {
        let mut s = Sim::ready();
        s.stream();
        assert_eq!(s.at(10, Input::Goodbye), Some(Command::Teardown { restart: true }));
        assert_eq!(s.state(), State::Stopping(StopReason::Goodbye));
        assert_eq!(s.at(80, Input::DecoderReady), None);
        assert_eq!(s.state(), State::Idle);
    }

    #[test]
    fn goodbye_while_connecting_also_tears_down() {
        let mut s = Sim::ready();
        s.at(0, Input::Traffic);
        assert_eq!(s.at(5, Input::Goodbye), Some(Command::Teardown { restart: true }));
        assert_eq!(s.state(), State::Stopping(StopReason::Goodbye));
    }

    #[test]
    fn killed_driver_times_out_exactly_at_stream_timeout() {
        let mut s = Sim::ready();
        s.stream();
        assert_eq!(s.at(16, Input::Traffic), None);
        let last = s.now;
        s.now = last + STREAM_TIMEOUT - MS;
        assert_eq!(s.at(0, Input::Tick), None);
        assert_eq!(s.state(), State::Streaming);
        s.now = last + STREAM_TIMEOUT;
        assert_eq!(s.at(0, Input::Tick), Some(Command::Teardown { restart: true }));
        assert_eq!(s.state(), State::Stopping(StopReason::Timeout));
    }

    #[test]
    fn a_new_stream_starts_immediately_after_a_kill() {
        let mut s = Sim::ready();
        for _ in 0..3 {
            s.stream();
            s.now += STREAM_TIMEOUT;
            assert_eq!(s.at(0, Input::Tick), Some(Command::Teardown { restart: true }));
            assert_eq!(s.at(50, Input::DecoderReady), None);
            assert_eq!(s.state(), State::Idle);
        }
        s.stream();
    }

    #[test]
    fn stalled_decoder_is_a_fault_without_immediate_restart() {
        let mut s = Sim::ready();
        s.stream();
        // Datagrams keep coming but pictures stop.
        let mut t = 0;
        while t + 100 < DECODE_TIMEOUT.as_millis() as u64 {
            assert_eq!(s.at(100, Input::Traffic), None);
            assert_eq!(s.at(0, Input::Tick), None, "faulted early at {t} ms");
            t += 100;
        }
        s.at(100, Input::Traffic);
        assert_eq!(s.at(0, Input::Tick), Some(Command::Teardown { restart: false }));
        assert_eq!(s.state(), State::Error(Fault::DecodeFailed));
        assert_eq!(s.at(RETRY_BACKOFF.as_millis() as u64, Input::Tick), Some(Command::Restart));
    }

    #[test]
    fn connecting_without_any_picture_fails_after_decode_timeout() {
        let mut s = Sim::ready();
        s.at(0, Input::Traffic);
        let mut elapsed = 0;
        while elapsed < DECODE_TIMEOUT.as_millis() as u64 - 100 {
            s.at(100, Input::Traffic);
            assert_eq!(s.at(0, Input::Tick), None);
            elapsed += 100;
        }
        s.at(100, Input::Traffic);
        assert_eq!(s.at(0, Input::Tick), Some(Command::Teardown { restart: false }));
        assert_eq!(s.state(), State::Error(Fault::DecodeFailed));
    }

    #[test]
    fn decoder_exit_from_any_live_state_is_a_fault_with_teardown() {
        for setup in 0..3 {
            let mut s = Sim::ready();
            if setup >= 1 {
                s.at(0, Input::Traffic);
            }
            if setup >= 2 {
                s.at(10, Input::Picture);
            }
            assert_eq!(s.at(10, Input::DecoderExited), Some(Command::Teardown { restart: false }));
            assert_eq!(s.state(), State::Error(Fault::DecoderExited));
        }
    }

    #[test]
    fn idle_ignores_goodbyes_and_ticks() {
        let mut s = Sim::ready();
        assert_eq!(s.at(0, Input::Goodbye), None);
        assert_eq!(s.at(10_000, Input::Tick), None);
        assert_eq!(s.state(), State::Idle);
    }

    #[test]
    fn stopping_and_error_ignore_stream_events() {
        let mut s = Sim::ready();
        s.stream();
        s.at(0, Input::Goodbye);
        for i in [Input::Traffic, Input::Picture, Input::Goodbye, Input::Tick, Input::DecoderExited] {
            assert_eq!(s.at(1, i), None);
            assert_eq!(s.state(), State::Stopping(StopReason::Goodbye));
        }
        s.at(1, Input::StartFailed(Fault::DecoderUnavailable));
        for i in [Input::Traffic, Input::Picture, Input::Goodbye, Input::DecoderExited] {
            assert_eq!(s.at(1, i), None);
            assert_eq!(s.state(), State::Error(Fault::DecoderUnavailable));
        }
    }

    #[test]
    fn every_terminal_transition_carries_a_teardown() {
        // Exhaustive over two-step input sequences from every reachable live
        // state: whenever the state becomes Stopping/Error from a live state,
        // the step returned Teardown (the viewer is cleared).
        let inputs = [
            Input::Traffic,
            Input::Picture,
            Input::Goodbye,
            Input::DecoderExited,
            Input::Tick,
            Input::DecoderReady,
            Input::StartFailed(Fault::DecoderUnavailable),
        ];
        for gap_ms in [0u64, 100, 2_000, 5_000] {
            for a in inputs {
                for b in inputs {
                    let mut s = Sim::ready();
                    for input in [a, b] {
                        let before = s.state();
                        let cmd = s.at(gap_ms, input);
                        let after = s.state();
                        let live = matches!(before, State::Idle | State::Connecting | State::Streaming);
                        let terminal = matches!(after, State::Stopping(_) | State::Error(_));
                        if live && terminal {
                            assert!(
                                matches!(cmd, Some(Command::Teardown { .. })),
                                "{before:?} --{input:?}--> {after:?} without teardown"
                            );
                        }
                    }
                }
            }
        }
    }

    #[test]
    fn state_names_are_stable() {
        assert_eq!(State::Idle.name(), "idle");
        assert_eq!(State::Connecting.name(), "connecting");
        assert_eq!(State::Streaming.name(), "streaming");
        assert_eq!(State::Stopping(StopReason::Timeout).name(), "stopping");
        assert_eq!(State::Error(Fault::DecodeFailed).name(), "error");
    }
}
