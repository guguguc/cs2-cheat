#include "bvh.h"

#include "cfg.h"

#include <algorithm>
#include <cstdio>

namespace {

constexpr int kMaxLeafCount = 8;
constexpr float kEpsilon = 1e-6f;

struct UtlVector {  // count(i32), pad, data(u64)
    std::int32_t count = 0;
    std::int32_t _pad = 0;
    std::uint64_t data = 0;
};

struct OuterNode {  // deadlocked layout: pad[12], left@12, pad[12], right@28, pad[8], shape@0x28
    std::uint8_t pad1[12];
    std::int32_t left;
    std::uint8_t pad2[12];
    std::int32_t right;
    std::uint8_t pad3[8];
    std::uint64_t shape;
};

struct HalfEdge { std::uint8_t next, twin, origin, face; };

// Möller–Trumbore; returns t>0 on hit.
bool ray_triangle(const Vector3& origin, const Vector3& dir,
                  const bvh::Triangle& tri, float& t) {
    const Vector3 edge1 = tri.v1 - tri.v0;
    const Vector3 edge2 = tri.v2 - tri.v0;
    const Vector3 h = {dir.y * edge2.z - dir.z * edge2.y,
                       dir.z * edge2.x - dir.x * edge2.z,
                       dir.x * edge2.y - dir.y * edge2.x};
    const float a = edge1.x * h.x + edge1.y * h.y + edge1.z * h.z;
    if (a > -kEpsilon && a < kEpsilon) return false;
    const float f = 1.0f / a;
    const Vector3 s = origin - tri.v0;
    const float u = f * (s.x * h.x + s.y * h.y + s.z * h.z);
    if (u < 0.0f || u > 1.0f) return false;
    const Vector3 q = {s.y * edge1.z - s.z * edge1.y,
                       s.z * edge1.x - s.x * edge1.z,
                       s.x * edge1.y - s.y * edge1.x};
    const float v = f * (dir.x * q.x + dir.y * q.y + dir.z * q.z);
    if (v < 0.0f || u + v > 1.0f) return false;
    const float tt = f * (edge2.x * q.x + edge2.y * q.y + edge2.z * q.z);
    if (tt <= kEpsilon) return false;
    t = tt;
    return true;
}

std::string rtti_name(const Memory& mem, std::uint64_t vtable) {
    const auto vt = mem.read<std::uint64_t>(vtable).value_or(0);
    if (!vt) return {};
    const auto rtti = mem.read<std::uint64_t>(vt - 8).value_or(0);
    if (!rtti) return {};
    const auto name_ptr = mem.read<std::uint64_t>(rtti + 8).value_or(0);
    if (!name_ptr) return {};
    char buf[64] = {0};
    mem.read(name_ptr, buf, sizeof(buf) - 1);
    return std::string(buf);
}

bool process_mesh(const Memory& mem, std::uint64_t shape, std::vector<bvh::Triangle>& out) {
    const auto mesh_data = mem.read<std::uint64_t>(shape + 0xC0).value_or(0);
    if (!mesh_data) return false;
    const auto mats = mem.read<UtlVector>(mesh_data + 144);
    if (!mats || mats->count == 0) return false;  // skip collision-only meshes (deadlocked)
    const auto vertices = mem.read<UtlVector>(mesh_data + 48);
    const auto tris = mem.read<UtlVector>(mesh_data + 72);
    if (!vertices || !tris || vertices->count <= 0 || tris->count <= 0 ||
        !vertices->data || !tris->data) return false;
    struct Tri { std::int32_t idx[3]; };
    std::vector<Vector3> v(vertices->count);
    for (int i = 0; i < vertices->count; ++i) {
        const auto p = mem.read<Vector3>(vertices->data + static_cast<std::uint64_t>(i) * 12);
        if (!p) return false;
        v[i] = *p;
    }
    for (int i = 0; i < tris->count; ++i) {
        const auto t = mem.read<Tri>(tris->data + static_cast<std::uint64_t>(i) * 12);
        if (!t) return false;
        if (t->idx[0] < 0 || t->idx[1] < 0 || t->idx[2] < 0 ||
            t->idx[0] >= vertices->count || t->idx[1] >= vertices->count ||
            t->idx[2] >= vertices->count)
            continue;
        out.push_back({v[t->idx[0]], v[t->idx[1]], v[t->idx[2]]});
    }
    return !out.empty();
}

bool process_hull(const Memory& mem, std::uint64_t shape, std::vector<bvh::Triangle>& out) {
    const auto data = mem.read<std::uint64_t>(shape + 0xB8).value_or(0);
    if (!data) return false;
    const float scale = mem.read<float>(shape + 0xB0).value_or(1.0f);
    const auto vertices = mem.read<UtlVector>(data + 112);
    const auto edges = mem.read<UtlVector>(data + 200);
    const auto faces = mem.read<UtlVector>(data + 224);
    if (!vertices || !edges || !faces ||
        vertices->count <= 0 || edges->count <= 0 || faces->count <= 0) return false;
    std::vector<Vector3> v(vertices->count);
    for (int i = 0; i < vertices->count; ++i) {
        const auto p = mem.read<Vector3>(vertices->data + static_cast<std::uint64_t>(i) * 12);
        if (!p) return false;
        v[i] = *p * scale;
    }
    std::vector<HalfEdge> e(edges->count);
    for (int i = 0; i < edges->count; ++i) {
        const auto h = mem.read<HalfEdge>(edges->data + static_cast<std::uint64_t>(i));
        if (!h) return false;
        e[i] = *h;
    }
    for (int i = 0; i < faces->count; ++i) {
        const auto start = mem.read<std::uint8_t>(faces->data + static_cast<std::uint64_t>(i));
        if (!start) return false;
        std::vector<Vector3> face_verts;
        std::uint8_t cur = *start;
        int guard = 0;
        while (guard++ < 1024) {
            if (cur >= edges->count) break;
            const auto& edge = e[cur];
            if (edge.origin >= vertices->count) break;
            face_verts.push_back(v[edge.origin]);
            cur = edge.next;
            if (cur == *start) break;
        }
        if (face_verts.size() >= 3) {
            for (std::size_t k = 1; k + 1 < face_verts.size(); ++k)
                out.push_back({face_verts[0], face_verts[k], face_verts[k + 1]});
        }
    }
    return true;
}

}  // namespace

