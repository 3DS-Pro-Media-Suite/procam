#include "ui_shapes.h"
#include <3ds.h>
#include <citro2d.h>
#include <math.h>

// The GPU fills whole pixels with hard edges, so curves and diagonals stair-step. Each shape here is a solid
// core plus a 1 px ring whose alpha falls to 0 across the true edge, so edge pixels get about their coverage.
// The parts never overlap, so translucent colours work.

#define MAX_POLYGON_POINTS 128

// Half the width of the soft edge, which is centred on the outline.
#define SOFT_EDGE_HALF_WIDTH 0.5f

// Points closer than this count as one.
#define SAME_POINT_DISTANCE 1e-3f
// A shorter miter means the two edge normals cancel out, so the corner uses its edge normal.
#define MIN_MITER_LENGTH 1e-4f
// Limits how far a sharp corner moves along its bisector.
#define MIN_MITER_COSINE 0.35f

// An arc this close to a full turn is a closed ring.
#define FULL_TURN_TOLERANCE 1e-3f
#define HALF_TURN 3.14159265f
#define QUARTER_TURN 1.5707963f

// Curves get more segments as they grow, within limits.
#define CIRCLE_SEGMENTS_PER_RADIUS 1.5f
#define CIRCLE_BASE_SEGMENTS 8
#define CIRCLE_MIN_SEGMENTS 12
#define CIRCLE_MAX_SEGMENTS 96
#define CORNER_SEGMENTS_PER_RADIUS 0.9f
#define CORNER_BASE_SEGMENTS 2
#define CORNER_MAX_SEGMENTS 16
#define ARC_SEGMENTS_PER_PIXEL 0.5f
#define ARC_BASE_SEGMENTS 8
#define ARC_MAX_SEGMENTS 96

// Corners with a smaller radius are drawn square.
#define MIN_CORNER_RADIUS 0.25f
// A thin bar's soft edge is at most this fraction of its thickness, so it keeps a solid core.
#define THIN_SHAPE_FEATHER_RATIO 0.25f

// Frames and arcs are four rings: transparent, solid, solid, transparent.
#define RING_COUNT 4
#define RECT_CORNER_COUNT 4

// The back of a pointer sits this fraction of its size behind the centre.
#define POINTER_BACK_RATIO 0.7f

// C2D colours are 0xAABBGGRR.
#define ALPHA_SHIFT 24
#define RGB_MASK 0x00FFFFFFu

static u32 scale_alpha(u32 color, float factor)
{
	u32 alpha = (u32)((color >> ALPHA_SHIFT) * factor + 0.5f);
	u32 clampedAlpha = alpha > 255 ? 255 : alpha;
	return (color & RGB_MASK) | clampedAlpha << ALPHA_SHIFT;
}

// Copies the points without repeats, which have no edge direction. Returns the number of points kept.
static int copy_distinct_points(const float *inputX, const float *inputY, int inputCount, float *pointX,
                                float *pointY)
{
	int count = 0;
	for (int i = 0; i < inputCount && count < MAX_POLYGON_POINTS; i++) {
		if (count && fabsf(inputX[i] - pointX[count - 1]) < SAME_POINT_DISTANCE &&
		    fabsf(inputY[i] - pointY[count - 1]) < SAME_POINT_DISTANCE) {
			continue;
		}
		pointX[count] = inputX[i];
		pointY[count] = inputY[i];
		count++;
	}
	while (count > 2 && fabsf(pointX[count - 1] - pointX[0]) < SAME_POINT_DISTANCE &&
	       fabsf(pointY[count - 1] - pointY[0]) < SAME_POINT_DISTANCE) {
		count--;
	}
	return count;
}

