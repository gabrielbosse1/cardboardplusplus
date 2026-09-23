"""
MediaPipe to OpenVR Hand Skeleton Visualizer
Camera (left) | OpenVR 31-bone skeleton (right)
Measurements from actual SteamVR GLB skeleton files.
"""
import cv2, numpy as np, mediapipe as mp, math, time, os
from mediapipe.tasks.python import BaseOptions
from mediapipe.tasks.python.vision import HandLandmarker, HandLandmarkerOptions, RunningMode
from scipy.spatial.transform import Rotation

# === MediaPipe Landmark Indices ===
WRIST=0; THUMB_CMC=1; THUMB_MCP=2; THUMB_IP=3; THUMB_TIP=4
INDEX_MCP=5; INDEX_PIP=6; INDEX_DIP=7; INDEX_TIP=8
MIDDLE_MCP=9; MIDDLE_PIP=10; MIDDLE_DIP=11; MIDDLE_TIP=12
RING_MCP=13; RING_PIP=14; RING_DIP=15; RING_TIP=16
PINKY_MCP=17; PINKY_PIP=18; PINKY_DIP=19; PINKY_TIP=20

# === OpenVR Bone Indices ===
BONE_ROOT=0; BONE_WRIST=1; BONE_THUMB0=2; BONE_THUMB1=3; BONE_THUMB2=4; BONE_THUMB3=5
BONE_INDEX0=6; BONE_INDEX1=7; BONE_INDEX2=8; BONE_INDEX3=9; BONE_INDEX4=10
BONE_MIDDLE0=11; BONE_MIDDLE1=12; BONE_MIDDLE2=13; BONE_MIDDLE3=14; BONE_MIDDLE4=15
BONE_RING0=16; BONE_RING1=17; BONE_RING2=18; BONE_RING3=19; BONE_RING4=20
BONE_PINKY0=21; BONE_PINKY1=22; BONE_PINKY2=23; BONE_PINKY3=24; BONE_PINKY4=25
BONE_AUX_THUMB=26; BONE_AUX_INDEX=27; BONE_AUX_MIDDLE=28; BONE_AUX_RING=29; BONE_AUX_PINKY=30
PARENT=[-1,0,1,2,3,4,1,6,7,8,9,1,11,12,13,14,1,16,17,18,19,1,21,22,23,24,0,0,0,0,0]

# Colors BGR
C=[(180,180,180),(255,255,255),(0,160,255),(0,160,255),(0,160,255),(0,160,255),
   (0,255,100),(0,255,100),(0,255,100),(0,255,100),(0,255,100),
   (255,200,0),(255,200,0),(255,200,0),(255,200,0),(255,200,0),
   (200,0,255),(200,0,255),(200,0,255),(200,0,255),(200,0,255),
   (0,100,255),(0,100,255),(0,100,255),(0,100,255),(0,100,255),
   (100,100,100),(100,100,100),(100,100,100),(100,100,100),(100,100,100)]

def v3(x,y,z): return np.array([x,y,z],dtype=np.float64)
def nrm(v):
    l=np.linalg.norm(v); return v/l

def build_rot(fwd, up_ref=None):
    f=nrm(fwd)
    if up_ref is None:
        r=nrm(np.cross(np.array([0,1,0]),f)) if abs(f[1])<0.9 else nrm(np.cross(np.array([1,0,0]),f))
    else:
        r=nrm(up_ref-np.dot(up_ref,f)*f)
        if np.linalg.norm(r)<1e-6:
            r=nrm(np.cross(np.array([0,1,0]),f)) if abs(f[1])<0.9 else nrm(np.cross(np.array([1,0,0]),f))
    u=nrm(np.cross(f,r))
    R=np.column_stack([r,u,f])
    if np.linalg.det(R)<0:
        R[:,0]=-R[:,0]; u=nrm(np.cross(f,R[:,0])); R[:,1]=u
    return Rotation.from_matrix(R).as_quat()

class HandDetector:
    def __init__(self, model_path="hand_landmarker.task"):
        if not os.path.exists(model_path):
            import urllib.request
            print("Downloading model...")
            urllib.request.urlretrieve("https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task", model_path)
        opts=HandLandmarkerOptions(base_options=BaseOptions(model_asset_path=model_path),
            running_mode=RunningMode.VIDEO, num_hands=1,
            min_hand_detection_confidence=0.5, min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5)
        self.det=HandLandmarker.create_from_options(opts)
    def detect(self, frame, ts_ms):
        rgb=cv2.cvtColor(frame,cv2.COLOR_BGR2RGB)
        img=mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
        return self.det.detect_for_video(img, ts_ms)
    def draw(self, frame, res):
        if res.hand_landmarks:
            for hl in res.hand_landmarks:
                for a,b in [(0,1),(1,2),(2,3),(3,4),(0,5),(5,6),(6,7),(7,8),
                    (5,9),(9,10),(10,11),(11,12),(9,13),(13,14),(14,15),(15,16),
                    (13,17),(17,18),(18,19),(19,20),(0,17)]:
                    p1=(int(hl[a].x*frame.shape[1]),int(hl[a].y*frame.shape[0]))
                    p2=(int(hl[b].x*frame.shape[1]),int(hl[b].y*frame.shape[0]))
                    cv2.line(frame,p1,p2,(0,255,0),2); cv2.circle(frame,p1,3,(0,255,0),-1)
        return frame

