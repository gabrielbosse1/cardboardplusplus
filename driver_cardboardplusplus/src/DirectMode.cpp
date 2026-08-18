#include "HmdDriver.h"
#include "DriverLog.h"
#include <dxgi.h>
#include <dxgi1_2.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using namespace vr;

// DIAG (Test A, FLICKER_ISSUE_MAP.md §8/§10): counts SubmitLayer calls between
// Presents and logs black-frame / format info to confirm multi-layer clobber (H1).
// Toggle: comment out the line below to compile without the diagnostics.
#define DRIVER_DIAG
#ifdef DRIVER_DIAG
static std::atomic<int> g_submitLayerCount{ 0 };
#endif

// ---------------------------------------------------------------------------
// IVRDriverDirectModeComponent compositing glue.
//
// This file owns the swap-texture-set bookkeeping and the Present/SubmitLayer
// path that turns the compositor's shared textures into private (left,right)
// copies queued for the background encoder thread. No actual encoding happens
// here; EncoderSetup.cpp / EncodingThread.cpp own the encoder and its loop.
// ---------------------------------------------------------------------------

void HmdDriver::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet)
{
	// Create three distinct shared textures (true triple buffering) so the app,
    // compositor, and encoder can each own a buffer simultaneously.
    DriverLog("CreateSwapTextureSet called: width=%d, height=%d, format=%d, samples=%d",
        pSwapTextureSetDesc->nWidth, pSwapTextureSetDesc->nHeight, pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nSampleCount);

    // Don't create shared textures while the encoder thread is hammering the
    // GPU (Intel drivers can stall shared allocations behind in-flight work).
    WaitEncoderIdle();
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = pSwapTextureSetDesc->nWidth;
        desc.Height = pSwapTextureSetDesc->nHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = (DXGI_FORMAT)pSwapTextureSetDesc->nFormat;  // Use SteamVR's exact format
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        std::shared_ptr<SwapTextureSet> set = std::make_shared<SwapTextureSet>();
        set->nextIndex = 0;

        for (int i = 0; i < 3; i++) {
            ID3D11Texture2D* pTexture = nullptr;
            HRESULT hr = m_pD3D11Device->CreateTexture2D(&desc, nullptr, &pTexture);

            if (FAILED(hr)) {
                DriverLog("Failed to create texture %d! HRESULT: 0x%x", i, hr);
                return;
            }

            IDXGIResource* pDXGIResource = nullptr;
            hr = pTexture->QueryInterface(__uuidof(IDXGIResource), (void**)&pDXGIResource);
            if (FAILED(hr)) {
                DriverLog("Failed to get DXGI resource for texture %d! HRESULT: 0x%x", i, hr);
                pTexture->Release();
                return;
            }

            HANDLE hSharedHandle = nullptr;
            hr = pDXGIResource->GetSharedHandle(&hSharedHandle);
            pDXGIResource->Release();

            if (FAILED(hr)) {
                DriverLog("Failed to get shared handle for texture %d! HRESULT: 0x%x", i, hr);
                pTexture->Release();
                return;
            }

            pOutSwapTextureSet->rSharedTextureHandles[i] = (vr::SharedTextureHandle_t)hSharedHandle;
            set->pTextures[i] = pTexture;
            set->hSharedHandles[i] = hSharedHandle;
            m_textureHandleMap[(vr::SharedTextureHandle_t)hSharedHandle] = pTexture;
            m_setByHandle[(vr::SharedTextureHandle_t)hSharedHandle] = set;
            DriverLog("Texture %d: handle=%llu", i, (uint64_t)hSharedHandle);
        }

        pOutSwapTextureSet->unTextureFlags = 0;
        m_swapTextureSets[unPid].push_back(set);
    }
    DriverLog("CreateSwapTextureSet complete for pid=%d", unPid);
}

