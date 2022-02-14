/*
    Copyright (C) 1996-1997  Id Software, Inc.
    Copyright (C) 1997       Greg Lewis

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

    See file, 'COPYING', for details.
*/

#include <climits>

#include <qbsp/qbsp.hh>

#include <list>
#include <atomic>

#include "tbb/task_group.h"

std::atomic<int> splitnodes;

static std::atomic<int> leaffaces;
static std::atomic<int> nodefaces;
static std::atomic<int> c_solid, c_empty, c_water, c_detail, c_detail_illusionary, c_detail_fence;
static std::atomic<int> c_illusionary_visblocker;
static bool usemidsplit;

/**
 * Total number of brushes in the map
 */
static int mapbrushes;

//============================================================================

void ConvertNodeToLeaf(node_t *node, const contentflags_t &contents)
{
    // backup the mins/maxs
    aabb3d bounds = node->bounds;

    // zero it
    memset(node, 0, sizeof(*node));

    // restore relevant fields
    node->bounds = bounds;

    node->planenum = PLANENUM_LEAF;
    node->contents = contents;

    Q_assert(node->markfaces.empty());
}

void DetailToSolid(node_t *node)
{
    if (node->planenum == PLANENUM_LEAF) {
        if (options.target_game->id == GAME_QUAKE_II) {
            return;
        }

        // We need to remap CONTENTS_DETAIL to a standard quake content type
        if (node->contents.is_detail(CFLAGS_DETAIL)) {
            node->contents = options.target_game->create_solid_contents();
        } else if (node->contents.is_detail(CFLAGS_DETAIL_ILLUSIONARY)) {
            node->contents = options.target_game->create_empty_contents();
        }
        /* N.B.: CONTENTS_DETAIL_FENCE is not remapped to CONTENTS_SOLID until the very last moment,
         * because we want to generate a leaf (if we set it to CONTENTS_SOLID now it would use leaf 0).
         */
        return;
    } else {
        DetailToSolid(node->children[0]);
        DetailToSolid(node->children[1]);

        // If both children are solid, we can merge the two leafs into one.
        // DarkPlaces has an assertion that fails if both children are
        // solid.
        if (node->children[0]->contents.is_solid(options.target_game) &&
            node->children[1]->contents.is_solid(options.target_game)) {
            // This discards any faces on-node. Should be safe (?)
            ConvertNodeToLeaf(node, options.target_game->create_solid_contents());
        }
    }
}

/*
==================
FaceSide

For BSP hueristic
==================
*/
static int FaceSide__(const face_t *in, const qbsp_plane_t &split)
{
    bool have_front, have_back;
    int i;

    have_front = have_back = false;

    if (split.type < 3) {
        /* shortcut for axial planes */
        const vec_t *p = &in->w[0][split.type];
        for (i = 0; i < in->w.size(); i++, p += 3) {
            if (*p > split.dist + ON_EPSILON) {
                if (have_back)
                    return SIDE_ON;
                have_front = true;
            } else if (*p < split.dist - ON_EPSILON) {
                if (have_front)
                    return SIDE_ON;
                have_back = true;
            }
        }
    } else {
        /* sloping planes take longer */
        for (i = 0; i < in->w.size(); i++) {
            const vec_t dot = split.distance_to(in->w[i]);
            if (dot > ON_EPSILON) {
                if (have_back)
                    return SIDE_ON;
                have_front = true;
            } else if (dot < -ON_EPSILON) {
                if (have_front)
                    return SIDE_ON;
                have_back = true;
            }
        }
    }

    if (!have_front)
        return SIDE_BACK;
    if (!have_back)
        return SIDE_FRONT;

    return SIDE_ON;
}

inline int FaceSide(const face_t *in, const qbsp_plane_t &split)
{
    vec_t dist = split.distance_to(in->origin);

    if (dist > in->radius)
        return SIDE_FRONT;
    else if (dist < -in->radius)
        return SIDE_BACK;
    else
        return FaceSide__(in, split);
}

