#include "fh6r/camera_tracker.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

// Synthetic FH6 camera rig. The car drives along +x at constant speed; each
// view adds its car-relative offset. Dashboard and Cockpit share the interior
// pose, matching the measured FH6 behaviour.
namespace {

constexpr float kDt = 0.01f;
constexpr float kSpeed = 20.0f; // m/s, straight line

struct ViewGeo {
    float off[3];
    float fwd[3];
    float up[3];
};

constexpr ViewGeo kInterior{{0.0f, 1.1f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
constexpr ViewGeo kHood    {{1.8f, 1.2f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
constexpr ViewGeo kBumper  {{2.8f, 0.5f, 0.0f}, {0.9928f, -0.1191f, 0.0f}, {0.1191f, 0.9928f, 0.0f}};
constexpr ViewGeo kNear    {{-4.5f, 1.8f, 0.0f}, {0.9806f, -0.1961f, 0.0f}, {0.1961f, 0.9806f, 0.0f}};
constexpr ViewGeo kFar     {{-7.5f, 2.6f, 0.0f}, {0.9523f, -0.3048f, 0.0f}, {0.3048f, 0.9523f, 0.0f}};

const ViewGeo& geo_for(fh6r::CabinMode v) {
    switch (v) {
        case fh6r::CabinMode::Hood: return kHood;
        case fh6r::CabinMode::Bumper: return kBumper;
        case fh6r::CabinMode::ChaseNear: return kNear;
        case fh6r::CabinMode::ChaseFar: return kFar;
        default: return kInterior; // Dashboard + Cockpit share one pose
    }
}

fh6r::CabinMode next_in_cycle(fh6r::CabinMode v) {
    switch (v) {
        case fh6r::CabinMode::Dashboard: return fh6r::CabinMode::Hood;
        case fh6r::CabinMode::Hood: return fh6r::CabinMode::Bumper;
        case fh6r::CabinMode::Bumper: return fh6r::CabinMode::ChaseNear;
        case fh6r::CabinMode::ChaseNear: return fh6r::CabinMode::ChaseFar;
        case fh6r::CabinMode::ChaseFar: return fh6r::CabinMode::Cockpit;
        default: return fh6r::CabinMode::Dashboard;
    }
}

struct Sim {
    fh6r::CameraTracker tracker;
    fh6r::CabinMode real = fh6r::CabinMode::Dashboard;
    float t = 0.0f;

    Sim() : tracker(make_cfg()) {}

    static fh6r::CameraTracker::Config make_cfg() {
        fh6r::CameraTracker::Config c;
        c.cluster_pos_m = 0.8f;   // synthetic rig is noise-free: tight clusters
        c.cluster_dir = 0.15f;
        return c;
    }

    fh6r::CameraGeometry sample() const {
        const ViewGeo& g = geo_for(real);
        fh6r::CameraGeometry out;
        out.pos[0] = kSpeed * t + g.off[0];
        out.pos[1] = g.off[1];
        out.pos[2] = g.off[2];
        for (int k = 0; k < 3; ++k) { out.fwd[k] = g.fwd[k]; out.up[k] = g.up[k]; }
        out.valid = true;
        return out;
    }

    void drive(float seconds) {
        const int ticks = static_cast<int>(seconds / kDt);
        for (int i = 0; i < ticks; ++i) {
            tracker.update(sample(), kDt, false);
            t += kDt;
        }
    }

    // The player presses the camera key: the real camera advances instantly.
    fh6r::CameraTracker::Result press() {
        real = next_in_cycle(real);
        const auto r = tracker.update(sample(), kDt, true);
        t += kDt;
        return r;
    }
};

} // namespace

int main() {
    // 1. Clean cycle from a correct Dashboard start: the first cycle runs on
    //    counting, the interior self-loop confirms on the second pass, and
    //    from then on every press matches the real view with all 5 clusters
    //    labeled.
    {
        Sim s;
        s.drive(3.0f);
        bool converged = false;
        for (int cycle = 0; cycle < 3 && !converged; ++cycle) {
            for (int step = 0; step < 6; ++step) {
                s.press();
                s.drive(1.5f);
            }
            if (s.tracker.anchored() && s.tracker.labeled_clusters() == 5) converged = true;
        }
        assert(converged);
        // Fully labeled: one more cycle must track every view exactly.
        for (int step = 0; step < 6; ++step) {
            s.press();
            assert(s.tracker.view() == s.real);
            s.drive(1.0f);
        }
    }

    // 2. Desync self-heal: starting from a wrong assumption (tracker thinks
    //    Dashboard, camera really at ChaseFar) must still converge, and once
    //    anchored every subsequent press is exact.
    {
        Sim s;
        s.real = fh6r::CabinMode::ChaseFar;
        s.drive(2.0f);
        int exact_run = 0;
        for (int i = 0; i < 24 && exact_run < 6; ++i) {
            s.press();
            s.drive(1.0f);
            if (s.tracker.view() == s.real) ++exact_run; else exact_run = 0;
        }
        assert(exact_run >= 6);
        assert(s.tracker.anchored());
    }

    // 3. Menu press safety: an edge without any geometry change on a labeled
    //    exterior cluster (game ignored the key) must not advance the view,
    //    and repeated same-visit presses must not mislabel the cluster.
    {
        Sim s;
        s.drive(3.0f);
        for (int cycle = 0; cycle < 3; ++cycle)
            for (int step = 0; step < 6; ++step) { s.press(); s.drive(1.0f); }
        assert(s.tracker.labeled_clusters() == 5);
        // At Hood with frozen geometry (game in a menu: the key did nothing),
        // fidget the camera key. The view must not advance.
        while (s.real != fh6r::CabinMode::Hood) { s.press(); s.drive(0.5f); }
        const auto before = s.tracker.view();
        for (int i = 0; i < 4; ++i) {
            const auto geo = s.sample(); // real view does NOT change
            s.tracker.update(geo, kDt, true);
            s.t += kDt;
            s.drive(0.2f);
        }
        assert(s.tracker.view() == before);
        assert(s.real == fh6r::CabinMode::Hood);
    }

    // 4. Remapped-key recovery: a switch the input poller cannot see (no
    //    edge, geometry jump only) is still tracked via the geometry anchor.
    {
        Sim s;
        s.drive(3.0f);
        for (int cycle = 0; cycle < 3; ++cycle)
            for (int step = 0; step < 6; ++step) { s.press(); s.drive(1.0f); }
        assert(s.tracker.labeled_clusters() == 5);
        // Advance the real camera without telling the input side.
        s.real = next_in_cycle(s.real);
        const auto r = s.tracker.update(s.sample(), kDt, false);
        if (s.real != fh6r::CabinMode::Dashboard) {
            // Geometry-visible switch: must snap immediately.
            assert(r.switched && r.snapped);
            assert(s.tracker.view() == s.real);
        }
    }

    // 5. Manual sync bootstraps labeling immediately (debug API path).
    {
        Sim s;
        s.real = fh6r::CabinMode::ChaseNear;
        s.drive(2.0f);
        s.tracker.set_view(fh6r::CabinMode::ChaseNear);
        // Next press goes to a fresh cluster but the labeled predecessor
        // proves it is ChaseFar.
        s.press(); // real -> ChaseFar
        assert(s.tracker.view() == fh6r::CabinMode::ChaseFar);
        s.drive(1.0f);
        s.press(); // real -> Cockpit (interior cluster)
        assert(s.tracker.view() == fh6r::CabinMode::Cockpit);
        assert(s.tracker.anchored());
    }

    std::puts("camera_tracker_test: all checks passed");
    return 0;
}