void HmdDriver::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
	// Find the texture set with the given shared handle and release it.
    DriverLog("DestroySwapTextureSet called: handle=%llu", (uint64_t)sharedTextureHandle);

    // Make sure the encoder is not reading any of these textures right now.
    WaitEncoderIdle();

    auto it = m_setByHandle.find(sharedTextureHandle);
    if (it == m_setByHandle.end()) {
        return;
    }
    std::shared_ptr<SwapTextureSet> set = it->second;

    for (int i = 0; i < 3; i++) {
        m_textureHandleMap.erase((vr::SharedTextureHandle_t)set->hSharedHandles[i]);
        m_setByHandle.erase((vr::SharedTextureHandle_t)set->hSharedHandles[i]);
        if (set->pTextures[i]) {
            set->pTextures[i]->Release();
            set->pTextures[i] = nullptr;
        }
    }

    for (auto& pair : m_swapTextureSets) {
        for (auto sit = pair.second.begin(); sit != pair.second.end(); ++sit) {
            if (*sit == set) {
                pair.second.erase(sit);
                return;
            }
        }
    }
}

void HmdDriver::DestroyAllSwapTextureSets(uint32_t unPid)
{
	// Release all texture sets associated with the given process ID.
    DriverLog("DestroyAllSwapTextureSets called for pid=%d", unPid);

    // Pause Present() from queueing new frames that reference these textures.
    m_sceneTearingDown = true;

    // Wait for any in-flight encode to finish (uses m_d3dMutex + m_encoderMutex).
    WaitEncoderIdle();

    // Clear any pending frame so the encoding thread doesn't pick up stale pointers.
    {
        std::lock_guard<std::mutex> lock(m_encodeMutex);
        m_frameQueued = false;
        m_pendingFrame = { nullptr, nullptr, 0, false };
    }

    auto it = m_swapTextureSets.find(unPid);
    if (it != m_swapTextureSets.end()) {
        for (auto& set : it->second) {
            for (int i = 0; i < 3; i++) {
                m_textureHandleMap.erase((vr::SharedTextureHandle_t)set->hSharedHandles[i]);
                m_setByHandle.erase((vr::SharedTextureHandle_t)set->hSharedHandles[i]);
                if (set->pTextures[i]) {
                    set->pTextures[i]->Release();
                    set->pTextures[i] = nullptr;
                }
            }
        }
        m_swapTextureSets.erase(it);
    }

    m_sceneTearingDown = false;
}

void HmdDriver::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2])
{

	// Round-robin each eye's swap texture set so the app renders into a
    // different buffer every frame. This is what makes triple buffering real:
    // the buffer the driver is encoding from is never the one being written.
    for (int eye = 0; eye < 2; eye++) {
        auto it = m_setByHandle.find(sharedTextureHandles[eye]);
        if (it == m_setByHandle.end()) {
            (*pIndices)[eye] = 0;
            continue;
        }
        uint32_t idx = it->second->nextIndex;
        it->second->nextIndex = (idx + 1) % 3;
        (*pIndices)[eye] = idx;
    }
}

void HmdDriver::SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2])
{
	// This is where the application submits the textures it rendered for each eye.
    DriverLog("SubmitLayer called - left: %llu, right: %llu",
        (uint64_t)perEye[0].hTexture, (uint64_t)perEye[1].hTexture);

    // Accumulate every layer; Present() composites them all in submission order.
    // (H1 fix — FLICKER_ISSUE_MAP.md §10: the driver used to keep only the LAST
    // layer, so apps that submit scene + overlays flickered / showed black.)
    SubmitLayerInfo info;
    info.hTextureLeft = perEye[0].hTexture;
    info.hTextureRight = perEye[1].hTexture;
    info.boundsLeft = perEye[0].bounds;
    info.boundsRight = perEye[1].bounds;
    // Guard against runaway growth if Present is somehow delayed.
    if (m_submitLayers.size() < 16) {
        m_submitLayers.push_back(info);
        m_hasSubmit = true;
    }
#ifdef DRIVER_DIAG
    g_submitLayerCount++; // DIAG Test A
#endif
}