// Fills a convex polygon given in either winding. `feather` is half the width of the soft edge.
static void fill_soft_polygon(const float *inputX, const float *inputY, int inputCount, u32 color,
                              float depth, float feather)
{
	float pointX[MAX_POLYGON_POINTS];
	float pointY[MAX_POLYGON_POINTS];
	int count = copy_distinct_points(inputX, inputY, inputCount, pointX, pointY);
	if (count < 3) {
		return;
	}
	float centerX = 0;
	float centerY = 0;
	for (int i = 0; i < count; i++) {
		centerX += pointX[i];
		centerY += pointY[i];
	}
	centerX /= count;
	centerY /= count;

	// Outward normal of the edge from point i to point i + 1.
	float normalX[MAX_POLYGON_POINTS];
	float normalY[MAX_POLYGON_POINTS];
	for (int i = 0; i < count; i++) {
		int next = (i + 1) % count;
		float edgeX = pointX[next] - pointX[i];
		float edgeY = pointY[next] - pointY[i];
		float edgeLength = sqrtf(edgeX * edgeX + edgeY * edgeY);
		normalX[i] = edgeY / edgeLength;
		normalY[i] = -edgeX / edgeLength;
		float middleX = (pointX[i] + pointX[next]) * 0.5f;
		float middleY = (pointY[i] + pointY[next]) * 0.5f;
		if (normalX[i] * (middleX - centerX) + normalY[i] * (middleY - centerY) < 0) {
			normalX[i] = -normalX[i];
			normalY[i] = -normalY[i];
		}
	}

	// Each corner moves along its bisector so both of its edges shift by the feather.
	float innerX[MAX_POLYGON_POINTS];
	float innerY[MAX_POLYGON_POINTS];
	float outerX[MAX_POLYGON_POINTS];
	float outerY[MAX_POLYGON_POINTS];
	for (int i = 0; i < count; i++) {
		int previous = (i + count - 1) % count;
		float miterX = normalX[previous] + normalX[i];
		float miterY = normalY[previous] + normalY[i];
		float miterLength = sqrtf(miterX * miterX + miterY * miterY);
		if (miterLength < MIN_MITER_LENGTH) {
			miterX = normalX[i];
			miterY = normalY[i];
		} else {
			miterX /= miterLength;
			miterY /= miterLength;
		}
		float cosine = miterX * normalX[i] + miterY * normalY[i];
		float shift = feather / (cosine < MIN_MITER_COSINE ? MIN_MITER_COSINE : cosine);
		innerX[i] = pointX[i] - miterX * shift;
		innerY[i] = pointY[i] - miterY * shift;
		outerX[i] = pointX[i] + miterX * shift;
		outerY[i] = pointY[i] + miterY * shift;
	}

	u32 transparent = scale_alpha(color, 0);
	for (int i = 1; i + 1 < count; i++) {
		C2D_DrawTriangle(innerX[0], innerY[0], color, innerX[i], innerY[i], color, innerX[i + 1],
		                 innerY[i + 1], color, depth);
	}
	for (int i = 0; i < count; i++) {
		int next = (i + 1) % count;
		C2D_DrawTriangle(innerX[i], innerY[i], color, innerX[next], innerY[next], color, outerX[next],
		                 outerY[next], transparent, depth);
		C2D_DrawTriangle(innerX[i], innerY[i], color, outerX[next], outerY[next], transparent, outerX[i],
		                 outerY[i], transparent, depth);
	}
}

void ui_shapes_circle(float centerX, float centerY, float radius, u32 color, float depth)
{
	int segments = (int)(radius * CIRCLE_SEGMENTS_PER_RADIUS) + CIRCLE_BASE_SEGMENTS;
	if (segments < CIRCLE_MIN_SEGMENTS) {
		segments = CIRCLE_MIN_SEGMENTS;
	}
	if (segments > CIRCLE_MAX_SEGMENTS) {
		segments = CIRCLE_MAX_SEGMENTS;
	}
	float pointX[CIRCLE_MAX_SEGMENTS];
	float pointY[CIRCLE_MAX_SEGMENTS];
	for (int i = 0; i < segments; i++) {
		float angle = UI_FULL_TURN * i / segments;
		pointX[i] = centerX + radius * cosf(angle);
		pointY[i] = centerY + radius * sinf(angle);
	}
	// A circle smaller than a pixel gets a narrower soft edge.
	float feather = radius < 1 ? radius * SOFT_EDGE_HALF_WIDTH : SOFT_EDGE_HALF_WIDTH;
	fill_soft_polygon(pointX, pointY, segments, color, depth, feather);
}

