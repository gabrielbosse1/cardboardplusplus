use std::fmt;
/// Errors from creating, opening, or mapping a shared-memory region. Surfaced to callers in `ring.rs` and `bridge-core/src/shm.rs`.
#[derive(Debug)]
pub enum MemError {
    Platform(String),
    InvalidState(&'static str),
}
impl fmt::Display for MemError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MemError::Platform(msg) => write!(f, "platform: {msg}"),
            MemError::InvalidState(msg) => write!(f, "invalid state: {msg}"),
        }
    }
}
impl std::error::Error for MemError {}
/// Shorthand result for shared-memory operations.
pub type MemResult<T> = Result<T, MemError>;
#[cfg(windows)]
/// Returns the driver→bridge region name in the session namespace. Called by `BridgeConsumer::open`.
pub fn region_name() -> String {
    format!("Local\\{}", crate::protocol::NAME_PREFIX)
}
#[cfg(windows)]
/// Returns the bridge→driver command region name. Called by `CmdProducer::open`.
pub fn cmd_region_name() -> String {
    format!("Local\\{}", crate::protocol::CMD_NAME_PREFIX)
}
#[cfg(not(windows))]
/// Returns the POSIX shm path for the driver→bridge region. Called by `BridgeConsumer::open`.
pub fn region_name() -> String {
    format!("/{}", crate::protocol::NAME_PREFIX)
}
#[cfg(not(windows))]
/// Returns the POSIX shm path for the bridge→driver command region. Called by `CmdProducer::open`.
pub fn cmd_region_name() -> String {
    format!("/{}", crate::protocol::CMD_NAME_PREFIX)
}
#[allow(dead_code)]
/// RAII handle for one mapped shared-memory region. `handle` owns the OS object, `base`/`size` describe the mapping used by `ring.rs`.
pub struct SharedMemory {
    handle: ShmHandle,
    base: *mut u8,
    size: usize,
}
#[cfg(windows)]
type ShmHandle = Option<windows::Win32::Foundation::HANDLE>;
#[cfg(windows)]
impl SharedMemory {
    /// Creates a pagefile-backed file mapping of `size` bytes and maps it. `_flags` is reserved for future open modes.
    pub fn create(name: &str, size: usize, _flags: u32) -> MemResult<Self> {
        use windows::core::PCWSTR;
        use windows::Win32::Foundation::HANDLE;
        use windows::Win32::System::Memory::*;
        let wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
        let h = unsafe {
            CreateFileMappingW(
                HANDLE::default(),
                None,
                PAGE_READWRITE,
                (size >> 32) as u32,
                (size & 0xFFFF_FFFF) as u32,
                PCWSTR(wide.as_ptr()),
            )
        }
        .map_err(|e| MemError::Platform(format!("CreateFileMappingW: {e}")))?;
        if h.is_invalid() {
            return Err(MemError::Platform("CreateFileMappingW returned invalid handle".into()));
        }
        Self::map(h, size)
    }
    /// Opens an existing Windows file mapping and maps a view over it. Called by `BridgeConsumer::open`.
    pub fn open(name: &str, size: usize) -> MemResult<Self> {
        use windows::core::PCWSTR;
        use windows::Win32::System::Memory::*;
        let wide: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
        let access = FILE_MAP_READ.0 | FILE_MAP_WRITE.0;
        let h = unsafe { OpenFileMappingW(access, false, PCWSTR(wide.as_ptr())) }
            .map_err(|e| MemError::Platform(format!("OpenFileMappingW: {e}")))?;
        if h.is_invalid() {
            return Err(MemError::Platform("OpenFileMappingW returned invalid handle".into()));
        }
        Self::map(h, size)
    }
    /// Maps the Windows handle into this process and stores the base pointer. Called by `create`/`open` above.
    fn map(h: windows::Win32::Foundation::HANDLE, size: usize) -> MemResult<Self> {
        use windows::Win32::System::Memory::*;
        let view = unsafe {
            MapViewOfFile(h, FILE_MAP(FILE_MAP_READ.0 | FILE_MAP_WRITE.0), 0, 0, size)
        };
        let base = view.Value as *mut u8;
        if base.is_null() {
            return Err(MemError::Platform("MapViewOfFile returned null".into()));
        }
        Ok(Self { handle: Some(h), base, size })
    }
}
#[cfg(not(windows))]
/// POSIX handle triple: fd plus ownership/name so `Drop` can unlink. Used only by the `SharedMemory` below.
struct ShmHandle {
    shm_fd: libc::c_int,
    owner: bool,
    name: String,
}
#[cfg(not(windows))]
impl SharedMemory {
    /// Creates and sizes a POSIX shm object, then maps it. `_flags` is reserved for future open modes.
    pub fn create(name: &str, size: usize, _flags: u32) -> MemResult<Self> {
        let cname = std::ffi::CString::new(name).map_err(|_| MemError::InvalidState("bad name"))?;
        let fd = unsafe {
            libc::shm_open(
                cname.as_ptr(),
                libc::O_CREAT | libc::O_RDWR,
                0o600,
            )
        };
        if fd < 0 {
            return Err(MemError::Platform(format!(
                "shm_open(error={})",
                std::io::Error::last_os_error()
            )));
        }
        if unsafe { libc::ftruncate(fd, size as libc::off_t) } != 0 {
            let e = MemError::Platform(format!("ftruncate: {}", std::io::Error::last_os_error()));
            unsafe { libc::close(fd) };
            return Err(e);
        }
        Self::map(ShmHandle { shm_fd: fd, owner: true, name: name.to_string() }, size)
    }
    /// Opens an existing POSIX shm object without resizing it. Called by `BridgeConsumer::open`.
    pub fn open(name: &str, size: usize) -> MemResult<Self> {
        let cname = std::ffi::CString::new(name).map_err(|_| MemError::InvalidState("bad name"))?;
        let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDWR, 0) };
        if fd < 0 {
            return Err(MemError::Platform(format!(
                "shm_open(error={})",
                std::io::Error::last_os_error()
            )));
        }
        Self::map(ShmHandle { shm_fd: fd, owner: false, name: name.to_string() }, size)
    }
    /// Mmaps the POSIX fd shared into this process. Called by `create`/`open` above.
    fn map(h: ShmHandle, size: usize) -> MemResult<Self> {
        let ptr = unsafe {
            libc::mmap(
                std::ptr::null_mut(),
                size,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                h.shm_fd,
                0,
            )
        };
        if ptr == libc::MAP_FAILED {
            let e = MemError::Platform(format!("mmap: {}", std::io::Error::last_os_error()));
            unsafe { libc::close(h.shm_fd) };
            return Err(e);
        }
        Ok(Self { handle: h, base: ptr as *mut u8, size })
    }
}
impl SharedMemory {
    /// Returns the raw base pointer of the mapping. Read by `ring.rs` to reach the region header and slots.
    pub fn base(&self) -> *mut u8 {
        self.base
    }
    /// Returns the mapped byte length. Used for bounds checks before draining slots.
    pub fn size(&self) -> usize {
        self.size
    }
}
impl Drop for SharedMemory {
    fn drop(&mut self) {
        unsafe {
            #[cfg(windows)]
            {
                use windows::Win32::System::Memory::{MEMORY_MAPPED_VIEW_ADDRESS, UnmapViewOfFile};
                if !self.base.is_null() {
                    let _ = UnmapViewOfFile(MEMORY_MAPPED_VIEW_ADDRESS {
                        Value: self.base as *mut _,
                    });
                }
            }
            #[cfg(not(windows))]
            {
                if !self.base.is_null() {
                    libc::munmap(self.base as *mut libc::c_void, self.size);
                    libc::close(self.handle.shm_fd);
                    if self.handle.owner {
                        if let Ok(s) = std::ffi::CString::new(self.handle.name.as_str()) {
                            libc::shm_unlink(s.as_ptr());
                        }
                    }
                }
            }
        }
    }
}
unsafe impl Send for SharedMemory {}