void HmdDriver::Present(vr::SharedTextureHandle_t syncTexture)
{
    // Count every Present SteamVR issues (compositor rate), independent of whether
    // we actually encode, so we can see if the stream is Present-bound or encode-bound.
    m_presentCount++;
    {
        long long nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
        if (m_lastPresentLogNs == 0) m_lastPresentLogNs = nowNs;
        if (nowNs - m_lastPresentLogNs >= 1'000'000'000LL) {
            double presentFps = m_presentCount * 1e9 / (nowNs - m_lastPresentLogNs);
            DriverLog("Present rate: %.1f fps (count=%d)", presentFps, m_presentCount);
            m_presentCount = 0;
            m_lastPresentLogNs = nowNs;
        }
    }

    if (!AcquireSyncTexture(syncTexture)) {
        static int syncFailCount = 0;
        if (++syncFailCount <= 5 || syncFailCount % 30 == 0)
            DriverLog("Present SKIPPED: AcquireSync failed (count=%d)", syncFailCount);
        return;
    }

    if (!m_hasSubmit || !m_encoderInitialized || !m_pVideoEncoder) {
        static int skipCount = 0;
        if (++skipCount <= 5 || skipCount % 30 == 0)
            DriverLog("Present SKIPPED: hasSubmit=%d encInit=%d enc=%p (count=%d)",
                      m_hasSubmit, m_encoderInitialized, m_pVideoEncoder, skipCount);
        ReleaseSyncTexture();
        return;
    }

    // If a scene transition is tearing down swap textures, don't queue.
    if (m_sceneTearingDown) {
        m_hasSubmit = false;
        ReleaseSyncTexture();
        return;
    }

    // Resolve every submitted layer's eye texture handles → D3D11Texture2D.
    // Skip layers whose handles aren't (yet) in the map; composite the rest.
    // If NONE are valid, drop the frame rather than streaming garbage.
    std::vector<SubmitLayerInfo> validLayers;
    std::vector<std::pair<ID3D11Texture2D*, ID3D11Texture2D*>> resolved;
    validLayers.reserve(m_submitLayers.size());
    resolved.reserve(m_submitLayers.size());
    for (const auto& layer : m_submitLayers) {
        ID3D11Texture2D* pL = nullptr;
        ID3D11Texture2D* pR = nullptr;
        auto itL = m_textureHandleMap.find(layer.hTextureLeft);
        if (itL != m_textureHandleMap.end()) pL = itL->second;
        auto itR = m_textureHandleMap.find(layer.hTextureRight);
        if (itR != m_textureHandleMap.end()) pR = itR->second;
        if (pL && pR) {
            validLayers.push_back(layer);
            resolved.push_back({ pL, pR });
        }
    }

    if (resolved.empty()) {
        static int mapFailCount = 0;
        if (++mapFailCount <= 5 || mapFailCount % 30 == 0)
            DriverLog("Map lookup FAILED: no valid layers of %zu submitted (map_size=%zu, count=%d)",
                m_submitLayers.size(), m_textureHandleMap.size(), mapFailCount);
        m_submitLayers.clear();
        m_hasSubmit = false;
        ReleaseSyncTexture();
        return;
    }

    // ===== DIAG (Test A): log layer count + submit rate every ~120 Presents =====
#ifdef DRIVER_DIAG
    {
        static int s_diagPresent = 0;
        if (++s_diagPresent % 120 == 0) {
            D3D11_TEXTURE2D_DESC dL = {}, dR = {};
            resolved.front().first->GetDesc(&dL);
            resolved.front().second->GetDesc(&dR);
            int submits = g_submitLayerCount.exchange(0); // layers over last ~120 frames
            DriverLog("[DIAG Present] #%d AcquireOK=%d layers=%zu submits/120f=%d firstLfmt=0x%x(%ux%u) firstRfmt=0x%x(%ux%u)",
                s_diagPresent, (int)(m_syncAcquired ? 1 : 0), m_submitLayers.size(), submits,
                (uint32_t)dL.Format, dL.Width, dL.Height,
                (uint32_t)dR.Format, dR.Width, dR.Height);
        }
    }
#endif

    // Only queue if encoder is idle — at most 1 frame in flight.
    {
        std::lock_guard<std::mutex> lock(m_encodeDoneMutex);
        if (!m_encodeDone) {
            static int dropCount = 0;
            if (++dropCount <= 5 || dropCount % 60 == 0)
                DriverLog("Present DROPPED: encoder busy (drop=%d)", dropCount);
            m_submitLayers.clear();
            m_hasSubmit = false;
            ReleaseSyncTexture();
            return;
        }
    }

    // GPU-copy each layer's compositor eye textures → its own shared private
    // copy. ComposeSBSGPU + ReadbackToBuffer happen on the encoding thread's own
    // D3D11 device, so the compositor thread is never blocked beyond these fast copies.
    {
        if (!EnsureLayerCopies(validLayers)) {
            DriverLog("Failed to create private eye copies on Present thread");
            m_submitLayers.clear();
            m_hasSubmit = false;
            ReleaseSyncTexture();
            return;
        }
        for (size_t i = 0; i < resolved.size() && i < m_layerCopies.size(); i++) {
            m_pD3D11DeviceContext->CopySubresourceRegion(m_layerCopies[i].pLeft, 0, 0, 0, 0,
                                                       resolved[i].first, 0, nullptr);
            m_pD3D11DeviceContext->CopySubresourceRegion(m_layerCopies[i].pRight, 0, 0, 0, 0,
                                                       resolved[i].second, 0, nullptr);
        }
        m_pD3D11DeviceContext->Flush();
    }

    // Queue for encoding. The encoding thread opens each shared copy on its own
    // D3D11 device and composites all layers (ComposeSBSGPU) + readback + encode.
    {
        std::lock_guard<std::mutex> lock(m_encodeMutex);
        m_pendingFrame = { nullptr, nullptr, m_encoderPts++, true };
        m_pendingFrame.layers.clear();
        for (size_t i = 0; i < resolved.size() && i < m_layerCopies.size(); i++) {
            if (!m_layerCopies[i].hLeft || !m_layerCopies[i].hRight) continue;
            PendingFrame::PendingLayer pl;
            pl.hLeft = m_layerCopies[i].hLeft;
            pl.hRight = m_layerCopies[i].hRight;
            pl.boundsLeft = validLayers[i].boundsLeft;
            pl.boundsRight = validLayers[i].boundsRight;
            m_pendingFrame.layers.push_back(pl);
        }
        m_frameQueued = true;
    }
    m_encodeCv.notify_one();

    {
        std::lock_guard<std::mutex> lock(m_encodeDoneMutex);
        m_encodeDone = false;
    }

    m_submitLayers.clear();
    m_hasSubmit = false;
    ReleaseSyncTexture();
}