class Converter:
    # MediaPipe world: +X right, +Y DOWN, +Z AWAY from camera (smaller = closer).
    # OpenVR: +X right, +Y UP, +Z toward user (nearer the head). Negate Y and Z.
    def _to_arr(self, lm):
        """Convert a landmark to numpy array (OpenVR axes)."""
        if hasattr(lm, 'x'):
            try:
                x, y, z = float(lm.x), float(lm.y), float(lm.z)
                return v3(x, -y, -z)
            except (TypeError, AttributeError):
                pass
        try:
            x, y, z = float(lm[0]), float(lm[1]), float(lm[2])
            return v3(x, -y, -z)
        except (TypeError, ValueError, IndexError):
            return v3(0,0,0)
    def convert(self, landmarks, world):
        if not world or len(world)==0 or len(world[0])==0: return None
        wl = world[0]  # First hand's landmarks
        lm = np.array([self._to_arr(l) for l in wl])
        b=[None]*31
        w=lm[WRIST]; tc=lm[THUMB_CMC]; tm=lm[THUMB_MCP]; ti=lm[THUMB_IP]; tt=lm[THUMB_TIP]
        imc=lm[INDEX_MCP]; ip=lm[INDEX_PIP]; idp=lm[INDEX_DIP]; it=lm[INDEX_TIP]
        mmc=lm[MIDDLE_MCP]; mp_=lm[MIDDLE_PIP]; mdp=lm[MIDDLE_DIP]; mtt=lm[MIDDLE_TIP]
        rmc=lm[RING_MCP]; rp=lm[RING_PIP]; rdp=lm[RING_DIP]; rt=lm[RING_TIP]
        pmc=lm[PINKY_MCP]; pp=lm[PINKY_PIP]; pdp=lm[PINKY_DIP]; pt=lm[PINKY_TIP]
        # Direct map: wrist stays at MediaPipe's anatomical wrist.
        # Valve metacarpals are short (wrist sits up in the palm): place
        # each Finger0 from the wrist along wrist->MCP at Valve's GLB
        # length (index 25.8 / middle 17.9 / ring 17.6 / pinky 24.5mm),
        # instead of at the MCP itself (which gives ~90mm grey base bones).
        # (Valve-offset solver removed for diagnosis.)
        b[BONE_ROOT]=w.copy(); b[BONE_WRIST]=w.copy()
        def _short_meta(mcp, L):
            d = mcp - w
            l = float(np.linalg.norm(d))
            return w + d * (L / l)
        imc0=_short_meta(imc, 0.0258); mmc0=_short_meta(mmc, 0.0179)
        rmc0=_short_meta(rmc, 0.0176); pmc0=_short_meta(pmc, 0.0245)
        # Thumb: 4 MediaPipe points -> 4 bones, direct map.
        b[BONE_THUMB0]=tc.copy(); b[BONE_THUMB1]=tm.copy(); b[BONE_THUMB2]=ti.copy(); b[BONE_THUMB3]=tt.copy(); b[BONE_AUX_THUMB]=tt.copy()
        # Fingers: Finger0 (meta) is the metacarpal stub at Valve's GLB length
        # (17-26mm from wrist). Finger1..4 = MCP, PIP, DIP, TIP direct.
        # AUX tracks the fingertip.
        b[BONE_INDEX0]=imc0; b[BONE_INDEX1]=imc.copy(); b[BONE_INDEX2]=ip.copy(); b[BONE_INDEX3]=idp.copy(); b[BONE_INDEX4]=it.copy(); b[BONE_AUX_INDEX]=it.copy()
        b[BONE_MIDDLE0]=mmc0; b[BONE_MIDDLE1]=mmc.copy(); b[BONE_MIDDLE2]=mp_.copy(); b[BONE_MIDDLE3]=mdp.copy(); b[BONE_MIDDLE4]=mtt.copy(); b[BONE_AUX_MIDDLE]=mtt.copy()
        b[BONE_RING0]=rmc0; b[BONE_RING1]=rmc.copy(); b[BONE_RING2]=rp.copy(); b[BONE_RING3]=rdp.copy(); b[BONE_RING4]=rt.copy(); b[BONE_AUX_RING]=rt.copy()
        b[BONE_PINKY0]=pmc0; b[BONE_PINKY1]=pmc.copy(); b[BONE_PINKY2]=pp.copy(); b[BONE_PINKY3]=pdp.copy(); b[BONE_PINKY4]=pt.copy(); b[BONE_AUX_PINKY]=pt.copy()
        return b

