//! D3D11 texture acquisition.
//!
//! The driver's Present path forwards raw `SharedTextureHandle_t` values (the
//! `GetSharedHandle` HANDLE of a texture created with
//! `D3D11_RESOURCE_MISC_SHARED`). The bridge owns its own `ID3D11Device`, opens
//! each handle with `OpenSharedResource`, copies the eye texture into a CPU
//! **staging** texture, maps it, and returns duplicated rows for the FFmpeg
//! encoder.
//!
//! # Thread / sync model
//!
//! Textures are written by SteamVR's compositor on the GPU. Before readback the
//! bridge issues `Flush()` on its device context so the copy is ordered after
//! the write. Shared textures without a keyed mutex can theoretically race
//! (torn frames); that is accepted today and tracked in
//! `docs/MIGRATION_STATUS.md` under "sync (keyed mutex / NTHANDLE)".

use windows::core::Interface;
use windows::Win32::Foundation::HANDLE;
use windows::Win32::Graphics::Direct3D::{D3D_FEATURE_LEVEL, D3D_FEATURE_LEVEL_11_1};
use windows::Win32::Graphics::Direct3D11::*;
use windows::Win32::Graphics::Dxgi::Common::{
    DXGI_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
    DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM,
};

#[derive(Debug, Clone)]
pub struct D3D11Error(pub String);

impl std::fmt::Display for D3D11Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}
impl std::error::Error for D3D11Error {}

fn win_err(ctx: &str, e: windows::core::Error) -> D3D11Error {
    D3D11Error(format!("{ctx}: {e}"))
}

/// A CPU-readable copy of one eye texture.
#[derive(Debug)]
pub struct CpuTexture {
    pub width: u32,
    pub height: u32,
    pub format: DXGI_FORMAT,
    pub row_pitch: usize,
    /// Dense rows (`width * bpp` per row).
    pub data: Vec<u8>,
}

/// Owns the D3D11 device used to open shared textures.
pub struct D3D11Acquirer {
    device: ID3D11Device,
    context: ID3D11DeviceContext,
}

impl D3D11Acquirer {
    pub fn new() -> Result<Self, D3D11Error> {
        let mut device: Option<ID3D11Device> = None;
        let mut context: Option<ID3D11DeviceContext> = None;
        let mut feature_level = D3D_FEATURE_LEVEL(0);

        unsafe {
            D3D11CreateDevice(
                None,
                windows::Win32::Graphics::Direct3D::D3D_DRIVER_TYPE_HARDWARE,
                DEFAULT_HMODULE,
                D3D11_CREATE_DEVICE_FLAG(0),
                Some(&[D3D_FEATURE_LEVEL_11_1]),
                7, // D3D11_SDK_VERSION
                Some(&mut device),
                Some(&mut feature_level),
                Some(&mut context),
            )
        }
        .map_err(|e| win_err("D3D11CreateDevice failed", e))?;

        let device = device.ok_or_else(|| D3D11Error("device is None".into()))?;
        let context = context.ok_or_else(|| D3D11Error("context is None".into()))?;
        log::info!("D3D11Acquirer: device created, feature level 0x{:X}", feature_level.0);
        Ok(Self { device, context })
    }

    /// Open a shared texture by its `GetSharedHandle` HANDLE value and copy it
    /// to CPU memory.
    pub fn readback(&self, shared_handle: u64) -> Result<CpuTexture, D3D11Error> {
        let handle = HANDLE(shared_handle as *mut _);
        let mut texture: Option<ID3D11Texture2D> = None;
        unsafe {
            self.device
                .OpenSharedResource::<ID3D11Texture2D>(handle, &mut texture)
        }
        .map_err(|e| win_err("OpenSharedResource failed", e))?;
        let texture = texture.ok_or_else(|| D3D11Error("texture is None".into()))?;

        let mut desc = D3D11_TEXTURE2D_DESC::default();
        unsafe { texture.GetDesc(&mut desc) };

        // Create a CPU staging texture matching the source.
        let staging_desc = D3D11_TEXTURE2D_DESC {
            Width: desc.Width,
            Height: desc.Height,
            MipLevels: 1,
            ArraySize: 1,
            Format: desc.Format,
            SampleDesc: desc.SampleDesc,
            Usage: D3D11_USAGE_STAGING,
            BindFlags: 0,
            CPUAccessFlags: D3D11_CPU_ACCESS_READ.0 as u32,
            MiscFlags: 0,
        };
        let mut staging: Option<ID3D11Texture2D> = None;
        unsafe {
            self.device
                .CreateTexture2D(&staging_desc, None, Some(&mut staging))
        }
        .map_err(|e| win_err("CreateTexture2D(staging) failed", e))?;
        let staging = staging.ok_or_else(|| D3D11Error("staging is None".into()))?;

        let staging_res: ID3D11Resource =
            staging.cast().map_err(|e| win_err("staging cast failed", e))?;
        let tex_res: ID3D11Resource =
            texture.cast().map_err(|e| win_err("texture cast failed", e))?;

        unsafe {
            self.context.Flush();
            self.context.CopyResource(&staging_res, &tex_res);
        }

        let mut mapped = D3D11_MAPPED_SUBRESOURCE::default();
        unsafe {
            self.context.Map(
                &staging_res,
                0,
                D3D11_MAP_READ,
                0,
                Some(&mut mapped),
            )
        }
        .map_err(|e| win_err("Map failed", e))?;

        let row_pitch = mapped.RowPitch as usize;
        let height = desc.Height as usize;
        let bytes_per_pixel = bytes_per_pixel(desc.Format).unwrap_or(4);
        let dense_row = (desc.Width as usize) * bytes_per_pixel;
        let mut data = vec![0u8; dense_row * height];

        unsafe {
            let src = mapped.pData as *const u8;
            for y in 0..height {
                let src_row = src.add(y * row_pitch);
                std::ptr::copy_nonoverlapping(
                    src_row,
                    data.as_mut_ptr().add(y * dense_row),
                    dense_row,
                );
            }
            self.context.Unmap(&staging_res, 0);
        }

        Ok(CpuTexture {
            width: desc.Width,
            height: desc.Height,
            format: desc.Format,
            row_pitch,
            data,
        })
    }
}

/// Null HMODULE (software adapter none). `D3D11CreateDevice` takes it by value.
const DEFAULT_HMODULE: windows::Win32::Foundation::HMODULE =
    windows::Win32::Foundation::HMODULE(std::ptr::null_mut());

fn bytes_per_pixel(fmt: DXGI_FORMAT) -> Option<usize> {
    Some(match fmt {
        DXGI_FORMAT_B8G8R8A8_UNORM => 4,
        DXGI_FORMAT_R8G8B8A8_UNORM => 4,
        DXGI_FORMAT_R10G10B10A2_UNORM => 4,
        DXGI_FORMAT_R16G16B16A16_FLOAT => 8,
        _ => return None,
    })
}