void HmdDriver::PostPresent()
{
    // Pose updates are already handled by RunFrame().
}

bool HmdDriver::AcquireSyncTexture(vr::SharedTextureHandle_t syncTexture)
{
    if (!syncTexture) return false;
    HANDLE hSync = (HANDLE)syncTexture;
    if (hSync == INVALID_HANDLE_VALUE) return false;

    if (m_cachedSyncHandle != hSync || !m_pSyncMutex) {
        // Open the compositor sync texture once and cache it (Valve's
        // recommendation - opening it every frame breaks drivers).
        ReleaseSyncTexture();
        ID3D11Texture2D* pSyncTex = nullptr;
        HRESULT hr = m_pD3D11Device->OpenSharedResource(hSync, __uuidof(ID3D11Texture2D), (void**)&pSyncTex);
        if (FAILED(hr)) {
            DriverLog("OpenSharedResource(sync) failed! HRESULT: 0x%x", hr);
            return false;
        }
        IDXGIKeyedMutex* pMutex = nullptr;
        hr = pSyncTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&pMutex);
        if (FAILED(hr)) {
            DriverLog("Sync texture has no keyed mutex! HRESULT: 0x%x", hr);
            pSyncTex->Release();
            return false;
        }
        m_cachedSyncHandle = hSync;
        m_pSyncTexture = pSyncTex;
        m_pSyncMutex = pMutex;
    }

    HRESULT hr = m_pSyncMutex->AcquireSync(0, 10);
    if (hr != S_OK) {
        DriverLog("AcquireSync(0,10) failed! HRESULT: 0x%x (skipping frame)", hr);
        return false;
    }
    m_syncAcquired = true;
    return true;
}

