//! Cross-platform shared-memory mapping.
//!
//! - **Windows**: named file mapping `Local\cardboard_pp_bridge`
//!   (`CreateFileMappingW` + `MapViewOfFile`). The producer creates it, the
//!   consumer opens it by name.
//! - **Linux**: POSIX shared memory `/cardboard_pp_bridge`
//!   (`shm_open` + `ftruncate` + `mmap`).
//!
//! The layout is the same on both platforms; only the mechanism differs.

use std::fmt;

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

pub type MemResult<T> = Result<T, MemError>;

/// The full platform name used to address the region.
#[cfg(windows)]
pub fn region_name() -> String {
    format!("Local\\{}", crate::protocol::NAME_PREFIX)
}

/// The full platform name used to address the command (settings) region.
#[cfg(windows)]
pub fn cmd_region_name() -> String {
    format!("Local\\{}", crate::protocol::CMD_NAME_PREFIX)
}

/// The full platform name used to address the region.
#[cfg(not(windows))]
pub fn region_name() -> String {
    format!("/{}", crate::protocol::NAME_PREFIX)
}

/// The full platform name used to address the command (settings) region.
#[cfg(not(windows))]
pub fn cmd_region_name() -> String {
    format!("/{}", crate::protocol::CMD_NAME_PREFIX)
}

/// A mapped shared-memory region.
#[allow(dead_code)]
pub struct SharedMemory {
    handle: ShmHandle,
    base: *mut u8,
    size: usize,
}

#[cfg(windows)]
type ShmHandle = Option<windows::Win32::Foundation::HANDLE>;

#[cfg(windows)]
impl SharedMemory {
    /// Create (producer) the named region. Fails if a region with the same
    /// name already exists, which would mean two drivers are running.
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

    /// Open (consumer) an existing named region.
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
struct ShmHandle {
    shm_fd: libc::c_int,
    /// 1 if we created the region (and should unlink on drop).
    owner: bool,
}

#[cfg(not(windows))]
impl SharedMemory {
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
        Self::map(ShmHandle { shm_fd: fd, owner: true }, size)
    }

    pub fn open(name: &str, size: usize) -> MemResult<Self> {
        let cname = std::ffi::CString::new(name).map_err(|_| MemError::InvalidState("bad name"))?;
        let fd = unsafe { libc::shm_open(cname.as_ptr(), libc::O_RDWR, 0) };
        if fd < 0 {
            return Err(MemError::Platform(format!(
                "shm_open(error={})",
                std::io::Error::last_os_error()
            )));
        }
        Self::map(ShmHandle { shm_fd: fd, owner: false }, size)
    }

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
    pub fn base(&self) -> *mut u8 {
        self.base
    }
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
                        // Best-effort unlink; ignore when another consumer still has it open.
                        let name =
                            region_name();
                        if let Ok(s) = std::ffi::CString::new(name) {
                            libc::shm_unlink(s.as_ptr());
                        }
                    }
                }
            }
        }
    }
}

// Raw pointers are not Send; the region is used from a single consumer thread.
unsafe impl Send for SharedMemory {}