void ui_shapes_round_rect(float x, float y, float width, float height, float radius, u32 color, float depth)
{
	if (radius > width * 0.5f) {
		radius = width * 0.5f;
	}
	if (radius > height * 0.5f) {
		radius = height * 0.5f;
	}
	float feather = width < height ? width * THIN_SHAPE_FEATHER_RATIO : height * THIN_SHAPE_FEATHER_RATIO;
	if (feather > SOFT_EDGE_HALF_WIDTH) {
		feather = SOFT_EDGE_HALF_WIDTH;
	}
	float outlineX[MAX_POLYGON_POINTS];
	float outlineY[MAX_POLYGON_POINTS];
	int pointCount = 0;
	if (radius < MIN_CORNER_RADIUS) {
		outlineX[0] = x;
		outlineY[0] = y;
		outlineX[1] = x + width;
		outlineY[1] = y;
		outlineX[2] = x + width;
		outlineY[2] = y + height;
		outlineX[3] = x;
		outlineY[3] = y + height;
		pointCount = 4;
	} else {
		int cornerSegments = (int)(radius * CORNER_SEGMENTS_PER_RADIUS) + CORNER_BASE_SEGMENTS;
		if (cornerSegments > CORNER_MAX_SEGMENTS) {
			cornerSegments = CORNER_MAX_SEGMENTS;
		}
		const float cornerCenterX[RECT_CORNER_COUNT] = {x + radius, x + width - radius, x + width - radius,
		                                                x + radius};
		const float cornerCenterY[RECT_CORNER_COUNT] = {y + radius, y + radius, y + height - radius,
		                                                y + height - radius};
		// Corners go clockwise from the top left, each a quarter turn.
		for (int corner = 0; corner < RECT_CORNER_COUNT; corner++) {
			for (int i = 0; i <= cornerSegments; i++) {
				float angle = HALF_TURN + QUARTER_TURN * (corner + (float)i / cornerSegments);
				outlineX[pointCount] = cornerCenterX[corner] + radius * cosf(angle);
				outlineY[pointCount] = cornerCenterY[corner] + radius * sinf(angle);
				pointCount++;
			}
		}
	}
	fill_soft_polygon(outlineX, outlineY, pointCount, color, depth, feather);
}

void ui_shapes_rect(float x, float y, float width, float height, u32 color, float depth)
{
	ui_shapes_round_rect(x, y, width, height, 0, color, depth);
}

// Corners are mitred, so no corner is drawn twice at double alpha.
void ui_shapes_frame(float x, float y, float width, float height, float thickness, u32 color, float depth)
{
	// A frame thinner than a pixel draws 1 px wide at reduced alpha.
	if (thickness < 1) {
		color = scale_alpha(color, thickness);
		thickness = 1;
	}
	u32 transparent = scale_alpha(color, 0);
	// Outward offset of each ring from the box edge.
	const float ringOffset[RING_COUNT] = {SOFT_EDGE_HALF_WIDTH, -SOFT_EDGE_HALF_WIDTH,
	                                      SOFT_EDGE_HALF_WIDTH - thickness,
	                                      -SOFT_EDGE_HALF_WIDTH - thickness};
	const u32 ringColor[RING_COUNT] = {transparent, color, color, transparent};
	float ringX[RING_COUNT][RECT_CORNER_COUNT];
	float ringY[RING_COUNT][RECT_CORNER_COUNT];
	for (int ring = 0; ring < RING_COUNT; ring++) {
		ringX[ring][0] = x - ringOffset[ring];
		ringY[ring][0] = y - ringOffset[ring];
		ringX[ring][1] = x + width + ringOffset[ring];
		ringY[ring][1] = y - ringOffset[ring];
		ringX[ring][2] = x + width + ringOffset[ring];
		ringY[ring][2] = y + height + ringOffset[ring];
		ringX[ring][3] = x - ringOffset[ring];
		ringY[ring][3] = y + height + ringOffset[ring];
	}
	for (int corner = 0; corner < RECT_CORNER_COUNT; corner++) {
		int nextCorner = (corner + 1) % RECT_CORNER_COUNT;
		for (int ring = 0; ring < RING_COUNT - 1; ring++) {
			C2D_DrawTriangle(ringX[ring][corner], ringY[ring][corner], ringColor[ring],
			                 ringX[ring][nextCorner], ringY[ring][nextCorner], ringColor[ring],
			                 ringX[ring + 1][nextCorner], ringY[ring + 1][nextCorner], ringColor[ring + 1],
			                 depth);
			C2D_DrawTriangle(ringX[ring][corner], ringY[ring][corner], ringColor[ring],
			                 ringX[ring + 1][nextCorner], ringY[ring + 1][nextCorner], ringColor[ring + 1],
			                 ringX[ring + 1][corner], ringY[ring + 1][corner], ringColor[ring + 1], depth);
		}
	}
}

void ui_shapes_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color, float depth)
{
	const float cornerX[3] = {x0, x1, x2};
	const float cornerY[3] = {y0, y1, y2};
	fill_soft_polygon(cornerX, cornerY, 3, color, depth, SOFT_EDGE_HALF_WIDTH);
}