void HmdDriver::ReleaseSyncTexture()
{
    if (m_pSyncMutex && m_syncAcquired) {
        m_pSyncMutex->ReleaseSync(0);
        m_syncAcquired = false;
    }
}

bool HmdDriver::EnsureLayerCopies(const std::vector<SubmitLayerInfo>& layers)
{
    // Ensure m_layerCopies has one (left,right) shared pair per layer, each sized
    // to that layer's eye resolution/format. Extra entries from a previous (larger)
    // frame are kept and reused; missing or size/format-changed entries are recreated.
    if (m_layerCopies.size() < layers.size()) {
        m_layerCopies.resize(layers.size());
    }

    for (size_t i = 0; i < layers.size(); i++) {
        auto itL = m_textureHandleMap.find(layers[i].hTextureLeft);
        auto itR = m_textureHandleMap.find(layers[i].hTextureRight);
        if (itL == m_textureHandleMap.end() || itR == m_textureHandleMap.end()) {
            continue; // already validated by the caller; skip defensively
        }
        D3D11_TEXTURE2D_DESC dL = {}, dR = {};
        itL->second->GetDesc(&dL);
        itR->second->GetDesc(&dR);

        LayerCopy& c = m_layerCopies[i];
        bool needRecreate = (!c.pLeft || !c.pRight ||
                             c.width != (int)dL.Width || c.height != (int)dL.Height ||
                             c.format != dL.Format);
        if (!needRecreate) continue;

        // Release the old copy + its shared handle before recreating.
        if (c.pLeft) { c.pLeft->Release(); c.pLeft = nullptr; }
        if (c.pRight) { c.pRight->Release(); c.pRight = nullptr; }
        if (c.hLeft) { CloseHandle(c.hLeft); c.hLeft = nullptr; }
        if (c.hRight) { CloseHandle(c.hRight); c.hRight = nullptr; }

        auto makeCopy = [&](ID3D11Texture2D* srcTex, D3D11_TEXTURE2D_DESC srcDesc,
                            ID3D11Texture2D** ppOut, HANDLE* phOut) -> bool {
            D3D11_TEXTURE2D_DESC desc = srcDesc;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
            HRESULT hr = m_pD3D11Device->CreateTexture2D(&desc, nullptr, ppOut);
            if (FAILED(hr)) {
                DriverLog("Failed to create private eye copy! HRESULT: 0x%x", hr);
                return false;
            }
            IDXGIResource* pRes = nullptr;
            HANDLE h = nullptr;
            if (SUCCEEDED((*ppOut)->QueryInterface(__uuidof(IDXGIResource), (void**)&pRes))) {
                pRes->GetSharedHandle(&h);
                pRes->Release();
            }
            if (!h) {
                DriverLog("Failed to get shared handle for private eye copy");
                (*ppOut)->Release(); *ppOut = nullptr;
                return false;
            }
            *phOut = h;
            return true;
        };

        if (!makeCopy(itL->second, dL, &c.pLeft, &c.hLeft)) return false;
        if (!makeCopy(itR->second, dR, &c.pRight, &c.hRight)) {
            if (c.pLeft) { c.pLeft->Release(); c.pLeft = nullptr; }
            if (c.hLeft) { CloseHandle(c.hLeft); c.hLeft = nullptr; }
            return false;
        }
        c.width = (int)dL.Width;
        c.height = (int)dL.Height;
        c.format = dL.Format;
    }

    return true;
}

void HmdDriver::WaitEncoderIdle()
{
    // Bounded wait: if the encoder thread ever gets stuck (e.g. GPU hang), we
    // must not freeze SteamVR's teardown paths (Destroy*/ApplyHardwareCap) and
    // trigger the vrserver watchdog abort.
    std::unique_lock<std::mutex> lock(m_encodeDoneMutex);
    m_encodeDoneCv.wait_for(lock, std::chrono::seconds(2), [this] { return m_encodeDone; });
}

void HmdDriver::GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming)
{
	// This is called to get additional frame timing stats from driver. Can be used to get the current framerate to optimize the encoder settings in real-time.
    DriverLog("GetFrameTiming called");
}