//
// rect.cpp — SDL2's rectangle arithmetic
//
// Small, exact, and used by everything: clipping a blit, clipping a fill,
// working out what part of a window a game actually touched. SDL2 treats a
// rectangle with a width or height of zero or less as EMPTY, and every
// function here follows that rule rather than the more obvious one, because
// an application's clipping logic is written against it.
//
#include <SDL2/SDL.h>

namespace
{
inline bool rect_empty(const SDL_Rect *r)
{
    return r == nullptr || r->w <= 0 || r->h <= 0;
}
inline bool frect_empty(const SDL_FRect *r)
{
    return r == nullptr || r->w <= 0.0f || r->h <= 0.0f;
}
} // namespace

extern "C" SDL_bool SDL_HasIntersection(const SDL_Rect *A, const SDL_Rect *B)
{
    if (rect_empty(A) || rect_empty(B))
        return SDL_FALSE;
    if (A->x >= B->x + B->w || B->x >= A->x + A->w)
        return SDL_FALSE;
    if (A->y >= B->y + B->h || B->y >= A->y + A->h)
        return SDL_FALSE;
    return SDL_TRUE;
}

extern "C" SDL_bool SDL_IntersectRect(const SDL_Rect *A, const SDL_Rect *B,
                                      SDL_Rect *result)
{
    if (result == nullptr)
        return SDL_HasIntersection(A, B);
    if (rect_empty(A) || rect_empty(B))
    {
        result->x = result->y = result->w = result->h = 0;
        return SDL_FALSE;
    }

    const int x0 = (A->x > B->x) ? A->x : B->x;
    const int y0 = (A->y > B->y) ? A->y : B->y;
    const int ax1 = A->x + A->w, bx1 = B->x + B->w;
    const int ay1 = A->y + A->h, by1 = B->y + B->h;
    const int x1 = (ax1 < bx1) ? ax1 : bx1;
    const int y1 = (ay1 < by1) ? ay1 : by1;

    result->x = x0;
    result->y = y0;
    result->w = x1 - x0;
    result->h = y1 - y0;
    if (result->w <= 0 || result->h <= 0)
    {
        result->w = 0;
        result->h = 0;
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

extern "C" void SDL_UnionRect(const SDL_Rect *A, const SDL_Rect *B, SDL_Rect *result)
{
    if (result == nullptr)
        return;
    // SDL2 treats an empty operand as contributing nothing, so the union of
    // an empty rectangle and a real one is the real one — not a rectangle
    // stretched back to the origin.
    if (rect_empty(A))
    {
        if (rect_empty(B))
        {
            result->x = result->y = result->w = result->h = 0;
            return;
        }
        *result = *B;
        return;
    }
    if (rect_empty(B))
    {
        *result = *A;
        return;
    }

    const int x0 = (A->x < B->x) ? A->x : B->x;
    const int y0 = (A->y < B->y) ? A->y : B->y;
    const int ax1 = A->x + A->w, bx1 = B->x + B->w;
    const int ay1 = A->y + A->h, by1 = B->y + B->h;
    const int x1 = (ax1 > bx1) ? ax1 : bx1;
    const int y1 = (ay1 > by1) ? ay1 : by1;

    result->x = x0;
    result->y = y0;
    result->w = x1 - x0;
    result->h = y1 - y0;
}

extern "C" SDL_bool SDL_EnclosePoints(const SDL_Point *points, int count,
                                      const SDL_Rect *clip, SDL_Rect *result)
{
    if (points == nullptr || count < 1)
        return SDL_FALSE;

    int minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool found = false;
    for (int i = 0; i < count; i++)
    {
        const int x = points[i].x;
        const int y = points[i].y;
        if (clip != nullptr)
        {
            if (rect_empty(clip))
                return SDL_FALSE;
            if (x < clip->x || x >= clip->x + clip->w ||
                y < clip->y || y >= clip->y + clip->h)
                continue;
        }
        if (!found)
        {
            minx = maxx = x;
            miny = maxy = y;
            found = true;
            continue;
        }
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (!found)
        return SDL_FALSE;
    if (result != nullptr)
    {
        result->x = minx;
        result->y = miny;
        result->w = maxx - minx + 1;
        result->h = maxy - miny + 1;
    }
    return SDL_TRUE;
}

// Cohen-Sutherland, on the integer grid SDL2 uses: the rectangle's right and
// bottom edges are at x+w-1 and y+h-1, because a line is drawn through
// pixels rather than between them.
extern "C" SDL_bool SDL_IntersectRectAndLine(const SDL_Rect *rect,
                                             int *X1, int *Y1, int *X2, int *Y2)
{
    if (rect_empty(rect) || X1 == nullptr || Y1 == nullptr ||
        X2 == nullptr || Y2 == nullptr)
        return SDL_FALSE;

    const int rectx1 = rect->x;
    const int recty1 = rect->y;
    const int rectx2 = rect->x + rect->w - 1;
    const int recty2 = rect->y + rect->h - 1;

    int x1 = *X1, y1 = *Y1, x2 = *X2, y2 = *Y2;

    // Wholly outside on one side: nothing to trim, nothing to draw.
    if ((x1 < rectx1 && x2 < rectx1) || (x1 > rectx2 && x2 > rectx2) ||
        (y1 < recty1 && y2 < recty1) || (y1 > recty2 && y2 > recty2))
        return SDL_FALSE;

    if (y1 == y2)
    {
        if (x1 < rectx1) x1 = rectx1;
        if (x1 > rectx2) x1 = rectx2;
        if (x2 < rectx1) x2 = rectx1;
        if (x2 > rectx2) x2 = rectx2;
    }
    else if (x1 == x2)
    {
        if (y1 < recty1) y1 = recty1;
        if (y1 > recty2) y1 = recty2;
        if (y2 < recty1) y2 = recty1;
        if (y2 > recty2) y2 = recty2;
    }
    else
    {
        // Liang-Barsky along the segment: keep the interval of the parameter
        // t for which the point is inside all four edges, then evaluate the
        // two ends once. A double carries screen-sized integer coordinates
        // and their ratios exactly enough that the endpoint it produces is
        // the same pixel the exact arithmetic would name.
        const int dx = x2 - x1;
        const int dy = y2 - y1;

        double t0 = 0.0, t1 = 1.0;

        auto clip = [&](double p, double q) -> bool {
            if (p == 0.0)
                return q >= 0.0;            // parallel to this edge
            const double t = q / p;
            if (p < 0.0)
            {
                if (t > t1) return false;
                if (t > t0) t0 = t;
            }
            else
            {
                if (t < t0) return false;
                if (t < t1) t1 = t;
            }
            return true;
        };

        if (!clip(-(double)dx, (double)(x1 - rectx1))) return SDL_FALSE;
        if (!clip( (double)dx, (double)(rectx2 - x1))) return SDL_FALSE;
        if (!clip(-(double)dy, (double)(y1 - recty1))) return SDL_FALSE;
        if (!clip( (double)dy, (double)(recty2 - y1))) return SDL_FALSE;

        const int nx1 = x1 + (int)(t0 * dx);
        const int ny1 = y1 + (int)(t0 * dy);
        const int nx2 = x1 + (int)(t1 * dx);
        const int ny2 = y1 + (int)(t1 * dy);
        x1 = nx1; y1 = ny1; x2 = nx2; y2 = ny2;
    }

    *X1 = x1; *Y1 = y1; *X2 = x2; *Y2 = y2;
    return SDL_TRUE;
}

// ---------------------------------------------------------------------------
// The floating-point forms, used by the SDL_FRect rendering entry points
// ---------------------------------------------------------------------------

extern "C" SDL_bool SDL_HasIntersectionF(const SDL_FRect *A, const SDL_FRect *B)
{
    if (frect_empty(A) || frect_empty(B))
        return SDL_FALSE;
    if (A->x >= B->x + B->w || B->x >= A->x + A->w)
        return SDL_FALSE;
    if (A->y >= B->y + B->h || B->y >= A->y + A->h)
        return SDL_FALSE;
    return SDL_TRUE;
}

extern "C" SDL_bool SDL_IntersectFRect(const SDL_FRect *A, const SDL_FRect *B,
                                       SDL_FRect *result)
{
    if (result == nullptr)
        return SDL_HasIntersectionF(A, B);
    if (frect_empty(A) || frect_empty(B))
    {
        result->x = result->y = result->w = result->h = 0.0f;
        return SDL_FALSE;
    }
    const float x0 = (A->x > B->x) ? A->x : B->x;
    const float y0 = (A->y > B->y) ? A->y : B->y;
    const float ax1 = A->x + A->w, bx1 = B->x + B->w;
    const float ay1 = A->y + A->h, by1 = B->y + B->h;
    const float x1 = (ax1 < bx1) ? ax1 : bx1;
    const float y1 = (ay1 < by1) ? ay1 : by1;

    result->x = x0;
    result->y = y0;
    result->w = x1 - x0;
    result->h = y1 - y0;
    if (result->w <= 0.0f || result->h <= 0.0f)
    {
        result->w = 0.0f;
        result->h = 0.0f;
        return SDL_FALSE;
    }
    return SDL_TRUE;
}

extern "C" void SDL_UnionFRect(const SDL_FRect *A, const SDL_FRect *B,
                               SDL_FRect *result)
{
    if (result == nullptr)
        return;
    if (frect_empty(A))
    {
        if (frect_empty(B))
        {
            result->x = result->y = result->w = result->h = 0.0f;
            return;
        }
        *result = *B;
        return;
    }
    if (frect_empty(B))
    {
        *result = *A;
        return;
    }
    const float x0 = (A->x < B->x) ? A->x : B->x;
    const float y0 = (A->y < B->y) ? A->y : B->y;
    const float ax1 = A->x + A->w, bx1 = B->x + B->w;
    const float ay1 = A->y + A->h, by1 = B->y + B->h;
    const float x1 = (ax1 > bx1) ? ax1 : bx1;
    const float y1 = (ay1 > by1) ? ay1 : by1;

    result->x = x0;
    result->y = y0;
    result->w = x1 - x0;
    result->h = y1 - y0;
}

extern "C" SDL_bool SDL_EncloseFPoints(const SDL_FPoint *points, int count,
                                       const SDL_FRect *clip, SDL_FRect *result)
{
    if (points == nullptr || count < 1)
        return SDL_FALSE;

    float minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool  found = false;
    for (int i = 0; i < count; i++)
    {
        const float x = points[i].x;
        const float y = points[i].y;
        if (clip != nullptr)
        {
            if (frect_empty(clip))
                return SDL_FALSE;
            if (x < clip->x || x > clip->x + clip->w ||
                y < clip->y || y > clip->y + clip->h)
                continue;
        }
        if (!found)
        {
            minx = maxx = x;
            miny = maxy = y;
            found = true;
            continue;
        }
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    if (!found)
        return SDL_FALSE;
    if (result != nullptr)
    {
        result->x = minx;
        result->y = miny;
        result->w = maxx - minx;
        result->h = maxy - miny;
    }
    return SDL_TRUE;
}
