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
#ifdef _DEBUG
#define DRIVER_DIAG
#endif
#ifdef DRIVER_DIAG
static std::atomic<int> g_submitLayerCount{ 0 };
#endif
// SteamVR per-process diagnostic counter, active only in debug/diag builds.
// Called by the SteamVR compositor to allocate 3 shared D3D11 textures; records handles in m_textureHandleMap/m_setByHandle.
void HmdDriver::CreateSwapTextureSet(uint32_t unPid, const SwapTextureSetDesc_t* pSwapTextureSetDesc, SwapTextureSet_t* pOutSwapTextureSet)
{
    if (pOutSwapTextureSet) {
        std::memset(pOutSwapTextureSet, 0, sizeof(*pOutSwapTextureSet));
    }
    DriverLog("CreateSwapTextureSet called: width=%d, height=%d, format=%d, samples=%d",
        pSwapTextureSetDesc->nWidth, pSwapTextureSetDesc->nHeight, pSwapTextureSetDesc->nFormat, pSwapTextureSetDesc->nSampleCount);
    WaitEncoderIdle();
    {
        std::lock_guard<std::mutex> lock(m_encoderMutex);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = pSwapTextureSetDesc->nWidth;
        desc.Height = pSwapTextureSetDesc->nHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = (DXGI_FORMAT)pSwapTextureSetDesc->nFormat;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        std::shared_ptr<SwapTextureSet> set = std::make_shared<SwapTextureSet>();
        set->nextIndex = 0;
        auto rollback = [&](int created) {
            for (int j = 0; j < created; j++) {
                m_textureHandleMap.erase((vr::SharedTextureHandle_t)set->hSharedHandles[j]);
                m_setByHandle.erase((vr::SharedTextureHandle_t)set->hSharedHandles[j]);
                if (set->pTextures[j]) {
                    set->pTextures[j]->Release();
                    set->pTextures[j] = nullptr;
                }
            }
        };
        for (int i = 0; i < 3; i++) {
            ID3D11Texture2D* pTexture = nullptr;
            HRESULT hr = m_pD3D11Device->CreateTexture2D(&desc, nullptr, &pTexture);
            if (FAILED(hr)) {
                DriverLog("Failed to create texture %d! HRESULT: 0x%x", i, hr);
                rollback(i);
                return;
            }
            IDXGIResource* pDXGIResource = nullptr;
            hr = pTexture->QueryInterface(__uuidof(IDXGIResource), (void**)&pDXGIResource);
            if (FAILED(hr)) {
                DriverLog("Failed to get DXGI resource for texture %d! HRESULT: 0x%x", i, hr);
                pTexture->Release();
                rollback(i);
                return;
            }
            HANDLE hSharedHandle = nullptr;
            hr = pDXGIResource->GetSharedHandle(&hSharedHandle);
            pDXGIResource->Release();
            if (FAILED(hr)) {
                DriverLog("Failed to get shared handle for texture %d! HRESULT: 0x%x", i, hr);
                pTexture->Release();
                rollback(i);
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
// Called by the compositor to release one triple-set; waits for the encoder to idle, then erases its handle entries.
void HmdDriver::DestroySwapTextureSet(vr::SharedTextureHandle_t sharedTextureHandle)
{
    DriverLog("DestroySwapTextureSet called: handle=%llu", (uint64_t)sharedTextureHandle);
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
// Called when a VR app exits; drops every swap-set for unPid and clears any queued frame.
void HmdDriver::DestroyAllSwapTextureSets(uint32_t unPid)
{
    DriverLog("DestroyAllSwapTextureSets called for pid=%d", unPid);
    m_sceneTearingDown = true;
    WaitEncoderIdle();
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
// Called by the compositor to rotate each eye's triple-buffer index; writes the next index into pIndices.
void HmdDriver::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t sharedTextureHandles[2], uint32_t(*pIndices)[2])
{
    if (!pIndices) return;
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
// Called by the compositor per submitted layer; queues handles + bounds for the Present thread (cap 16).
void HmdDriver::SubmitLayer(const SubmitLayerPerEye_t(&perEye)[2])
{
#ifdef DRIVER_DIAG
    static long long lastLog = 0;
    long long nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
    if (nowNs - lastLog >= 1'000'000'000LL) {
        lastLog = nowNs;
        DriverLog("SubmitLayer called - left: %llu, right: %llu",
            (uint64_t)perEye[0].hTexture, (uint64_t)perEye[1].hTexture);
    }
#endif
    SubmitLayerInfo info;
    info.hTextureLeft = perEye[0].hTexture;
    info.hTextureRight = perEye[1].hTexture;
    info.boundsLeft = perEye[0].bounds;
    info.boundsRight = perEye[1].bounds;
    {
        std::lock_guard<std::mutex> lock(m_submitLayersMutex);
        if (m_submitLayers.size() < 16) {
            m_submitLayers.push_back(info);
            m_hasSubmit.store(true, std::memory_order_release);
        }
    }
#ifdef DRIVER_DIAG
    g_submitLayerCount++;
#endif
}
// Called by the compositor at vsync with the sync texture; resolves queued layers into private copies and wakes the encoding thread.
void HmdDriver::Present(vr::SharedTextureHandle_t syncTexture)
{
    if (!m_streamEnabled.load(std::memory_order_relaxed)) {
        return;
    }
    m_presentCount++;
#ifdef DRIVER_DIAG
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
#endif
    if (!AcquireSyncTexture(syncTexture)) {
#ifdef DRIVER_DIAG
        static int syncFailCount = 0;
        if (++syncFailCount <= 5 || syncFailCount % 30 == 0)
            DriverLog("Present SKIPPED: AcquireSync failed (count=%d)", syncFailCount);
#endif
        return;
    }
    if (!m_hasSubmit.load(std::memory_order_acquire) || !m_encoderInitialized || !m_pVideoEncoder) {
#ifdef DRIVER_DIAG
        static int skipCount = 0;
        if (++skipCount <= 5 || skipCount % 30 == 0)
            DriverLog("Present SKIPPED: hasSubmit=%d encInit=%d enc=%p (count=%d)",
                      (int)m_hasSubmit.load(std::memory_order_relaxed), m_encoderInitialized, m_pVideoEncoder, skipCount);
#endif
        ReleaseSyncTexture();
        return;
    }
    std::vector<SubmitLayerInfo> submitted;
    {
        std::lock_guard<std::mutex> lock(m_submitLayersMutex);
        submitted.swap(m_submitLayers);
        m_submitLayers.clear();
        m_hasSubmit.store(false, std::memory_order_release);
    }
    if (m_sceneTearingDown) {
        ReleaseSyncTexture();
        return;
    }
    std::vector<SubmitLayerInfo> validLayers;
    std::vector<std::pair<ID3D11Texture2D*, ID3D11Texture2D*>> resolved;
    validLayers.reserve(submitted.size());
    resolved.reserve(submitted.size());
    for (const auto& layer : submitted) {
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
#ifdef DRIVER_DIAG
        static int mapFailCount = 0;
        if (++mapFailCount <= 5 || mapFailCount % 30 == 0)
            DriverLog("Map lookup FAILED: no valid layers of %zu submitted (map_size=%zu, count=%d)",
                submitted.size(), m_textureHandleMap.size(), mapFailCount);
#endif
        ReleaseSyncTexture();
        return;
    }
#ifdef DRIVER_DIAG
    {
        static int s_diagPresent = 0;
        if (++s_diagPresent % 120 == 0) {
            D3D11_TEXTURE2D_DESC dL = {}, dR = {};
            resolved.front().first->GetDesc(&dL);
            resolved.front().second->GetDesc(&dR);
            int submits = g_submitLayerCount.exchange(0);
            DriverLog("[DIAG Present] #%d AcquireOK=%d layers=%zu submits/120f=%d firstLfmt=0x%x(%ux%u) firstRfmt=0x%x(%ux%u)",
                s_diagPresent, (int)(m_syncAcquired ? 1 : 0), submitted.size(), submits,
                (uint32_t)dL.Format, dL.Width, dL.Height,
                (uint32_t)dR.Format, dR.Width, dR.Height);
        }
    }
#endif
    {
        std::lock_guard<std::mutex> lock(m_encodeDoneMutex);
        if (!m_encodeDone) {
#ifdef DRIVER_DIAG
            static int dropCount = 0;
            if (++dropCount <= 5 || dropCount % 60 == 0)
                DriverLog("Present DROPPED: encoder busy (drop=%d)", dropCount);
#endif
            ReleaseSyncTexture();
            return;
        }
    }
    {
        if (!EnsureLayerCopies(validLayers)) {
            DriverLog("Failed to create private eye copies on Present thread");
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
    ReleaseSyncTexture();
}
// Post-present hook (no-op); the compositor calls it after Present returns.
// Opens and acquires the compositor keyed-mutex sync texture; caches the handle while it stays valid.
void HmdDriver::PostPresent()
{
}
// Opens and acquires the compositor keyed-mutex sync texture; caches the handle while it stays valid.
bool HmdDriver::AcquireSyncTexture(vr::SharedTextureHandle_t syncTexture)
{
    if (!syncTexture) return false;
    HANDLE hSync = (HANDLE)syncTexture;
    if (hSync == INVALID_HANDLE_VALUE) return false;
    if (m_cachedSyncHandle != hSync || !m_pSyncMutex) {
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
// Releases the keyed-mutex acquired above; called on every Present exit path.
void HmdDriver::ReleaseSyncTexture()
{
    if (m_pSyncMutex && m_syncAcquired) {
        m_pSyncMutex->ReleaseSync(0);
        m_syncAcquired = false;
    }
}
// Creates or resizes private per-eye copies matching each submitted layer; lets the encoder hold textures past compositor reuse.
bool HmdDriver::EnsureLayerCopies(const std::vector<SubmitLayerInfo>& layers)
{
    if (m_layerCopies.size() < layers.size()) {
        m_layerCopies.resize(layers.size());
    }
    for (size_t i = 0; i < layers.size(); i++) {
        auto itL = m_textureHandleMap.find(layers[i].hTextureLeft);
        auto itR = m_textureHandleMap.find(layers[i].hTextureRight);
        if (itL == m_textureHandleMap.end() || itR == m_textureHandleMap.end()) {
            continue;
        }
        D3D11_TEXTURE2D_DESC dL = {}, dR = {};
        itL->second->GetDesc(&dL);
        itR->second->GetDesc(&dR);
        LayerCopy& c = m_layerCopies[i];
        bool needRecreate = (!c.pLeft || !c.pRight ||
                             c.width != (int)dL.Width || c.height != (int)dL.Height ||
                             c.format != dL.Format);
        if (!needRecreate) continue;
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
// Blocks texture teardown/re-init paths up to 2 s until the encoding thread signals completion.
// Frame-timing hook (timing output intentionally left empty); the compositor calls it per frame.
void HmdDriver::WaitEncoderIdle()
{
    std::unique_lock<std::mutex> lock(m_encodeDoneMutex);
    m_encodeDoneCv.wait_for(lock, std::chrono::seconds(2), [this] { return m_encodeDone; });
}
// Frame-timing hook (timing output intentionally left empty); the compositor calls it per frame.
void HmdDriver::GetFrameTiming(DriverDirectMode_FrameTiming* pFrameTiming)
{
#ifdef DRIVER_DIAG
    static long long lastLog = 0;
    long long nowNs = std::chrono::steady_clock::now().time_since_epoch().count();
    if (nowNs - lastLog >= 1'000'000'000LL) {
        lastLog = nowNs;
        DriverLog("GetFrameTiming called");
    }
#endif
}