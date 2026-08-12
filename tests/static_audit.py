from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
code = "\n".join(p.read_text(errors="replace") for base in (root / "src", root / "include") for p in base.rglob("*") if p.suffix in {".cpp", ".hpp"})
forbidden = {
    "INADDR_ANY": "dashboard must not bind every interface",
    "Access-Control-Allow-Origin": "dashboard must not opt into cross-origin API access",
    "Sleep(5)": "WASAPI capture must remain event driven",
    "d3d12.h": "v0.1 intentionally excludes the D3D12 artwork hook",
    "kiero": "v0.1 intentionally excludes graphics hooking",
    "MinHook": "v0.1 intentionally excludes graphics hooking",
    "force_stereo_audio": "old unsafe channel-forcing path must not return",
}
failed = False
for needle, reason in forbidden.items():
    if needle in code:
        print(f"FAIL: found {needle!r}: {reason}")
        failed = True

bridge = (root / "src/fmod/dsp_bridge.cpp").read_text()
required = [
    "safe_channel_count",
    "kPrebufferFrames",
    "consumer_apply_reset",
    "primed_.store(false",
]
for needle in required:
    if needle not in bridge:
        print(f"FAIL: missing DSP safety marker {needle!r}")
        failed = True

http = (root / "src/http/http_server.cpp").read_text()
for needle in ['inet_pton(AF_INET, "127.0.0.1"', "cross-origin request rejected", "kMaxHeaders", "kMaxBody", "server.exchange"]:
    if needle not in http:
        print(f"FAIL: missing HTTP hardening marker {needle!r}")
        failed = True

ring_h = (root / "include/fh6r/audio_ring.hpp").read_text()
if "request_reset" not in ring_h or "consumer_apply_reset" not in ring_h:
    print("FAIL: ring reset must be consumer-owned")
    failed = True

wasapi = (root / "src/wasapi_capture.cpp").read_text()
for needle in ["AUDCLNT_STREAMFLAGS_EVENTCALLBACK", "AvSetMmThreadCharacteristicsW", "worker_alive_", "drift::resample_step", "ring_.request_reset"]:
    if needle not in wasapi:
        print(f"FAIL: missing WASAPI reliability marker {needle!r}")
        failed = True

fmod_h = (root / "include/fh6r/fmod/dsp_bridge.hpp").read_text()
if "resolver && unlock" not in fmod_h:
    print("FAIL: FMOD handle unlock must be mandatory")
    failed = True

sys.exit(1 if failed else 0)