/*
 * Split a bounding box by a plane; The front and back bounds returned
 * are such that they completely contain the portion of the input box
 * on that side of the plane. Therefore, if the split plane is
 * non-axial, then the returned bounds will overlap.
 */
static void DivideBounds(const aabb3d &in_bounds, const qbsp_plane_t &split, aabb3d &front_bounds, aabb3d &back_bounds)
{
    int a, b, c, i, j;
    vec_t dist1, dist2, mid, split_mins, split_maxs;
    qvec3d corner;

    front_bounds = back_bounds = in_bounds;

    if (split.type < 3) {
        front_bounds[0][split.type] = back_bounds[1][split.type] = split.dist;
        return;
    }

    /* Make proper sloping cuts... */
    for (a = 0; a < 3; ++a) {
        /* Check for parallel case... no intersection */
        if (fabs(split.normal[a]) < NORMAL_EPSILON)
            continue;

        b = (a + 1) % 3;
        c = (a + 2) % 3;

        split_mins = in_bounds.maxs()[a];
        split_maxs = in_bounds.mins()[a];
        for (i = 0; i < 2; ++i) {
            corner[b] = in_bounds[i][b];
            for (j = 0; j < 2; ++j) {
                corner[c] = in_bounds[j][c];

                corner[a] = in_bounds[0][a];
                dist1 = split.distance_to(corner);

                corner[a] = in_bounds[1][a];
                dist2 = split.distance_to(corner);

                mid = in_bounds[1][a] - in_bounds[0][a];
                mid *= (dist1 / (dist1 - dist2));
                mid += in_bounds[0][a];

                split_mins = max(min(mid, split_mins), in_bounds.mins()[a]);
                split_maxs = min(max(mid, split_maxs), in_bounds.maxs()[a]);
            }
        }
        if (split.normal[a] > 0) {
            front_bounds[0][a] = split_mins;
            back_bounds[1][a] = split_maxs;
        } else {
            back_bounds[0][a] = split_mins;
            front_bounds[1][a] = split_maxs;
        }
    }
}

/*
 * Calculate the split plane metric for axial planes
 */
inline vec_t SplitPlaneMetric_Axial(const qbsp_plane_t &p, const aabb3d &bounds)
{
    vec_t value = 0;
    for (int i = 0; i < 3; i++) {
        if (i == p.type) {
            const vec_t dist = p.dist * p.normal[i];
            value += (bounds.maxs()[i] - dist) * (bounds.maxs()[i] - dist);
            value += (dist - bounds.mins()[i]) * (dist - bounds.mins()[i]);
        } else {
            value += 2 * (bounds.maxs()[i] - bounds.mins()[i]) * (bounds.maxs()[i] - bounds.mins()[i]);
        }
    }

    return value;
}

/*
 * Calculate the split plane metric for non-axial planes
 */
inline vec_t SplitPlaneMetric_NonAxial(const qbsp_plane_t &p, const aabb3d &bounds)
{
    aabb3d f, b;
    vec_t value = 0.0;

    DivideBounds(bounds, p, f, b);
    for (int i = 0; i < 3; i++) {
        value += (f.maxs()[i] - f.mins()[i]) * (f.maxs()[i] - f.mins()[i]);
        value += (b.maxs()[i] - b.mins()[i]) * (b.maxs()[i] - b.mins()[i]);
    }

    return value;
}

inline vec_t SplitPlaneMetric(const qbsp_plane_t &p, const aabb3d &bounds)
{
    if (p.type < 3)
        return SplitPlaneMetric_Axial(p, bounds);
    else
        return SplitPlaneMetric_NonAxial(p, bounds);
}