namespace bvh {

bool read_map(const Memory& mem, std::uintptr_t vphys_world,
              std::vector<Triangle>& out_triangles) {
    out_triangles.clear();
    if (!vphys_world) return false;
    // deadlocked chain:
    //   vphys_world_global  = read(get_relative_address(insn,3,7))   (slot content)
    //   wld                 = read(vphys_world_global)                 (world ptr)
    //   inner               = read(wld + 0x30)
    // Our abs4 pattern stores the slot ADDRESS (get_relative_address), so we
    // dereference twice here to match.
    const auto slot_content = mem.read<std::uintptr_t>(vphys_world).value_or(0);
    if (!slot_content) return false;
    const auto wld = mem.read<std::uintptr_t>(slot_content).value_or(0);
    if (!wld) return false;
    const auto inner = mem.read<std::uintptr_t>(wld + 0x30).value_or(0);
    if (!inner) return false;
    const auto bods = mem.read<std::uintptr_t>(inner + 0x118).value_or(0);
    if (!bods) return false;
    const auto bdcnt = mem.read<std::int32_t>(bods + 0x268).value_or(0);
    if (bdcnt <= 0 || bdcnt > 4096) return false;

    for (int idx = 0; idx < bdcnt; ++idx) {
        const std::uintptr_t bod = bods + static_cast<std::uintptr_t>(idx) * 88;
        const auto bdty = mem.read<std::uint32_t>(bod + 0x40).value_or(0);
        if (bdty != 2) continue;  // only static geometry
        const auto rt = mem.read<std::int32_t>(bod).value_or(-1);
        const auto ndptr = mem.read<std::uintptr_t>(bod + 0x18).value_or(0);
        const auto cnt = mem.read<std::int32_t>(bod + 0x08).value_or(0);
        if (rt < 0 || !ndptr || cnt <= 0 || cnt > 1 << 20) {
            continue;
        }

        std::vector<OuterNode> outer(cnt);
        if (!mem.read(ndptr, outer.data(), sizeof(OuterNode) * static_cast<std::size_t>(cnt)))
            continue;

        // collect leaf shapes
        std::vector<std::uint64_t> leaves;
        std::vector<std::int32_t> stack;
        stack.push_back(rt);
        int guard = 0;
        while (!stack.empty() && guard++ < 1 << 20) {
            const std::int32_t index = stack.back();
            stack.pop_back();
            if (index < 0 || index >= cnt) continue;
            const auto& node = outer[static_cast<std::size_t>(index)];
            if (node.left == -1 && node.right == -1) leaves.push_back(node.shape);
            if (node.left != -1) stack.push_back(node.left);
            if (node.right != -1) stack.push_back(node.right);
        }

        for (const auto shape : leaves) {
            const std::string name = rtti_name(mem, shape);
            if (name == "12CRnMeshShape") process_mesh(mem, shape, out_triangles);
            else if (name == "12CRnHullShape") process_hull(mem, shape, out_triangles);
        }
    }
    return !out_triangles.empty();
}

bool MapBvh::load(const Memory& mem, std::uintptr_t vphys_world) {
    nodes_.clear();
    root_ = -1;
    if (!read_map(mem, vphys_world, triangles_)) return false;
    if (triangles_.empty()) return false;
    std::vector<int> prims(triangles_.size());
    for (std::size_t i = 0; i < prims.size(); ++i) prims[i] = static_cast<int>(i);
    root_ = build_recursive(prims);
    return root_ >= 0;
}

int MapBvh::build_recursive(std::vector<int>& prims) {
    if (prims.size() <= static_cast<std::size_t>(kMaxLeafCount)) {
        Aabb aabb{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        for (const int idx : prims) aabb.expand(triangles_[static_cast<std::size_t>(idx)].v0);
        for (const int idx : prims) aabb.expand(triangles_[static_cast<std::size_t>(idx)].v1);
        for (const int idx : prims) aabb.expand(triangles_[static_cast<std::size_t>(idx)].v2);
        Node n;
        n.aabb = aabb;
        n.left = n.right = -1;
        n.prims = prims;
        nodes_.push_back(std::move(n));
        return static_cast<int>(nodes_.size()) - 1;
    }
    Aabb cb{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
    for (const int idx : prims) cb.expand(triangles_[static_cast<std::size_t>(idx)].centroid());
    const Vector3 extent = cb.max - cb.min;
    const int axis = (extent.x >= extent.y && extent.x >= extent.z) ? 0
                   : (extent.y >= extent.z) ? 1 : 2;
    std::sort(prims.begin(), prims.end(), [&](int a, int b) {
        return triangles_[static_cast<std::size_t>(a)].centroid()[axis] <
               triangles_[static_cast<std::size_t>(b)].centroid()[axis];
    });
    const std::size_t mid = prims.size() / 2;
    std::vector<int> left(prims.begin(), prims.begin() + static_cast<long>(mid));
    std::vector<int> right(prims.begin() + static_cast<long>(mid), prims.end());
    const int l = build_recursive(left);
    const int r = build_recursive(right);
    Node n;
    n.aabb = nodes_[static_cast<std::size_t>(l)].aabb.merged(nodes_[static_cast<std::size_t>(r)].aabb);
    n.left = l;
    n.right = r;
    nodes_.push_back(std::move(n));
    return static_cast<int>(nodes_.size()) - 1;
}

bool MapBvh::has_line_of_sight(const Vector3& start, const Vector3& end) const {
    if (root_ < 0) return true;
    Vector3 dir = end - start;
    const float dist = dir.Length();
    if (dist < 0.0001f) return true;
    dir = dir / dist;
    // inv_dir: guard against zero components.
    Vector3 inv_dir;
    inv_dir.x = (dir.x != 0.0f) ? 1.0f / dir.x : 1e30f;
    inv_dir.y = (dir.y != 0.0f) ? 1.0f / dir.y : 1e30f;
    inv_dir.z = (dir.z != 0.0f) ? 1.0f / dir.z : 1e30f;

    // iterative DFS to avoid deep recursion on huge maps
    std::vector<int> stack;
    stack.push_back(root_);
    while (!stack.empty()) {
        const int idx = stack.back();
        stack.pop_back();
        const Node& node = nodes_[static_cast<std::size_t>(idx)];
        if (!node.aabb.ray_intersect(start, inv_dir, dist)) continue;
        if (node.left < 0) {  // leaf
            for (const int ti : node.prims) {
                float t = 0.f;
                if (ray_triangle(start, dir, triangles_[static_cast<std::size_t>(ti)], t) &&
                    t >= 0.0f && t <= dist)
                    return false;  // blocked
            }
        } else {
            stack.push_back(node.left);
            stack.push_back(node.right);
        }
    }
    return true;
}

}  // namespace bvh
