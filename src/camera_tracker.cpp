#include "fh6r/camera_tracker.hpp"

#include <cmath>

namespace fh6r {
namespace {

float dist3(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

CameraTracker::CameraTracker(Config cfg) noexcept : cfg_{cfg} {}

CabinMode CameraTracker::next_view(CabinMode v) noexcept {
    // Confirmed FH6 cycle (2026-08-14): Dashboard -> Hood -> Bumper ->
    // ChaseNear -> ChaseFar -> Cockpit -> Dashboard.
    switch (v) {
        case CabinMode::Dashboard: return CabinMode::Hood;
        case CabinMode::Hood: return CabinMode::Bumper;
        case CabinMode::Bumper: return CabinMode::ChaseNear;
        case CabinMode::ChaseNear: return CabinMode::ChaseFar;
        case CabinMode::ChaseFar: return CabinMode::Cockpit;
        case CabinMode::Cockpit: return CabinMode::Dashboard;
        default: return CabinMode::Dashboard;
    }
}

CameraTracker::ClusterLabel CameraTracker::next_label(ClusterLabel l) noexcept {
    switch (l) {
        case ClusterLabel::Interior: return ClusterLabel::Hood;
        case ClusterLabel::Hood: return ClusterLabel::Bumper;
        case ClusterLabel::Bumper: return ClusterLabel::ChaseNear;
        case ClusterLabel::ChaseNear: return ClusterLabel::ChaseFar;
        default: return ClusterLabel::Unlabeled; // ChaseFar -> Interior: proven on arrival
    }
}

CabinMode CameraTracker::exterior_label_to_view(ClusterLabel l) noexcept {
    switch (l) {
        case ClusterLabel::Hood: return CabinMode::Hood;
        case ClusterLabel::Bumper: return CabinMode::Bumper;
        case ClusterLabel::ChaseNear: return CabinMode::ChaseNear;
        case ClusterLabel::ChaseFar: return CabinMode::ChaseFar;
        default: return CabinMode::Unknown;
    }
}

CameraTracker::ClusterLabel CameraTracker::label_for_view(CabinMode v) noexcept {
    switch (v) {
        case CabinMode::Dashboard:
        case CabinMode::Cockpit: return ClusterLabel::Interior;
        case CabinMode::Hood: return ClusterLabel::Hood;
        case CabinMode::Bumper: return ClusterLabel::Bumper;
        case CabinMode::ChaseNear: return ClusterLabel::ChaseNear;
        case CabinMode::ChaseFar: return ClusterLabel::ChaseFar;
        default: return ClusterLabel::Unlabeled;
    }
}

void CameraTracker::reset(CabinMode initial) noexcept {
    cluster_count_ = 0;
    current_cluster_ = -1;
    view_ = (initial == CabinMode::Unknown) ? CabinMode::Dashboard : initial;
    anchored_ = false;
    have_baseline_ = false;
    have_prev_geometry_ = false;
    speed_ = 0.0f;
    events_ = 0;
    for (auto& c : clusters_) c = Cluster{};
}

void CameraTracker::set_view(CabinMode v) noexcept {
    if (v == CabinMode::Unknown) return;
    view_ = v;
    anchored_ = true;
    // Teach the current geometry cluster the synced view's label; a single
    // manual sync therefore bootstraps absolute labeling immediately.
    if (current_cluster_ >= 0) label_cluster(current_cluster_, label_for_view(v));
}

int CameraTracker::labeled_clusters() const noexcept {
    int n = 0;
    for (int i = 0; i < cluster_count_; ++i)
        if (clusters_[i].label != ClusterLabel::Unlabeled) ++n;
    return n;
}

int CameraTracker::cluster_count() const noexcept { return cluster_count_; }

void CameraTracker::label_cluster(int idx, ClusterLabel l) noexcept {
    if (idx < 0 || idx >= cluster_count_) return;
    if (clusters_[idx].label == ClusterLabel::Unlabeled) clusters_[idx].label = l;
}

int CameraTracker::match_cluster(const float rel[3], const float fwd[3], const float up[3]) const noexcept {
    int best = -1;
    float best_cost = 0.0f;
    for (int i = 0; i < cluster_count_; ++i) {
        const float dp = dist3(rel, clusters_[i].rel);
        if (dp > cfg_.cluster_pos_m) continue;
        const float dd = dist3(fwd, clusters_[i].fwd) + dist3(up, clusters_[i].up);
        if (dd > cfg_.cluster_dir) continue;
        const float cost = dp / cfg_.cluster_pos_m + dd / cfg_.cluster_dir;
        if (best < 0 || cost < best_cost) { best = i; best_cost = cost; }
    }
    return best;
}

int CameraTracker::create_cluster(const float rel[3], const float fwd[3], const float up[3]) noexcept {
    int idx = cluster_count_;
    if (idx >= kMaxClusters) {
        // Recycle the least-observed cluster; a full table means geometry was
        // noisier than expected and the stalest entry is the safest to drop.
        idx = 0;
        for (int i = 1; i < kMaxClusters; ++i)
            if (clusters_[i].samples < clusters_[idx].samples) idx = i;
        clusters_[idx] = Cluster{};
    } else {
        ++cluster_count_;
    }
    Cluster& c = clusters_[idx];
    for (int k = 0; k < 3; ++k) { c.rel[k] = rel[k]; c.fwd[k] = fwd[k]; c.up[k] = up[k]; }
    c.samples = 1;
    return idx;
}

CameraTracker::Result CameraTracker::counting_advance(EventSource source) noexcept {
    // Pre-labeling fallback: identical semantics to the old open-loop counter.
    const CabinMode next = next_view(view_);
    Result r;
    r.source = source;
    r.switched = (next != view_);
    view_ = next;
    ++events_;
    return r;
}

CameraTracker::Result CameraTracker::transition(int from, int to, EventSource source) noexcept {
    ++events_;
    Result r;
    r.source = source;

    ClusterLabel from_label = (from >= 0) ? clusters_[from].label : ClusterLabel::Unlabeled;
    ClusterLabel to_label = (to >= 0) ? clusters_[to].label : ClusterLabel::Unlabeled;

    // Contradiction defense: a mislabeled cluster must not poison the graph
    // permanently. The only valid predecessor of the interior cluster is
    // ChaseFar (or itself via the self-loop); the only valid self-loop is the
    // interior pair. Violations demote the label and fall back to counting.
    if (from != to && to_label == ClusterLabel::Interior &&
        from_label != ClusterLabel::Unlabeled && from_label != ClusterLabel::ChaseFar) {
        clusters_[to].label = ClusterLabel::Unlabeled;
        to_label = ClusterLabel::Unlabeled;
    }
    if (from == to && to >= 0 && to_label != ClusterLabel::Unlabeled &&
        to_label != ClusterLabel::Interior) {
        clusters_[to].label = ClusterLabel::Unlabeled;
        to_label = ClusterLabel::Unlabeled;
    }

    CabinMode new_view = CabinMode::Unknown;
    bool anchored_decision = false;

    if (from == to && to >= 0) {
        // Same-cluster switch: the only same-geometry adjacent pair in the
        // cycle is Cockpit -> Dashboard, so the cluster is the interior one
        // and we have just landed on Dashboard.
        label_cluster(to, ClusterLabel::Interior);
        new_view = CabinMode::Dashboard;
        anchored_decision = true;
    } else if (to_label == ClusterLabel::Interior) {
        // Exterior -> interior is uniquely ChaseFar -> Cockpit, which also
        // proves the origin cluster was ChaseFar.
        label_cluster(from, ClusterLabel::ChaseFar);
        new_view = CabinMode::Cockpit;
        anchored_decision = true;
    } else if (to_label != ClusterLabel::Unlabeled) {
        // Arriving at a labeled exterior cluster: snap to the absolute view
        // and back-propagate the predecessor label along the cycle.
        new_view = exterior_label_to_view(to_label);
        if (from_label == ClusterLabel::Unlabeled) {
            if (to_label == ClusterLabel::Hood) label_cluster(from, ClusterLabel::Interior);
            else if (to_label == ClusterLabel::Bumper) label_cluster(from, ClusterLabel::Hood);
            else if (to_label == ClusterLabel::ChaseNear) label_cluster(from, ClusterLabel::Bumper);
            else if (to_label == ClusterLabel::ChaseFar) label_cluster(from, ClusterLabel::ChaseNear);
        }
        anchored_decision = true;
    } else if (from_label != ClusterLabel::Unlabeled) {
        // Leaving a labeled cluster into fresh geometry: the destination is
        // the next label along the cycle.
        const ClusterLabel nl = next_label(from_label);
        if (nl != ClusterLabel::Unlabeled) {
            label_cluster(to, nl);
            new_view = exterior_label_to_view(nl);
            anchored_decision = true;
        }
    }

    if (new_view == CabinMode::Unknown) {
        // Nothing proven yet: fall back to relative counting.
        const CabinMode next = next_view(view_);
        r.switched = (next != view_);
        view_ = next;
    } else {
        r.switched = (new_view != view_);
        r.snapped = anchored_decision;
        view_ = new_view;
        anchored_ = true;
    }
    current_cluster_ = to;
    ++visit_counter_;
    current_visit_ = visit_counter_;
    return r;
}

CameraTracker::Result CameraTracker::update(const CameraGeometry& g, float dt, bool input_edge) noexcept {
    if (!g.valid) {
        // No geometry ground truth (pre-attach, menus): edges degrade to the
        // legacy counter so behaviour is never worse than the old build.
        have_prev_geometry_ = false;
        if (input_edge) return counting_advance(EventSource::InputEdge);
        return {};
    }

    if (!have_baseline_) {
        have_baseline_ = true;
        for (int k = 0; k < 3; ++k) baseline_[k] = g.pos[k];
    }

    // Motion-compensated position: the slow baseline follows the car, so rel
    // converges to the camera's offset from the vehicle itself.
    float rel[3];
    for (int k = 0; k < 3; ++k) rel[k] = g.pos[k] - baseline_[k];

    bool jump = false;
    if (have_prev_geometry_) {
        const float dp = dist3(g.pos, prev_pos_);
        const float df = dist3(g.fwd, prev_fwd_);
        const float du = dist3(g.up, prev_up_);
        jump = (dp > cfg_.jump_pos_m) || (df > cfg_.jump_dir) || (du > cfg_.jump_dir);
        if (dt > 0.0f) {
            const float v = dp / dt;
            speed_ += 0.05f * (v - speed_);
        }
    }
    for (int k = 0; k < 3; ++k) {
        prev_pos_[k] = g.pos[k];
        prev_fwd_[k] = g.fwd[k];
        prev_up_[k] = g.up[k];
    }
    have_prev_geometry_ = true;

    if (jump) {
        // A real switch happened regardless of which physical control caused
        // it (including remapped keys the input poller cannot see).
        int to = match_cluster(rel, g.fwd, g.up);
        if (to < 0) to = create_cluster(rel, g.fwd, g.up);
        const int from = current_cluster_;
        if (to == from) {
            // Jumped but landed back in the same cluster: camera shake or a
            // collision bump. A same-cluster switch is only legitimate for
            // the interior pair, and that pair never produces a jump. Ignore.
            return {};
        }
        return transition(from, to, EventSource::GeometryJump);
    }

    if (input_edge) {
        // A press without a geometry jump. Either the interior self-loop
        // (Cockpit -> Dashboard shares one listener pose) or the game ignored
        // the press (menus, pause). Advance only with evidence:
        //  - cluster already labeled Interior: certain self-loop;
        //  - moving car + tiny compensated radius, or a repeated no-jump edge
        //    on this cluster: confirms the bootstrap hypothesis;
        //  - otherwise record a strike and hold the view (menu-press safe).
        if (current_cluster_ >= 0) {
            Cluster& c = clusters_[current_cluster_];
            if (c.label == ClusterLabel::Interior)
                return transition(current_cluster_, current_cluster_, EventSource::InputEdge);
            const float abs_radius = std::sqrt(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
            const bool radius_ok = speed_ > cfg_.min_speed_for_radius_mps &&
                                   abs_radius < cfg_.interior_rel_m;
            const bool repeated_visit = c.self_loop_strikes >= 1 &&
                                        c.last_strike_visit != current_visit_;
            if (radius_ok || repeated_visit) {
                label_cluster(current_cluster_, ClusterLabel::Interior);
                return transition(current_cluster_, current_cluster_, EventSource::InputEdge);
            }
            ++c.self_loop_strikes;
            c.last_strike_visit = current_visit_;
            return {};
        }
        return counting_advance(EventSource::InputEdge);
    }

    // Steady state: track the car with the baseline and refine the centroid.
    const float halflife = cfg_.baseline_halflife_s > 0.0f ? cfg_.baseline_halflife_s : 2.5f;
    const float alpha = 1.0f - std::exp(-0.69314718056f * dt / halflife);
    for (int k = 0; k < 3; ++k) baseline_[k] += alpha * (g.pos[k] - baseline_[k]);
    if (current_cluster_ >= 0) {
        Cluster& c = clusters_[current_cluster_];
        const float ca = 1.0f / static_cast<float>(c.samples + 1);
        for (int k = 0; k < 3; ++k) {
            c.rel[k] += ca * (rel[k] - c.rel[k]);
            c.fwd[k] += ca * (g.fwd[k] - c.fwd[k]);
            c.up[k] += ca * (g.up[k] - c.up[k]);
        }
        ++c.samples;
    } else {
        current_cluster_ = match_cluster(rel, g.fwd, g.up);
        if (current_cluster_ < 0) current_cluster_ = create_cluster(rel, g.fwd, g.up);
        // First geometry contact: if this cluster was labeled earlier in the
        // session, snap immediately (re-attach recovery).
        const ClusterLabel l = clusters_[current_cluster_].label;
        if (l == ClusterLabel::Interior) {
            // The interior pose alone cannot separate Dashboard from Cockpit;
            // keep the current view but count as anchored.
            anchored_ = true;
        } else if (l != ClusterLabel::Unlabeled) {
            const CabinMode v = exterior_label_to_view(l);
            anchored_ = true;
            if (v != CabinMode::Unknown && v != view_) {
                view_ = v;
                Result r;
                r.switched = true;
                r.snapped = true;
                r.source = EventSource::GeometryJump;
                return r;
            }
        }
    }
    return {};
}

} // namespace fh6r