/*
==================
ChooseMidPlaneFromList

The clipping hull BSP doesn't worry about avoiding splits
==================
*/
static face_t *ChooseMidPlaneFromList(std::vector<brush_t> &brushes, const aabb3d &bounds)
{
    /* pick the plane that splits the least */
    vec_t bestaxialmetric = VECT_MAX;
    face_t *bestaxialsurface = nullptr;
    vec_t bestanymetric = VECT_MAX;
    face_t *bestanysurface = nullptr;

    for (int pass = 0; pass < 2; pass++) {
        for (auto &brush : brushes) {
            if (brush.contents.is_detail() != (pass == 1)) {
                continue;
            }

            for (auto &face : brush.faces) {
                if (face.onnode)
                    continue;

                const qbsp_plane_t &plane = map.planes[face.planenum];
                bool axial = false;

                /* check for axis aligned surfaces */
                if (plane.type < 3) {
                    axial = true;
                }

                /* calculate the split metric, smaller values are better */
                const vec_t metric = SplitPlaneMetric(plane, bounds);

                if (metric < bestanymetric) {
                    bestanymetric = metric;
                    bestanysurface = &face;
                }

                if (axial) {
                    if (metric < bestaxialmetric) {
                        bestaxialmetric = metric;
                        bestaxialsurface = &face;
                    }
                }
            }
        }

        if (bestanysurface != nullptr || bestaxialsurface != nullptr) {
            break;
        }
    }

    // prefer the axial split
    auto bestsurface = (bestaxialsurface == nullptr) ? bestanysurface : bestaxialsurface;
    return bestsurface;
}

/*
==================
ChoosePlaneFromList

The real BSP heuristic
==================
*/
static face_t *ChoosePlaneFromList(std::vector<brush_t> &brushes, const aabb3d &bounds)
{
    /* pick the plane that splits the least */
    int minsplits = INT_MAX - 1;
    vec_t bestdistribution = VECT_MAX;
    face_t *bestsurface = nullptr;

    /* Two passes - exhaust all non-detail faces before details */
    for (int pass = 0; pass < 2; pass++) {
        for (auto &brush : brushes) {
            if (brush.contents.is_detail() != (pass == 1)) {
                continue;
            }

            for (auto &face : brush.faces) {
                if (face.onnode) {
                    continue;
                }

                const bool hintsplit = map.mtexinfos.at(face.texinfo).flags.is_hint;

                const qbsp_plane_t &plane = map.planes[face.planenum];
                int splits = 0;

                // now check all of the other faces in `brushes` and count how many
                // would get split if we used `face` as the splitting plane
                for (auto &brush2 : brushes) {
                    for (auto &face2 : brush2.faces) {
                        if (face2.planenum == face.planenum || face2.onnode)
                            continue;

                        const surfflags_t &flags = map.mtexinfos.at(face2.texinfo).flags;
                        /* Don't penalize for splitting skip faces */
                        if (flags.is_skip)
                            continue;
                        if (FaceSide(&face2, plane) == SIDE_ON) {
                            /* Never split a hint face except with a hint */
                            if (!hintsplit && flags.is_hint) {
                                splits = INT_MAX;
                                break;
                            }
                            splits++;
                            if (splits >= minsplits)
                                break;
                        }
                    }
                    if (splits > minsplits)
                        break;
                }
                if (splits > minsplits)
                    continue;

                /*
                 * if equal numbers axial planes win, otherwise decide on spatial
                 * subdivision
                 */
                if (splits < minsplits || (splits == minsplits && plane.type < 3)) {
                    if (plane.type < 3) {
                        const vec_t distribution = SplitPlaneMetric(plane, bounds);
                        if (distribution > bestdistribution && splits == minsplits)
                            continue;
                        bestdistribution = distribution;
                    }
                    /* currently the best! */
                    minsplits = splits;
                    bestsurface = &face;
                }
            }
        }

        /* If we found a candidate on first pass, don't do a second pass */
        if (bestsurface != nullptr) {
            return bestsurface;
        }
    }

    return bestsurface;
}

