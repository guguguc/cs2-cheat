#pragma once

#include "math.h"
#include "memory.h"

#include <cstdint>
#include <vector>

// BVH-based line-of-sight visibility (ported from deadlocked parser/bvh.rs).
// Reads the map's physics collision triangles from the running game
// (IVPhysicsWorld -> bodies -> BVH nodes -> mesh/hull shapes), builds an
// AABB tree, then answers "is the segment eye->bone clear?" with a ray cast.
namespace bvh {

// One physics collision triangle in world space (Source units).
struct Triangle {
    Vector3 v0, v1, v2;
    Vector3 centroid() const { return (v0 + v1 + v2) * (1.0f / 3.0f); }
};

struct Aabb {
    Vector3 min, max;
    Vector3 centroid() const { return (min + max) * 0.5f; }
    void expand(const Vector3& p) {
        if (p.x < min.x) min.x = p.x;
        if (p.y < min.y) min.y = p.y;
        if (p.z < min.z) min.z = p.z;
        if (p.x > max.x) max.x = p.x;
        if (p.y > max.y) max.y = p.y;
        if (p.z > max.z) max.z = p.z;
    }
    Aabb merged(const Aabb& o) const {
        return {{min.x < o.min.x ? min.x : o.min.x,
                 min.y < o.min.y ? min.y : o.min.y,
                 min.z < o.min.z ? min.z : o.min.z},
                {max.x > o.max.x ? max.x : o.max.x,
                 max.y > o.max.y ? max.y : o.max.y,
                 max.z > o.max.z ? max.z : o.max.z}};
    }
    // slab test; inv_dir = 1/dir (ray must not have zero components).
    bool ray_intersect(const Vector3& origin, const Vector3& inv_dir, float max_t) const {
        const Vector3 t1 = (min - origin) * inv_dir;
        const Vector3 t2 = (max - origin) * inv_dir;
        const float tmin_x = t1.x < t2.x ? t1.x : t2.x;
        const float tmin_y = t1.y < t2.y ? t1.y : t2.y;
        const float tmin_z = t1.z < t2.z ? t1.z : t2.z;
        const float tmax_x = t1.x > t2.x ? t1.x : t2.x;
        const float tmax_y = t1.y > t2.y ? t1.y : t2.y;
        const float tmax_z = t1.z > t2.z ? t1.z : t2.z;
        const float t_min = tmin_x > tmin_y ? (tmin_x > tmin_z ? tmin_x : tmin_z)
                                            : (tmin_y > tmin_z ? tmin_y : tmin_z);
        const float t_max = tmax_x < tmax_y ? (tmax_x < tmax_z ? tmax_x : tmax_z)
                                            : (tmax_y < tmax_z ? tmax_y : tmax_z);
        return t_min <= t_max && t_min <= max_t && t_max >= 0.0f;
    }
};

// Reads all collision triangles for the current map. Returns false when the
// physics world / bodies are unavailable (menu, loading, or a new vphys
// layout) - callers then fall back to spotted-mask visibility.
bool read_map(const Memory& mem, std::uintptr_t vphys_world,
              std::vector<Triangle>& out_triangles);

// Builds the acceleration structure. Cheap enough to run once per map.
class MapBvh {
public:
    // Loads + builds for the given physics world pointer. Returns false if no
    // triangles could be read (caller falls back to spotted mask).
    bool load(const Memory& mem, std::uintptr_t vphys_world);

    // True when the segment start->end does not hit any collision triangle.
    bool has_line_of_sight(const Vector3& start, const Vector3& end) const;

    std::size_t triangle_count() const { return triangles_.size(); }

private:
    std::vector<Triangle> triangles_;
    struct Node {
        Aabb aabb;
        int left = -1;         // >=0: branch children, <0: leaf
        int right = -1;
        std::vector<int> prims;  // leaf only
    };
    std::vector<Node> nodes_;
    int root_ = -1;

    int build_recursive(std::vector<int>& prims);
};

}  // namespace bvh
