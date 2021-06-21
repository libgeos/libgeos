/**********************************************************************
 *
 * GEOS - Geometry Engine Open Source
 * http://geos.osgeo.org
 *
 * Copyright (C) 2021 Paul Ramsey <pramsey@cleverelephant.ca>
 * Copyright (C) 2021 Martin Davis
 *
 * This is free software; you can redistribute and/or modify it under
 * the terms of the GNU Lesser General Public Licence as published
 * by the Free Software Foundation.
 * See the COPYING file for more information.
 *
 **********************************************************************/

#include <geos/algorithm/LineIntersector.h>
#include <geos/algorithm/Orientation.h>
#include <geos/algorithm/PointLocation.h>
#include <geos/geom/Coordinate.h>
#include <geos/geom/Geometry.h>
#include <geos/geom/LinearRing.h>
#include <geos/geom/Location.h>
#include <geos/geom/Polygon.h>
#include <geos/noding/BasicSegmentString.h>
#include <geos/noding/MCIndexNoder.h>
#include <geos/noding/SegmentString.h>
#include <geos/operation/valid/PolygonIntersectionAnalyzer.h>
#include <geos/operation/valid/PolygonRing.h>
#include <geos/operation/valid/PolygonTopologyAnalyzer.h>
#include <geos/operation/valid/RepeatedPointRemover.h>