void ui_shapes_pointer(float centerX, float centerY, float size, int direction, u32 color, float depth)
{
	if (direction == POINTER_DIRECTION_UP) {
		float backY = centerY + size * POINTER_BACK_RATIO;
		ui_shapes_triangle(centerX, centerY - size, centerX - size, backY, centerX + size, backY, color,
		                   depth);
	} else {
		float tipX = centerX + direction * size;
		float backX = centerX - direction * size * POINTER_BACK_RATIO;
		ui_shapes_triangle(tipX, centerY, backX, centerY - size, backX, centerY + size, color, depth);
	}
}

// Four rings of points: a soft inner edge, the solid band and a soft outer edge. An open arc gets soft ends.
void ui_shapes_arc(float centerX, float centerY, float radius, float from, float to, float thickness,
                   u32 color, float depth)
{
	float span = to - from;
	bool fullCircle = fabsf(span) >= UI_FULL_TURN - FULL_TURN_TOLERANCE;
	int segments = (int)(fabsf(span) * radius * ARC_SEGMENTS_PER_PIXEL) + ARC_BASE_SEGMENTS;
	if (segments > ARC_MAX_SEGMENTS) {
		segments = ARC_MAX_SEGMENTS;
	}
	// A band thinner than a pixel draws 1 px wide at reduced alpha.
	if (thickness < 1) {
		color = scale_alpha(color, thickness);
		thickness = 1;
	}
	u32 transparent = scale_alpha(color, 0);
	float halfThickness = thickness * 0.5f;
	const float ringRadius[RING_COUNT] = {
	        radius - halfThickness - SOFT_EDGE_HALF_WIDTH, radius - halfThickness + SOFT_EDGE_HALF_WIDTH,
	        radius + halfThickness - SOFT_EDGE_HALF_WIDTH, radius + halfThickness + SOFT_EDGE_HALF_WIDTH};
	const u32 ringColor[RING_COUNT] = {transparent, color, color, transparent};

	// The points of the last two spokes, used in turn.
	float spokeX[2][RING_COUNT];
	float spokeY[2][RING_COUNT];
	for (int i = 0; i <= segments; i++) {
		float angle = from + span * i / segments;
		float sine = sinf(angle);
		float cosine = cosf(angle);
		for (int ring = 0; ring < RING_COUNT; ring++) {
			spokeX[i & 1][ring] = centerX + ringRadius[ring] * sine;
			spokeY[i & 1][ring] = centerY - ringRadius[ring] * cosine;
		}
		if (i == 0) {
			continue;
		}
		const float *previousX = spokeX[(i - 1) & 1];
		const float *previousY = spokeY[(i - 1) & 1];
		const float *currentX = spokeX[i & 1];
		const float *currentY = spokeY[i & 1];
		for (int ring = 0; ring + 1 < RING_COUNT; ring++) {
			int nextRing = ring + 1;
			C2D_DrawTriangle(previousX[ring], previousY[ring], ringColor[ring], currentX[ring],
			                 currentY[ring], ringColor[ring], currentX[nextRing], currentY[nextRing],
			                 ringColor[nextRing], depth);
			C2D_DrawTriangle(previousX[ring], previousY[ring], ringColor[ring], currentX[nextRing],
			                 currentY[nextRing], ringColor[nextRing], previousX[nextRing],
			                 previousY[nextRing], ringColor[nextRing], depth);
		}
	}
	if (fullCircle) {
		return;
	}

	// Each end fades out over half a pixel past the end angle.
	for (int end = 0; end < 2; end++) {
		float angle = end ? to : from;
		float endSign = end ? 1.0f : -1.0f;
		float spanSign = span < 0 ? -1.0f : 1.0f;
		float outward = endSign * spanSign;
		float sine = sinf(angle);
		float cosine = cosf(angle);
		// The clockwise tangent, half a pixel long.
		float tangentX = cosine * SOFT_EDGE_HALF_WIDTH * outward;
		float tangentY = sine * SOFT_EDGE_HALF_WIDTH * outward;
		for (int ring = 0; ring + 1 < RING_COUNT; ring++) {
			float innerX = centerX + ringRadius[ring] * sine;
			float innerY = centerY - ringRadius[ring] * cosine;
			float outerX = centerX + ringRadius[ring + 1] * sine;
			float outerY = centerY - ringRadius[ring + 1] * cosine;
			C2D_DrawTriangle(innerX, innerY, ringColor[ring], outerX, outerY, ringColor[ring + 1],
			                 outerX + tangentX, outerY + tangentY, transparent, depth);
			C2D_DrawTriangle(innerX, innerY, ringColor[ring], outerX + tangentX, outerY + tangentY,
			                 transparent, innerX + tangentX, innerY + tangentY, transparent, depth);
		}
	}
}
