#pragma once

#include "fh6r/cabin_dsp.hpp"

#include <cstdint>

namespace fh6r {

// One listener-geometry sample in world space. Obtained at runtime through
// FMOD's System::get3DListenerAttributes, which is resolved by anchor+pattern
// scans at startup. No hardcoded game addresses are involved in this tracker.
struct CameraGeometry {
    float pos[3]{};
    float fwd[3]{};
    float up[3]{};
    bool valid = false;
};

// Closed-loop six-view camera tracker.
//
// Root-cause fix for view/audio desync. The previous design inferred the
// camera by counting Tab/RB input edges (open loop): any missed or phantom
// edge desynchronised the audio permanently, and an unexpected starting view
// broke it from the first frame. This tracker treats the game's own listener
// geometry as ground truth and keeps input edges only as a secondary hint:
//
//  - A camera switch is detected from a geometry discontinuity (an instant
//    position/direction jump far beyond smooth driving motion) OR from an
//    input edge when geometry alone cannot see the transition.
//  - Geometry is clustered online in a motion-compensated feature space
//    (position relative to a slow EMA baseline, plus fwd/up directions).
//    FH6's six views occupy five clusters because Dashboard and Cockpit share
//    the same listener pose.
//  - The confirmed cycle Dashboard -> Hood -> Bumper -> ChaseNear -> ChaseFar
//    -> Cockpit maps to cluster transitions I->H->B->N->F->I plus a unique
//    self-loop I->I (Cockpit -> Dashboard). Observed transitions therefore
//    label clusters absolutely: a self-loop proves "interior", entering the
//    interior cluster proves Cockpit, leaving it proves Hood, and each
//    exterior->exterior hop propagates the next label along the cycle.
//  - Arriving at a labeled cluster snaps the view to that label, so any
//    residual counting error self-corrects automatically within a few
//    switches. Before labeling converges the tracker falls back to plain
//    transition counting, i.e. it is never worse than the old counter.
//
// Platform-neutral by design: the controller feeds samples in, and portable
// tests drive the same code with synthetic geometry.
class CameraTracker {
public:
    enum class EventSource : std::uint8_t { None, InputEdge, GeometryJump };

    struct Config {
        float jump_pos_m = 1.0f;        // pos jump beyond smooth motion => switch
        float jump_dir = 0.18f;         // |dfwd| or |dup| jump => switch (~10 deg)
        float cluster_pos_m = 1.6f;     // cluster tolerance, compensated position
        float cluster_dir = 0.30f;      // cluster tolerance for fwd+up combined
        float baseline_halflife_s = 2.5f;
        // Bootstrap aid: while the car is moving, the motion-compensated
        // interior pose sits near the car itself, so a tiny |rel| is strong
        // (not conclusive) evidence for the interior cluster. Meaningless
        // when parked (rel collapses for every view), hence the speed gate.
        float interior_rel_m = 0.9f;
        float min_speed_for_radius_mps = 5.0f;
    };

    struct Result {
        bool switched = false;          // view_ changed this update
        bool snapped = false;           // change came from a labeled-cluster anchor
        EventSource source = EventSource::None;
    };

    explicit CameraTracker(Config cfg = {}) noexcept;

    // Attach / vehicle change: drop learned clusters and counting state.
    void reset(CabinMode initial) noexcept;
    // Manual sync (debug API). Also teaches the tracker: the current cluster
    // inherits the synced view's label, which accelerates absolute labeling.
    void set_view(CabinMode v) noexcept;

    // Feed one tick. dt in seconds; input_edge is true on the press edge of
    // the camera key. Geometry may be invalid (pre-attach, menus).
    Result update(const CameraGeometry& g, float dt, bool input_edge) noexcept;

    CabinMode view() const noexcept { return view_; }
    bool anchored() const noexcept { return anchored_; }
    int labeled_clusters() const noexcept;
    int cluster_count() const noexcept;
    std::uint64_t events() const noexcept { return events_; }

private:
    enum class ClusterLabel : std::uint8_t { Unlabeled, Interior, Hood, Bumper, ChaseNear, ChaseFar };

    struct Cluster {
        float rel[3]{};                 // motion-compensated position centroid
        float fwd[3]{};
        float up[3]{};
        std::uint32_t samples = 0;
        ClusterLabel label = ClusterLabel::Unlabeled;
        // An edge without a geometry jump on an unlabeled cluster is either
        // the interior self-loop or an ignored (menu) press. Confirmation
        // requires strikes from at least two DISTINCT visits (a parked
        // fidget within one stay must never confirm), or the moving-car
        // radius prior.
        std::uint8_t self_loop_strikes = 0;
        std::uint32_t last_strike_visit = 0;
    };

    static constexpr int kMaxClusters = 8;

    static CabinMode next_view(CabinMode v) noexcept;
    static ClusterLabel next_label(ClusterLabel l) noexcept;
    static CabinMode exterior_label_to_view(ClusterLabel l) noexcept;

    int match_cluster(const float rel[3], const float fwd[3], const float up[3]) const noexcept;
    int create_cluster(const float rel[3], const float fwd[3], const float up[3]) noexcept;
    void label_cluster(int idx, ClusterLabel l) noexcept;
    static ClusterLabel label_for_view(CabinMode v) noexcept;
    Result transition(int from, int to, EventSource source) noexcept;
    Result counting_advance(EventSource source) noexcept;

    Config cfg_;
    Cluster clusters_[kMaxClusters]{};
    int cluster_count_ = 0;
    int current_cluster_ = -1;
    CabinMode view_ = CabinMode::Dashboard;
    bool anchored_ = false;
    bool have_baseline_ = false;
    bool have_prev_geometry_ = false;
    float baseline_[3]{};
    float prev_pos_[3]{};
    float prev_fwd_[3]{};
    float prev_up_[3]{};
    float speed_ = 0.0f;                // EMA of |dpos|/dt, gates the radius prior
    std::uint32_t visit_counter_ = 0;   // +1 per cluster arrival
    std::uint32_t current_visit_ = 0;
    std::uint64_t events_ = 0;
};

} // namespace fh6r