/*
==================
SelectPartition

Selects a surface from a linked list of surfaces to split the group on
returns NULL if the surface list can not be divided any more (a leaf)

Called in parallel.
==================
*/
static face_t *SelectPartition(std::vector<brush_t> &brushes)
{
    // calculate a bounding box of the entire surfaceset
    aabb3d bounds;

    for (auto &brush : brushes) {
        bounds += brush.bounds;
    }

    // how much of the map are we partitioning?
    double fractionOfMap = brushes.size() / (double)mapbrushes;

    bool largenode = false;

    // decide if we should switch to the midsplit method
    if (options.midsplitSurfFraction != 0.0) {
        // new way (opt-in)
        largenode = (fractionOfMap > options.midsplitSurfFraction);
    } else {
        // old way (ericw-tools 0.15.2+)
        if (options.maxNodeSize >= 64) {
            const vec_t maxnodesize = options.maxNodeSize - ON_EPSILON;

            largenode = (bounds.maxs()[0] - bounds.mins()[0]) > maxnodesize ||
                        (bounds.maxs()[1] - bounds.mins()[1]) > maxnodesize ||
                        (bounds.maxs()[2] - bounds.mins()[2]) > maxnodesize;
        }
    }

    if (usemidsplit || largenode) // do fast way for clipping hull
        return ChooseMidPlaneFromList(brushes, bounds);

    // do slow way to save poly splits for drawing hull
    return ChoosePlaneFromList(brushes, bounds);
}

//============================================================================

/*
================
WindingIsTiny

Returns true if the winding would be crunched out of
existance by the vertex snapping.
================
*/
#define EDGE_LENGTH 0.2
bool WindingIsTiny(const winding_t &w)
{
#if 0
	if (WindingArea (w) < 1)
		return true;
	return false;
#else
    int edges = 0;
    for (size_t i = 0; i < w.size(); i++) {
        size_t j = (i + 1) % w.size();
        const qvec3d delta = w[j] - w[i];
        const double len = qv::length(delta);
        if (len > EDGE_LENGTH) {
            if (++edges == 3)
                return false;
        }
    }
    return true;
#endif
}

/*
================
WindingIsHuge

Returns true if the winding still has one of the points
from basewinding for plane
================
*/
bool WindingIsHuge(const winding_t &w)
{
    for (size_t i = 0; i < w.size(); i++) {
        for (size_t  j = 0; j < 3; j++)
            if (w[i][j] < -8000 || w[i][j] > 8000)
                return true;
    }
    return false;
}

/*
==================
BrushMostlyOnSide

==================
*/
side_t BrushMostlyOnSide(const brush_t &brush, const qplane3d &plane)
{
    vec_t max = 0;
    side_t side = SIDE_FRONT;
    for (auto &face : brush.faces) {
        for (size_t j = 0; j < face.w.size(); j++) {
            vec_t d = qv::dot(face.w[j], plane.normal) - plane.dist;
            if (d > max) {
                max = d;
                side = SIDE_FRONT;
            }
            if (-d > max) {
                max = -d;
                side = SIDE_BACK;
            }
        }
    }
    return side;
}

/*
==================
BrushVolume

==================
*/
vec_t BrushVolume(const brush_t &brush)
{
    // grab the first valid point as the corner
    
    bool found = false;
    qvec3d corner;
    for (auto &face : brush.faces) {
        if (face.w.size() > 0) {
            corner = face.w[0];
            found = true;
        }
    }
    if (!found) {
        return 0;
    }

    // make tetrahedrons to all other faces

    vec_t volume = 0;
    for (auto &face : brush.faces) {
        auto plane = Face_Plane(&face);
        vec_t d = -(qv::dot(corner, plane.normal) - plane.dist);
        vec_t area = face.w.area();
        volume += d * area;
    }

    volume /= 3;
    return volume;
}