GLOVE_LEFT_PATH = "C:/Program Files (x86)/Steam/steamapps/common/SteamVR/resources/rendermodels/vr_glove/vr_glove_left_model_slim.glb"
GLOVE_RIGHT_PATH = "C:/Program Files (x86)/Steam/steamapps/common/SteamVR/resources/rendermodels/vr_glove/vr_glove_right_model_slim.glb"
TRI_STRIDE = 1  # full mesh (4899 tris); decimation by stride leaves holes

def kabsch_rigid(A, B):
    """Rigid transform mapping A->B (Nx3 each). Returns R(3x3), t(3,)."""
    ca = A.mean(axis=0); cb = B.mean(axis=0)
    H = (A - ca).T @ (B - cb)
    U, _, Vt = np.linalg.svd(H)
    R = Vt.T @ U.T
    if np.linalg.det(R) < 0:
        Vt[-1, :] *= -1
        R = Vt.T @ U.T
    return R, cb - R @ ca

class GloveMesh:
    """Valve vr_glove slim mesh with linear-blend skinning driven by live bones."""
    AUX_JOINTS = frozenset((0, 26, 27, 28, 29, 30))
    # skin-joint topology: (node id, child node id or None=reuse parent delta)
    JTREE = [(1,None),(2,12),
        (3,4),(4,5),(5,6),(6,None),
        (7,8),(8,9),(9,10),(10,11),(11,None),
        (12,13),(13,14),(14,15),(15,16),(16,None),
        (17,18),(18,19),(19,20),(20,21),(21,None),
        (22,23),(23,24),(24,25),(25,26),(26,None),
        (27,None),(28,None),(29,None),(30,None),(31,None)]
    # live-bone direction for each skin joint: (from bone, to bone or None)
    # Finger0 (meta) = metacarpal stub, Finger1 (bone) sits AT the MCP.
    JDIR = [(BONE_ROOT,None),(BONE_WRIST,BONE_MIDDLE0),
        (BONE_THUMB0,BONE_THUMB1),(BONE_THUMB1,BONE_THUMB2),(BONE_THUMB2,BONE_THUMB3),(BONE_THUMB3,None),
        (BONE_INDEX0,BONE_INDEX1),(BONE_INDEX1,BONE_INDEX2),(BONE_INDEX2,BONE_INDEX3),(BONE_INDEX3,BONE_INDEX4),(BONE_INDEX4,None),
        (BONE_MIDDLE0,BONE_MIDDLE1),(BONE_MIDDLE1,BONE_MIDDLE2),(BONE_MIDDLE2,BONE_MIDDLE3),(BONE_MIDDLE3,BONE_MIDDLE4),(BONE_MIDDLE4,None),
        (BONE_RING0,BONE_RING1),(BONE_RING1,BONE_RING2),(BONE_RING2,BONE_RING3),(BONE_RING3,BONE_RING4),(BONE_RING4,None),
        (BONE_PINKY0,BONE_PINKY1),(BONE_PINKY1,BONE_PINKY2),(BONE_PINKY2,BONE_PINKY3),(BONE_PINKY3,BONE_PINKY4),(BONE_PINKY4,None),
        (BONE_AUX_THUMB,None),(BONE_AUX_INDEX,None),(BONE_AUX_MIDDLE,None),(BONE_AUX_RING,None),(BONE_AUX_PINKY,None),
    ]
    def __init__(self, path):
        from pygltflib import GLTF2
        import struct as _st
        g = GLTF2().load(path)
        blob = g.binary_blob()
        _COMP = {5121: 'B', 5123: 'H', 5125: 'I', 5126: 'f'}
        _N = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
        def _read(i):
            a = g.accessors[i]; bv = g.bufferViews[a.bufferView]
            off = (bv.byteOffset or 0) + (a.byteOffset or 0)
            n = a.count * _N[a.type]
            vals = _st.unpack_from('<' + str(n) + _COMP[a.componentType], blob, off)
            return np.array(vals, dtype=np.float64).reshape(a.count, _N[a.type])
        prim = g.meshes[0].primitives[0]
        self.verts = _read(prim.attributes.POSITION)
        idx = _read(prim.indices).ravel().astype(np.int64)
        try:
            self.normals = _read(prim.attributes.NORMAL)
        except Exception:
            self.normals = None
        self.tris = idx.reshape(-1, 3)[::TRI_STRIDE]
        self.joints = _read(prim.attributes.JOINTS_0).astype(np.int64)
        self.weights = _read(prim.attributes.WEIGHTS_0)
        # Valve's slim export isn't weight-normalized (rows sum up to ~2) -> fix
        s = self.weights.sum(axis=1, keepdims=True)
        self.weights = np.divide(self.weights, np.maximum(s, 1e-9))
        self.ibm = _read(g.skins[0].inverseBindMatrices).reshape(-1, 4, 4, order='F')  # glTF MAT4 is column-major
        assert self.ibm.shape[0] == 31, self.ibm.shape
        # mesh node world matrix (node 32, ~identity, kept for correctness)
        self.mesh_world = self._node_world(g, 32)
        # bind joint world pos/rot (scene space) for delta-based retargeting
        self.joint_nodes = list(g.skins[0].joints)
        bw = np.zeros((len(self.joint_nodes), 3)); br = np.zeros((len(self.joint_nodes), 3, 3))
        for j, nid in enumerate(self.joint_nodes):
            M = self._node_world(g, int(nid))
            bw[j] = M[:3, 3]; br[j] = M[:3, :3]
        self.bind_jpos = bw; self.bind_jrot = br
        self._g = None  # release gltf tree (keep arrays only)
        # bind segment lengths (node world distances) keyed by live bone pairs.
        # 5 nodes per finger (meta,0,1,2,end); live *0..*4 sit on meta,0,1,2,end.
        _chains = [
            ([BONE_THUMB0, BONE_THUMB1, BONE_THUMB2, BONE_THUMB3], [3, 4, 5, 6]),
            ([BONE_INDEX0, BONE_INDEX1, BONE_INDEX2, BONE_INDEX3, BONE_INDEX4], [7, 8, 9, 10, 11]),
            ([BONE_MIDDLE0, BONE_MIDDLE1, BONE_MIDDLE2, BONE_MIDDLE3, BONE_MIDDLE4], [12, 13, 14, 15, 16]),
            ([BONE_RING0, BONE_RING1, BONE_RING2, BONE_RING3, BONE_RING4], [17, 18, 19, 20, 21]),
            ([BONE_PINKY0, BONE_PINKY1, BONE_PINKY2, BONE_PINKY3, BONE_PINKY4], [22, 23, 24, 25, 26]),
        ]
        _npos = {int(n): bw[j] for j, n in enumerate(self.joint_nodes)}
        _wrist = _npos[2]
        self.seg_len = []
        for bones_c, nodes_c in _chains:
            self.seg_len.append((BONE_WRIST, bones_c[0], float(np.linalg.norm(_npos[nodes_c[0]] - _wrist))))
            for fa, ta, fn, tn in zip(bones_c[:-1], bones_c[1:], nodes_c[:-1], nodes_c[1:]):
                self.seg_len.append((fa, ta, float(np.linalg.norm(_npos[tn] - _npos[fn]))))
        # bind verts/normals in scene space
        vw = self.mesh_world
        self.bind_v = (vw[:3, :3] @ self.verts.T).T + vw[:3, 3]
        if self.normals is not None:
            self.bind_n = self.normals @ vw[:3, :3].T
        else:
            self.bind_n = None

    @staticmethod
    def _rot_axis(a, th):
        """Rotation matrix about unit axis a by angle th (Rodrigues)."""
        a = a / max(float(np.linalg.norm(a)), 1e-9)
        K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
        return np.eye(3) + math.sin(th) * K + (1 - math.cos(th)) * (K @ K)

    @staticmethod
    def _align_rot(a, b):
        """Minimal rotation mapping unit vector a -> b."""
        c = float(np.dot(a, b))
        if c > 1 - 1e-9:
            return np.eye(3)
        if c < -1 + 1e-9:
            p = np.array([1., 0., 0.]) if abs(a[0]) < 0.9 else np.array([0., 1., 0.])
            v = np.cross(a, p); v /= np.linalg.norm(v)
            return 2 * np.outer(v, v) - np.eye(3)
        v = np.cross(a, b)
        K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
        return np.eye(3) + K + K @ K / (1 + c)

    def live_mats(self, bones):
        """31 live joint world matrices: bind orientation + delta to live bone dirs."""
        P = lambda i: bones[i]
        jpos = {int(n): self.bind_jpos[j] for j, n in enumerate(self.joint_nodes)}
        mats = np.zeros((31, 4, 4))
        prev_D = np.eye(3)
        for j, ((nid, child), (pb, tb)) in enumerate(zip(self.JTREE, self.JDIR)):
            M = np.eye(4)
            M[:3, 3] = P(pb)
            if j in self.AUX_JOINTS or child is None or tb is None:
                if child is None and tb is None:
                    D = prev_D  # tip/end/aux reuse parent delta
                else:
                    D = np.eye(3)
                    prev_D = D
                M[:3, :3] = D @ self.bind_jrot[j]
            else:
                db = jpos[child] - jpos[nid]
                dl = P(tb) - P(pb)
                if np.linalg.norm(db) < 1e-9 or np.linalg.norm(dl) < 1e-9:
                    D = prev_D
                else:
                    D = self._align_rot(db / np.linalg.norm(db), dl / np.linalg.norm(dl))
                prev_D = D
                M[:3, :3] = D @ self.bind_jrot[j]
            mats[j] = M
        return mats

    def normalize(self, bones):
        """Rescale live segments to Valve bind lengths along live directions.

        Kills rubber-stretch from MediaPipe scale noise; curl angles untouched.
        Returns a new bone list (input untouched)."""
        nb = [b.copy() if b is not None else None for b in bones]
        for fa, ta, L in self.seg_len:
            if nb[fa] is None or nb[ta] is None:
                continue
            d = nb[ta] - nb[fa]
            l = float(np.linalg.norm(d))
            if l < 1e-9:
                continue
            nb[ta] = nb[fa] + d * (L / l)
        for aux, tip in ((BONE_AUX_THUMB, BONE_THUMB3), (BONE_AUX_INDEX, BONE_INDEX4),
                         (BONE_AUX_MIDDLE, BONE_MIDDLE4), (BONE_AUX_RING, BONE_RING4),
                         (BONE_AUX_PINKY, BONE_PINKY4)):
            if nb[tip] is not None:
                nb[aux] = nb[tip].copy()
        return nb

    def skin(self, bones):
        """Linear-blend skinning -> (verts, normals) in live world space."""
        return self.skin_from_mats(self.live_mats(bones))

    def skin_from_mats(self, M):
        """Skin from precomputed (31,4,4) joint matrices (allows smoothing)."""
        M = M @ self.ibm  # (31,4,4)
        R = M[:, :3, :3]; t = M[:, :3, 3]
        V = self.bind_v
        out = np.zeros_like(V)
        for k in range(4):
            j = self.joints[:, k]
            w = self.weights[:, k]
            nz = w > 1e-9
            if not np.any(nz):
                continue
            # per-vertex joint transform: out[i] += w[i] * (R[j[i]] @ V[i] + t[j[i]])
            Rv = np.einsum('nij,nj->ni', R[j[nz]], V[nz]) + t[j[nz]]
            out[nz] += Rv * w[nz][:, None]
        if self.bind_n is not None:
            nout = np.zeros_like(self.bind_n)
            for k in range(4):
                j = self.joints[:, k]
                w = self.weights[:, k]
                nz = w > 1e-9
                if not np.any(nz):
                    continue
                nout[nz] += np.einsum('nij,nj->ni', R[j[nz]], self.bind_n[nz]) * w[nz][:, None]
            nl = np.linalg.norm(nout, axis=1, keepdims=True)
            nout = np.divide(nout, np.maximum(nl, 1e-9))
        else:
            nout = None
        return out, nout

    @staticmethod
    def _local_mat(n):
        from scipy.spatial.transform import Rotation as _R
        T = np.eye(4)
        if n.rotation:
            T[:3, :3] = _R.from_quat([n.rotation[0], n.rotation[1], n.rotation[2], n.rotation[3]]).as_matrix()
        if n.translation:
            T[:3, 3] = n.translation
        s = n.scale or [1, 1, 1]
        T[:3, :3] *= np.array(s, dtype=np.float64)
        return T

    def _node_world(self, g, target):
        kids = {}
        for i, n in enumerate(g.nodes):
            for c in (n.children or []):
                kids[c] = i
        chain, cur = [target], target
        while cur in kids:
            cur = kids[cur]
            chain.append(cur)
        M = np.eye(4)
        for i in reversed(chain):
            M = M @ self._local_mat(g.nodes[i])
        return M