namespace geos {      // geos
namespace operation { // geos.operation
namespace valid {     // geos.operation.valid

using namespace geos::geom;


/* public */
PolygonTopologyAnalyzer(const Geometry* geom, bool p_isInvertedRingValid)
    : inputGeom(geom)
    , isInvertedRingValid(p_isInvertedRingValid)
    , intFinder(p_isInvertedRingValid)
    , disconnectionPt(nullptr)
{
    if (! geom->isEmpty()) {
        std::vector<SegmentString*> segStrings = createSegmentStrings(geom, p_isInvertedRingValid);
        polyRings = getPolygonRings(segStrings);
        // Code copied in from analyzeIntersections()
        MCIndexNoder noder;
        noder.setSegmentIntersector(&intFinder);
        noder.computeNodes(segStrings);
    }
}


/* public static */
const Coordinate*
findSelfIntersection(const LinearRing* ring)
{
    PolygonTopologyAnalyzer ata(ring, false);
    if (ata.hasIntersection())
        return ata.getIntersectionLocation();
    return nullptr;
}


/* public static */
bool
isSegmentInRing(const Coordinate* p0, const Coordinate* p1,
    const LinearRing* ring) const
{
    const CoordinateSequence* ringPts = ring->getCoordinatesRO();
    Location loc = algorithm::PointLocation::locateInRing(*p0, *ringPts);
    if (loc == Location::EXTERIOR) return false;
    if (loc == Location::INTERIOR) return true;

    /**
    * The segment point is on the boundary of the ring.
    * Use the topology at the node to check if the segment
    * is inside or outside the ring.
    */
    return isIncidentSegmentInRing(p0, p1, ringPts);
}


/* public static */
bool
isIncidentSegmentInRing(const Coordinate* p0, const Coordinate* p1,
    const CoordinateSequence* ringPts) const
{
    int index = intersectingSegIndex(ringPts, p0);
    if (index < 0) {
        throw IllegalArgumentException("Segment vertex does not intersect ring");
    }
    const Coordinate& rPrev = ringPts.getAt(index);
    const Coordinate& rNext = ringPts.getAt(index + 1);
    if (p0.equals2D(ringPts.getAt(index))) {
        rPrev = ringPts.getAt(ringIndexPrev(ringPts, index));
    }
    /**
    * If ring orientation is not normalized, flip the corner orientation
    */
    bool isInteriorOnRight = ! Orientation::isCCW(ringPts);
    if (! isInteriorOnRight) {
        const Coordinate* temp = rPrev;
        rPrev = rNext;
        rNext = temp;
    }
    return PolygonNode::isInteriorSegment(&p0, &rPrev, &rNext, &p1);
}


/* private static */
int
intersectingSegIndex(const CoordinateSequence* ringPts, const Coordinate& pt) const
{
    LineIntersector li;
    for (int i = 0; i < ringPts->size() - 1; i++) {
      li.computeIntersection(pt, ringPts->getAt(i), ringPts->getAt(i+1));
      if (li.hasIntersection()) {
        //-- check if pt is the start point of the next segment
        if (pt.equals2D(ringPts->getAt(i + 1))) {
          return i + 1;
        }
        return i;
      }
    }
    return -1;
}


/* private static */
int
ringIndexPrev(const CoordinateSequence* ringPts, int index) const
{
    int iPrev = index - 1;
    if (index == 0)
        iPrev = ringPts->size() - 2;
    return iPrev;
}


/* public */
bool
isInteriorDisconnectedByRingCycle() const
{
  /**
   * PolyRings will be null for empty, no hole or LinearRing inputs
   */
    if (polyRings.size() > 0) {
        disconnectionPt = PolygonRing::findTouchCycleLocation(polyRings);
    }
    return disconnectionPt != nullptr;
}


/* public */
bool
isInteriorDisconnectedBySelfTouch() const
{
    if (polyRings.size() > 0) {
        disconnectionPt = PolygonRing::findInteriorSelfNode(polyRings);
    }
    return disconnectionPt != nullptr;
}


/* private static */
std::vector<SegmentString*>
createSegmentStrings(const Geometry* geom, bool isInvertedRingValid)
{
    std::vector<SegmentString*> segStrings;
    if (geom->getGeometryTypeId() == GEOS_LINEARRING) {
        const LinearRing* ring = static_cast<const LinearRing*>(geom);
        segStrings.push_back(createSegString(ring, nullptr));
        return segStrings;
    }
    for (int i = 0; i < geom->getNumGeometries(); i++) {
        assert(geom->getGeometryTypeId() == GEOS_POLYGON);
        const Polygon* poly = static_cast<const Polygon*>(geom);
        if (poly->isEmpty()) continue;
        bool hasHoles = poly->getNumInteriorRing() > 0;

        //--- polygons with no holes do not need connected interior analysis
        PolygonRing* shellRing = nullptr;
        if (hasHoles || isInvertedRingValid) {
            shellRing = createPolygonRing(poly->getExteriorRing());
        }
        segStrings.push_back(createSegString(poly->getExteriorRing(), shellRing));

        for (int j = 0 ; j < poly->getNumInteriorRing(); j++) {
            const LinearRing* hole = poly->getInteriorRingN(j);
            if (hole->isEmpty()) continue;
            PolygonRing* holeRing = createPolygonRing(hole, j, shellRing);
            segStrings.push_back(createSegString(hole, holeRing));
        }
    }
    return segStrings;
}


/* private */
PolygonRing*
createPolygonRing(const LinearRing* p_ring)
{
    polyRingStore.emplace_back(p_ring);
    return &(polyRingStore.back());
}


/* private */
PolygonRing*
createPolygonRing(const LinearRing* p_ring, int p_index, const PolygonRing* p_shell)
{
    polyRingStore.emplace_back(p_ring, p_index, p_shell);
    return &(polyRingStore.back());
}


/* private static */
std::vector<PolygonRing*>
getPolygonRings(const std::vector<SegmentString*>& segStrings)
{
    std::vector<PolygonRing*> polyRings;
    for (SegmentString* ss : segStrings) {
        PolygonRing* polyRing = static_cast<PolygonRing*>(ss->getData());
        if (polyRing != nullptr) {
            polyRings.push_back(polyRing);
        }
    }
    return polyRings;
}


/* private static */
SegmentString*
createSegString(const LinearRing* ring, const PolygonRing* polyRing)
{
    // Let the input LinearRing retain ownership of the
    // CoordinateSequence, and pass it directly into the BasicSegmentString
    // constructor.
    CoordinateSequence* pts = const_cast<CoordinateSequence*>(ring.getCoordinatesRO());

    // Repeated points must be removed for accurate intersection detection
    // So, in this case we create a de-duped copy of the CoordinateSequence
    // and manage the lifecycle locally. This we pass on to the SegmentString
    if (pts->hasRepeatedPoints()) {
        std::unique_ptr<CoordinateSequence> newPts = RepeatedPointRemover::removeRepeatedPoints(pts);
        coordSeqStore.push_back(newPts);
        pts = newPts.get();
    }

    // Allocate the BasicSegmentString in the store and return a
    // pointer into the store. This way we don't have to track the
    // individual SegmentStrings, they just go away when the
    // PolygonTopologyAnalyzer deallocated.
    segStringStore.emplace_back(pts, polyRing);
    SegmentString* ss = static_cast<SegmentString*>(&(segStringStore.back()));
    return ss;
}



} // namespace geos.operation.valid
} // namespace geos.operation
} // namespace geos