/*
================
SplitBrush

Generates two new brushes, leaving the original
unchanged

https://github.com/id-Software/Quake-2-Tools/blob/master/bsp/qbsp3/brushbsp.c#L935
================
*/
static twosided<std::optional<brush_t>> SplitBrush(const brush_t &brush, const qplane3d &split)
{
    twosided<std::optional<brush_t>> result;
    
    // check all points
    vec_t d_front = 0;
    vec_t d_back = 0;
    for (auto &face : brush.faces) {
        for (int j = 0; j < face.w.size(); j++) {
            vec_t d = qv::dot(face.w[j], split.normal) - split.dist;
            if (d > 0 && d > d_front)
                d_front = d;
            if (d < 0 && d < d_back)
                d_back = d;
        }
    }
    if (d_front < 0.1) // PLANESIDE_EPSILON)
    { // only on back
        result.back = {brush};
        return result;
    }
    if (d_back > -0.1) // PLANESIDE_EPSILON)
    { // only on front
        result.front = {brush};
        return result;
    }

    // create a new winding from the split plane
    auto w = std::optional<winding_t>{BaseWindingForPlane(split)};
    for (auto &face : brush.faces) {
        if (!w) {
            break;
        }
        auto [frontOpt, backOpt] = w->clip(Face_Plane(&face));
        w = backOpt;
    }

    if (!w || WindingIsTiny(*w)) { // the brush isn't really split
        side_t side = BrushMostlyOnSide(brush, split);
        if (side == SIDE_FRONT)
            result.front = {brush};
        else
            result.back = {brush};
        return result;
    }

    if (WindingIsHuge(*w)) {
        LogPrint("WARNING: huge winding\n");
    }

    winding_t midwinding = *w;

    // split it for real

    // start with 2 empty brushes

    for (int i = 0; i < 2; i++) {
        result[i] = { brush_t{} };
        // fixme-brushbsp: set original pointer
        //b[i]->original = brush->original;
        // fixme-brushbsp: add a brush_t copy constructor to make sure we get all fields
        result[i]->contents = brush.contents;
        result[i]->lmshift = brush.lmshift;
    }

    // split all the current windings

    for (const auto& face : brush.faces) {
        auto cw = face.w.clip(split, 0 /*PLANESIDE_EPSILON*/);
        for (size_t j = 0; j < 2; j++) {
            if (!cw[j])
                continue;
#if 0
			if (WindingIsTiny (cw[j]))
			{
				FreeWinding (cw[j]);
				continue;
			}
#endif

            // add the clipped face to result[j]
            face_t faceCopy = face;
            faceCopy.w = *cw[j];
            
            // fixme-brushbsp: configure any settings on the faceCopy?
            // Q2 does `cs->tested = false;`, why?

            result[j]->faces.push_back(std::move(faceCopy));
        }
    }

    // see if we have valid polygons on both sides

    for (int i = 0; i < 2; i++) {
        result[i]->update_bounds();

        bool bogus = false;
        for (int j = 0; j < 3; j++) {
            if (result[i]->bounds.mins()[j] < -4096 || result[i]->bounds.maxs()[j] > 4096) {
                LogPrint("bogus brush after clip\n");
                bogus = true;
                break;
            }
        }

        if (result[i]->faces.size() < 3 || bogus) {
            result[i] = std::nullopt;
        }
    }

    if (!(result[0] && result[1])) {
        if (!result[0] && !result[1])
            LogPrint("split removed brush\n");
        else
            LogPrint("split not on both sides\n");
        if (result[0]) {
            result.front = {brush};
        }
        if (result[1]) {
            result.back = {brush};
        }
        return result;
    }

    // add the midwinding to both sides
    for (int i = 0; i < 2; i++) {
        face_t cs{};
        
        const bool front = (i == 0);
        
        cs.planenum = FindPlane(front ? split : -split, &cs.planeside);
        cs.texinfo = MakeSkipTexinfo();

        // fixme-brushbsp: configure any other settings on the face?

        cs.w = front ? midwinding : midwinding.flip();

        result[i]->faces.push_back(std::move(cs));
    }

    {
        vec_t v1;
        int i;

        for (i = 0; i < 2; i++) {
            v1 = BrushVolume(*result[i]);
            if (v1 < 1.0) {
                result[i] = std::nullopt;
                //			qprintf ("tiny volume after clip\n");
            }
        }
    }

    return result;
}