class Renderer3D:
    def __init__(self, w, h):
        self.w=w; self.h=h; self.cx=w//2; self.cy=h//2+40
        self.dist=0.50; self.fov=800.0; self.rx=-20.0; self.ry=0.0
        self.sarr=None; self.alpha=0.5
        self.show_mesh=False  # stick skeleton by default; G toggles glove mesh
        self.show_skeleton=True
        self.meshes={}  # 'Left'/'Right' -> GloveMesh (lazy)
        self._prev_mats=None; self._prev_hand=None; self.mat_alpha=0.6
        self._prev_filt=None  # last smoothed end skeleton
        self.final_alpha=0.6  # final light smoothing on the end skeleton
    def _mesh_for(self, hand):
        key = hand if hand in ("Left", "Right") else "Left"
        if key not in self.meshes:
            try:
                p = GLOVE_LEFT_PATH if key == "Left" else GLOVE_RIGHT_PATH
                self.meshes[key] = GloveMesh(p)
                print(f"Loaded OpenVR glove mesh ({key}): "
                      f"{len(self.meshes[key].verts)} verts, {len(self.meshes[key].tris)} tris")
            except Exception as e:
                print(f"Glove mesh load failed ({key}): {e}; using stick fallback")
                self.meshes[key] = None
        return self.meshes.get(key)
    def proj(self, p):
        rx,ry=math.radians(self.rx),math.radians(self.ry)
        x=p[0]*math.cos(ry)+p[2]*math.sin(ry); z=-p[0]*math.sin(ry)+p[2]*math.cos(ry); y=p[1]
        y2=y*math.cos(rx)-z*math.sin(rx); z2=y*math.sin(rx)+z*math.cos(rx)
        zo=z2+self.dist
        if zo<0.01: zo=0.01
        s=self.fov/zo
        return (int(self.cx+x*s),int(self.cy-y2*s)),zo
    def proj_batch(self, pts):
        rx,ry=math.radians(self.rx),math.radians(self.ry)
        cy,sy=math.cos(ry),math.sin(ry); cx,sx=math.cos(rx),math.sin(rx)
        x=pts[:,0]*cy+pts[:,2]*sy; z=-pts[:,0]*sy+pts[:,2]*cy; y=pts[:,1]
        y2=y*cx-z*sx; z2=y*sx+z*cx
        zo=np.maximum(z2+self.dist,0.01); s=self.fov/zo
        return (self.cx+x*s).astype(np.int32), (self.cy-y2*s).astype(np.int32), zo
    def smooth(self, bones):
        if bones is None: return None
        if self.sarr is None:
            self.sarr=[b.copy() if b is not None else None for b in bones]; return self.sarr
        r=[]
        for i in range(len(bones)):
            if bones[i] is not None and self.sarr[i] is not None:
                r.append(self.sarr[i]+(bones[i]-self.sarr[i])*self.alpha)
            elif bones[i] is not None: r.append(bones[i].copy())
            else: r.append(None)
        self.sarr=[b.copy() if b is not None else None for b in r]; return self.sarr
    def render(self, frame, bones, handedness="Left"):
        frame[:]=0
        if bones is None:
            cv2.putText(frame,"No hand",(self.w//2-50,self.h//2),cv2.FONT_HERSHEY_SIMPLEX,0.7,(60,60,60),2); return
        bones=self.smooth(bones)
        # Grid lines
        for i in range(-4,5):
            for av in [-0.15,0.15]:
                p1,_=self.proj(v3(av,0,i*0.04)); p2,_=self.proj(v3(-av,0,i*0.04))
                cv2.line(frame,p1,p2,(20,20,20),1)
        mesh = self._mesh_for(handedness)
        if mesh is not None and self.show_mesh:
            try:
                # Valve proportions + extra filtering only for the mesh path;
                # stick skeleton stays raw MediaPipe for diagnosis.
                bones = mesh.normalize(bones)
                # then a final light smoothing pass on the end skeleton
                if self._prev_hand != handedness:
                    self._prev_filt = None
                a = self.final_alpha
                if self._prev_filt is not None:
                    sm = [b.copy() if b is not None else None for b in bones]
                    for i in range(len(sm)):
                        if sm[i] is not None and self._prev_filt[i] is not None:
                            sm[i] = self._prev_filt[i] + (sm[i] - self._prev_filt[i]) * a
                    bones = sm
                self._prev_filt = [b.copy() if b is not None else None for b in bones]
            except Exception as e:
                print("filter err:", e)
        if mesh is not None and self.show_mesh:
            try:
                mats = mesh.live_mats(bones)  # bones already normalized+filtered above
                # smooth joint matrices so MP depth noise doesn't snap fingers
                if self._prev_mats is not None and self._prev_hand == handedness:
                    a = self.mat_alpha
                    Rm = self._prev_mats[:, :3, :3] + (mats[:, :3, :3] - self._prev_mats[:, :3, :3]) * a
                    for j in range(Rm.shape[0]):
                        U, _, Vt = np.linalg.svd(Rm[j])
                        Rm[j] = U @ Vt
                    tm = self._prev_mats[:, :3, 3] + (mats[:, :3, 3] - self._prev_mats[:, :3, 3]) * a
                    mats = np.zeros_like(mats); mats[:, :3, :3] = Rm; mats[:, :3, 3] = tm; mats[:, 3, 3] = 1
                self._prev_mats = mats.copy(); self._prev_hand = handedness
                V, N = mesh.skin_from_mats(mats)  # articulated: fingers follow tracking
                self._draw_mesh(frame, mesh, V, N)
            except Exception as e:
                cv2.putText(frame,"mesh err",(10,45),cv2.FONT_HERSHEY_SIMPLEX,0.5,(0,0,255),1)
                print("mesh render err:", e)
                self._draw_skeleton(frame, bones)
        else:
            self._draw_skeleton(frame, bones)
        if self.show_skeleton and mesh is not None:
            self._draw_skeleton(frame, bones, thin=True)
        # Labels
        for idx,nm in [(BONE_THUMB3,"Thumb"),(BONE_INDEX4,"Index"),(BONE_MIDDLE4,"Middle"),(BONE_RING4,"Ring"),(BONE_PINKY4,"Pinky")]:
            if bones[idx] is not None:
                (px,py),_=self.proj(bones[idx]); cv2.putText(frame,nm,(px+10,py-10),cv2.FONT_HERSHEY_SIMPLEX,0.4,C[idx],1,cv2.LINE_AA)
    def _draw_mesh(self, frame, mesh, V, N):
        px, py, zo = self.proj_batch(V)
        tris = mesh.tris
        p0x, p0y = px[tris[:,0]], py[tris[:,0]]
        p1x, p1y = px[tris[:,1]], py[tris[:,1]]
        p2x, p2y = px[tris[:,2]], py[tris[:,2]]
        # no backface culling: glTF winding flips sign under our view
        # transform, and culling left holes. Painter's algorithm handles it.
        zavg = (zo[tris[:,0]]+zo[tris[:,1]]+zo[tris[:,2]])/3.0
        order = np.argsort(-zavg)  # far -> near
        # flat shade from skinned normals
        light = np.array([0.4, 0.8, 0.6]); light/=np.linalg.norm(light)
        if N is not None:
            shade = 0.45 + 0.55*np.clip(N @ light, 0, 1)
        else:
            shade = np.full(len(V), 0.75)
        tavg = (shade[tris[order,0]]+shade[tris[order,1]]+shade[tris[order,2]])/3.0
        H, W = frame.shape[:2]
        for k, f in enumerate(order):
            s = tavg[k]; col = (int(150*s+40), int(165*s+40), int(185*s+40))
            a = tris[f]
            q0=(int(px[a[0]]),int(py[a[0]])); q1=(int(px[a[1]]),int(py[a[1]])); q2=(int(px[a[2]]),int(py[a[2]]))
            if min(q0[0],q1[0],q2[0])<-50 or max(q0[0],q1[0],q2[0])>W+50: continue
            if min(q0[1],q1[1],q2[1])<-50 or max(q0[1],q1[1],q2[1])>H+50: continue
            cv2.fillConvexPoly(frame, np.array([q0,q1,q2],dtype=np.int32), col, cv2.LINE_AA)
    def _draw_skeleton(self, frame, bones, thin=False):
        th_b, th_j = (1, 3) if thin else (3, 5)
        for i in range(1,31):
            p=PARENT[i]
            if p>=0 and bones[i] is not None and bones[p] is not None:
                self._bone(frame,bones[p],bones[i],C[i],th_b)
        for i in range(31):
            if bones[i] is not None: self._joint(frame,bones[i],C[i],th_j)
    def _bone(self,frame,p1,p2,color,thick):
        (x1,y1),z1=self.proj(p1); (x2,y2),z2=self.proj(p2)
        t=max(1,int(thick*(self.dist/max(z1,0.01))))
        cv2.line(frame,(x1,y1),(x2,y2),color,t,cv2.LINE_AA)
    def _joint(self,frame,p,color,r):
        (px,py),z=self.proj(p); rr=max(2,int(r*(self.dist/max(z,0.01))))
        cv2.circle(frame,(px,py),rr,color,-1,cv2.LINE_AA)
        cv2.circle(frame,(px,py),rr+1,(255,255,255),1,cv2.LINE_AA)

def estimate_pose(bones):
    if not bones or bones[BONE_WRIST] is None: return None,None
    w=bones[BONE_WRIST]
    idx=bones[BONE_INDEX0] if bones[BONE_INDEX0] is not None else w
    mid=bones[BONE_MIDDLE0] if bones[BONE_MIDDLE0] is not None else w
    pk=bones[BONE_PINKY0] if bones[BONE_PINKY0] is not None else w
    palm=(idx+mid+pk)/3; pos=w+(palm-w)*0.3
    f=nrm(mid-w); a=nrm(pk-idx); u=nrm(np.cross(a,f))
    R=np.column_stack([a,u,f])
    if np.linalg.det(R)<0: R[:,0]=-R[:,0]; u=nrm(np.cross(f,R[:,0])); R[:,1]=u
    return pos, Rotation.from_matrix(R).as_quat()

def main():
    print("="*55); print("  MediaPipe -> OpenVR Hand Skeleton Visualizer"); print("="*55)
    det=HandDetector(); conv=Converter(); ren=Renderer3D(480,480)
    cap=cv2.VideoCapture(0)
    if not cap.isOpened(): print("Cannot open camera!"); return
    cap.set(cv2.CAP_PROP_FRAME_WIDTH,640); cap.set(cv2.CAP_PROP_FRAME_HEIGHT,480)
    CW,CH=640,480; WINW=CW+480
    cv2.namedWindow("MediaPipe -> OpenVR",cv2.WINDOW_NORMAL); cv2.resizeWindow("MediaPipe -> OpenVR",WINW,CH)
    dragging=False; lmx=0; lmy=0
    def onmouse(ev,x,y,fl,param):
        nonlocal dragging,lmx,lmy
        if ev==cv2.EVENT_LBUTTONDOWN and x>CW: dragging=True; lmx,lmy=x,y
        elif ev==cv2.EVENT_LBUTTONUP: dragging=False
        elif ev==cv2.EVENT_MOUSEMOVE and dragging:
            ren.ry+=(x-lmx)*0.5; ren.rx=max(-80,min(80,ren.rx+(y-lmy)*0.5)); lmx,lmy=x,y
    cv2.setMouseCallback("MediaPipe -> OpenVR",onmouse)
    fc=0; ft=time.time(); fd=0
    locked="Left"; lock_run=0  # handedness lock: MP flickers L/R, swapping meshes pops
    print("Running! Drag on right panel to rotate. +/- zoom, G glove mesh, M skeleton, R reset, Q/ESC quit.")
    while True:
        ret,frame=cap.read()
        if not ret: break
        frame=cv2.flip(frame,1); ts=int(time.time()*1000)
        res=det.detect(frame,ts)
        cam=det.draw(frame.copy(),res)
        bones=None; hn=None
        if res.hand_landmarks and res.handedness:
            hn=res.handedness[0][0].category_name
            if hn == locked:
                lock_run = min(lock_run + 1, 8)
            else:
                lock_run += 1
                if lock_run >= 8:
                    locked = hn; lock_run = 0
                    ren._prev_mats = None; ren._prev_filt = None
            bones=conv.convert(res.hand_landmarks,res.hand_world_landmarks)
        p3d=np.zeros((CH,480,3),dtype=np.uint8); ren.render(p3d,bones,locked)
        cv2.putText(cam,"Camera+MediaPipe",(10,25),cv2.FONT_HERSHEY_SIMPLEX,0.6,(0,255,0),2)
        cv2.putText(p3d,"OpenVR Glove" if ren.show_mesh else "OpenVR Skeleton",(10,25),cv2.FONT_HERSHEY_SIMPLEX,0.6,(0,200,255),2)
        cv2.putText(cam,f"FPS:{fd:.0f}",(10,CH-10),cv2.FONT_HERSHEY_SIMPLEX,0.5,(0,255,0),1)
        if hn: cv2.putText(cam,f"Hand:{hn}",(120,CH-10),cv2.FONT_HERSHEY_SIMPLEX,0.5,(0,255,0),1)
        if bones:
            pos,q=estimate_pose(bones)
            if pos is not None: cv2.putText(cam,f"Ctrl:[{pos[0]:.3f},{pos[1]:.3f},{pos[2]:.3f}]",(250,CH-10),cv2.FONT_HERSHEY_SIMPLEX,0.4,(0,200,200),1)
        cv2.line(cam,(CW-1,0),(CW-1,CH),(100,100,100),2)
        cv2.imshow("MediaPipe -> OpenVR",np.hstack([cam,p3d]))
        fc+=1
        if time.time()-ft>=1.0: fd=fc/(time.time()-ft); fc=0; ft=time.time()
        k=cv2.waitKey(1)&0xFF
        if k in (ord("q"),27): break
        elif k in (ord("+"),ord("=")): ren.dist=max(0.15,ren.dist-0.02)
        elif k in (ord("-"),ord("_")): ren.dist=min(1.5,ren.dist+0.02)
        elif k in (ord("m"),ord("M")): ren.show_skeleton=not ren.show_skeleton
        elif k in (ord("g"),ord("G")): ren.show_mesh=not ren.show_mesh; ren._prev_mats=None
        elif k in (ord("r"),ord("R")): ren.rx=-20; ren.ry=0; ren.dist=0.50; ren.sarr=None; ren._prev_mats=None; ren._prev_filt=None
    det.det.close(); cap.release(); cv2.destroyAllWindows(); print("Done!")

if __name__=="__main__": main()