//============================================================================

/*
==================
DivideNodeBounds
==================
*/
inline void DivideNodeBounds(node_t *node, const qbsp_plane_t &split)
{
    DivideBounds(node->bounds, split, node->children[0]->bounds, node->children[1]->bounds);
}

static bool AllDetail(const std::vector<brush_t> &brushes)
{
    for (auto &brush : brushes) {
        if (!brush.contents.is_detail()) {
            return false;
        }
    }
    return true;
}

static contentflags_t MergeContents(contentflags_t a, contentflags_t b)
{
    // fixme-brushbsp: Q2 can combine the two content types (under some circumstances?)

    auto a_pri = a.priority(options.target_game);
    auto b_pri = b.priority(options.target_game);

    if (a_pri > b_pri) {
        return a;
    }
    return b;
}

/*
==================
CreateLeaf

Determines the contents of the leaf and creates the final list of
original faces that have some fragment inside this leaf.

Called in parallel.
==================
*/
static void CreateLeaf(const std::vector<brush_t> &brushes, node_t *leafnode)
{
    leafnode->facelist.clear();
    leafnode->planenum = PLANENUM_LEAF;

    leafnode->contents = options.target_game->create_empty_contents();
    for (auto &brush : brushes) {
        leafnode->contents = MergeContents(leafnode->contents, brush.contents);
    }

    if (leafnode->contents.extended & CFLAGS_ILLUSIONARY_VISBLOCKER) {
        c_illusionary_visblocker++;
    } else if (leafnode->contents.extended & CFLAGS_DETAIL_FENCE) {
        c_detail_fence++;
    } else if (leafnode->contents.extended & CFLAGS_DETAIL_ILLUSIONARY) {
        c_detail_illusionary++;
    } else if (leafnode->contents.extended & CFLAGS_DETAIL) {
        c_detail++;
    } else if (leafnode->contents.is_empty(options.target_game)) {
        c_empty++;
    } else if (leafnode->contents.is_solid(options.target_game)) {
        c_solid++;
    } else if (leafnode->contents.is_liquid(options.target_game) || leafnode->contents.is_sky(options.target_game)) {
        c_water++;
    } else {
        // FIXME: what to call here? is_valid()? this hits in Q2 a lot
        // FError("Bad contents in face: {}", leafnode->contents.to_string(options.target_game));
    }

    // fixme-brushbsp: move somewhere else
#if 0
    // write the list of the original faces to the leaf's markfaces
    // free surf and the surf->faces list.
    leaffaces += count;
    leafnode->markfaces.reserve(count);

    for (auto &surf : planelist) {
        for (auto &f : surf.faces) {
            leafnode->markfaces.push_back(f->original);
            delete f;
        }
    }
#endif
}

/*
==================
PartitionBrushes

Called in parallel.
==================
*/
static void PartitionBrushes(std::vector<brush_t> &brushes, node_t *node)
{
    face_t *split = SelectPartition(brushes);

    if (split == nullptr) { // this is a leaf node
        node->planenum = PLANENUM_LEAF;

        CreateLeaf(brushes, node);
        return;
    }

    splitnodes++;

    // fixme-brushbsp: populate somewhere later
    //node->facelist = LinkNodeFaces(*split);
    node->children[0] = new node_t{};
    node->children[1] = new node_t{};
    node->planenum = split->planenum;
    node->detail_separator = AllDetail(brushes);

    const qbsp_plane_t &splitplane = map.planes[split->planenum];

    DivideNodeBounds(node, splitplane);

    // multiple surfaces, so split all the polysurfaces into front and back lists
    std::vector<brush_t> frontlist, backlist;

    for (auto &brush : brushes) {
        auto frags = SplitBrush(brush, splitplane);

        // mark faces which were used as a splitter
        for (auto &brushMaybe : frags) {
            if (brushMaybe) {
                for (auto &face : brushMaybe->faces) {
                    if (face.planenum == split->planenum) {
                        face.onnode = true;
                    }
                }
            }
        }

        if (frags.front) {
            if (frags.front->faces.empty()) {
                FError("Surface with no faces");
            }
            frontlist.emplace_back(std::move(*frags.front));
        }
        if (frags.back) {
            if (frags.back->faces.empty()) {
                FError("Surface with no faces");
            }
            backlist.emplace_back(std::move(*frags.back));
        }
    }

    // FIXME: can't parallelize until we lock all map.planes reads/writes
#if 0
    tbb::task_group g;
    g.run([&]() { PartitionBrushes(frontlist, node->children[0]); });
    g.run([&]() { PartitionBrushes(backlist, node->children[1]); });
    g.wait();
#else
    PartitionBrushes(frontlist, node->children[0]);
    PartitionBrushes(backlist, node->children[1]);
#endif
}

/*
==================
SolidBSP
==================
*/
node_t *SolidBSP(mapentity_t *entity, bool midsplit)
{
    if (entity->brushes.empty()) {
        /*
         * We allow an entity to be constructed with no visible brushes
         * (i.e. all clip brushes), but need to construct a simple empty
         * collision hull for the engine. Probably could be done a little
         * smarter, but this works.
         */
        node_t *headnode = new node_t{};
        headnode->bounds = entity->bounds.grow(SIDESPACE);
        headnode->children[0] = new node_t{};
        headnode->children[0]->planenum = PLANENUM_LEAF;
        headnode->children[0]->contents = options.target_game->create_empty_contents();
        headnode->children[1] = new node_t{};
        headnode->children[1]->planenum = PLANENUM_LEAF;
        headnode->children[1]->contents = options.target_game->create_empty_contents();

        return headnode;
    }

    LogPrint(LOG_PROGRESS, "---- {} ----\n", __func__);

    node_t *headnode = new node_t{};
    usemidsplit = midsplit;

    // calculate a bounding box for the entire model
    headnode->bounds = entity->bounds.grow(SIDESPACE);

    // recursively partition everything
    splitnodes = 0;
    leaffaces = 0;
    nodefaces = 0;
    c_solid = 0;
    c_empty = 0;
    c_water = 0;
    c_detail = 0;
    c_detail_illusionary = 0;
    c_detail_fence = 0;
    c_illusionary_visblocker = 0;
    // count map surfaces; this is used when deciding to switch between midsplit and the expensive partitioning
    mapbrushes = entity->brushes.size();

    PartitionBrushes(entity->brushes, headnode);

    LogPrint(LOG_STAT, "     {:8} split nodes\n", splitnodes.load());
    LogPrint(LOG_STAT, "     {:8} solid leafs\n", c_solid.load());
    LogPrint(LOG_STAT, "     {:8} empty leafs\n", c_empty.load());
    LogPrint(LOG_STAT, "     {:8} water leafs\n", c_water.load());
    LogPrint(LOG_STAT, "     {:8} detail leafs\n", c_detail.load());
    LogPrint(LOG_STAT, "     {:8} detail illusionary leafs\n", c_detail_illusionary.load());
    LogPrint(LOG_STAT, "     {:8} detail fence leafs\n", c_detail_fence.load());
    LogPrint(LOG_STAT, "     {:8} illusionary visblocker leafs\n", c_illusionary_visblocker.load());
    LogPrint(LOG_STAT, "     {:8} leaffaces\n", leaffaces.load());
    LogPrint(LOG_STAT, "     {:8} nodefaces\n", nodefaces.load());

    return headnode;
}
