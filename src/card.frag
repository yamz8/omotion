VARYING vec2 pictureUv;
VARYING vec3 worldPos;
VARYING vec3 toEye;
VARYING vec3 surface;

// Whether something stands between a point and a light: each shaped
// piece (a few ellipsoids round its parts) and model (one round its box),
// thirty-two at most, packed in the occluders texture (its middle, its
// half sizes and whose it is, its turn), the line to the light passing
// through or near one. Its own don't count.
vec3 turnBack(vec4 q, vec3 v)
{
    vec3 u = -q.xyz;
    return v + 2.0 * cross(u, cross(u, v) + q.w * v);
}

// A shaped piece's own shape between a point and a light: the line,
// brought into its box (-1 to 1 each way), walked through the cells its
// shape fills; how much of the light gets through.
float throughCells(sampler3D cells, int i, vec3 p, vec3 light)
{
    vec3 middle = texelFetch(solids, ivec2(0, i), 0).xyz;
    vec4 sizeOf = texelFetch(solids, ivec2(1, i), 0);
    if (sizeOf.w == ownLayer)
        return 1.0;
    vec3 size = max(sizeOf.xyz, vec3(0.001));
    // Far from the line, it can't be in the way: no need to look closer.
    vec3 along = light - p;
    float t = clamp(dot(middle - p, along) / max(dot(along, along), 0.000001), 0.0, 1.0);
    if (length(p + along * t - middle) > length(size))
        return 1.0;
    vec4 q = texelFetch(solids, ivec2(2, i), 0);
    vec3 a = turnBack(q, p - middle) / size;
    vec3 b = turnBack(q, light - middle) / size;
    vec3 d = b - a;
    // Where the line is inside the box.
    vec3 inverse = 1.0 / (sign(d) * max(abs(d), vec3(0.000001)));
    vec3 t0 = (-1.0 - a) * inverse, t1 = (1.0 - a) * inverse;
    float enter = max(max(min(t0.x, t1.x), min(t0.y, t1.y)), max(min(t0.z, t1.z), 0.0));
    float leave = min(min(max(t0.x, t1.x), max(t0.y, t1.y)), min(max(t0.z, t1.z), 1.0));
    if (enter >= leave)
        return 1.0;
    float lit = 1.0;
    // A step a cell or so: as many as the line crosses (a side is 32).
    int steps = int(clamp(length(d * (leave - enter)) * 18.0, 4.0, 48.0));
    for (int k = 0; k < 48; ++k) {
        if (k >= steps)
            break;
        float s = mix(enter, leave, (float(k) + 0.5) / float(steps));
        vec3 at = (a + d * s) * 0.5 + 0.5;
        lit *= 1.0 - 0.35 * smoothstep(0.3, 0.7, texture(cells, at).r);
        if (lit < 0.1)
            break;
    }
    return mix(0.12, 1.0, smoothstep(0.1, 0.9, lit));
}

float shadowed(vec3 p, vec3 light)
{
    float lit = 1.0;
    for (int i = 0; i < 32; ++i) {
        if (float(i) >= occluderCount)
            break;
        if (texelFetch(occluders, ivec2(1, i), 0).w == ownLayer)
            continue;
        vec3 middle = texelFetch(occluders, ivec2(0, i), 0).xyz;
        vec3 size = max(texelFetch(occluders, ivec2(1, i), 0).xyz, vec3(0.001));
        vec4 q = texelFetch(occluders, ivec2(2, i), 0);
        vec3 a = turnBack(q, p - middle) / size;
        vec3 b = turnBack(q, light - middle) / size;
        if (length(a) < 1.0)
            continue;
        vec3 d = b - a;
        float t = clamp(-dot(a, d) / max(dot(d, d), 0.000001), 0.0, 1.0);
        lit *= mix(0.12, 1.0, smoothstep(0.7, 1.15, length(a + d * t)));
    }
    if (solidCount > 0.5)
        lit *= throughCells(solid0, 0, p, light);
    if (solidCount > 1.5)
        lit *= throughCells(solid1, 1, p, light);
    if (solidCount > 2.5)
        lit *= throughCells(solid2, 2, p, light);
    if (solidCount > 3.5)
        lit *= throughCells(solid3, 3, p, light);
    if (solidCount > 4.5)
        lit *= throughCells(solid4, 4, p, light);
    if (solidCount > 5.5)
        lit *= throughCells(solid5, 5, p, light);
    if (solidCount > 6.5)
        lit *= throughCells(solid6, 6, p, light);
    if (solidCount > 7.5)
        lit *= throughCells(solid7, 7, p, light);
    return lit;
}

// The lights added to the picture's own: each brightens what faces it,
// falling off with distance, with a highlight as shiny and rough as the
// piece is.
vec3 oneLight(vec3 albedo, vec3 n, vec3 p, vec3 eye, vec3 at, vec3 colour, float reach)
{
    vec3 toLight = at - p;
    float d = length(toLight);
    vec3 l = toLight / max(d, 0.0001);
    float fall = 1.0 / (1.0 + d * d / (reach * reach));
    float diffuse = max(dot(n, l), 0.0);
    vec3 r = reflect(-l, n);
    float highlight = shine * pow(max(dot(r, normalize(eye)), 0.0), mix(80.0, 6.0, roughness)) * step(0.0, diffuse);
    vec3 added = (albedo * diffuse + vec3(highlight)) * colour * fall;
    // Where it adds next to nothing, what's in its way doesn't matter.
    if (max(added.r, max(added.g, added.b)) < 0.002)
        return added;
    return added * shadowed(p, at);
}

vec3 lights(vec3 albedo, vec3 n, vec3 p, vec3 eye)
{
    return oneLight(albedo, n, p, eye, lightAt0, lightColour0, lightReach0)
         + oneLight(albedo, n, p, eye, lightAt1, lightColour1, lightReach1)
         + oneLight(albedo, n, p, eye, lightAt2, lightColour2, lightReach2)
         + oneLight(albedo, n, p, eye, lightAt3, lightColour3, lightReach3);
}

// Smooth noise, -1 to 1, as the flat drawing's.
float hashOf(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z)) * 2.0 - 1.0;
}

float noise3(vec3 x)
{
    vec3 i = floor(x);
    vec3 f = x - i;
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hashOf(i), hashOf(i + vec3(1, 0, 0)), f.x), mix(hashOf(i + vec3(0, 1, 0)), hashOf(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hashOf(i + vec3(0, 0, 1)), hashOf(i + vec3(1, 0, 1)), f.x), mix(hashOf(i + vec3(0, 1, 1)), hashOf(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

// A flat piece, or the picture: as painted, with what the lights add (its
// surface's lie from its mesh, or from how its distances change across the
// screen where the mesh has none),
// fading into the air when farther than the picture had it, and faded out
// as its opacity says.
void MAIN()
{
    // Billowing: where it reads its picture pushed about by drifting noise.
    vec2 uv = pictureUv;
    if (warpAmount > 0.0) {
        vec2 at = vec2(uv.x, 1.0 - uv.y) * warpScale;
        vec2 push = vec2(noise3(vec3(at + warpDrift, warpDrift.x - warpDrift.y)),
                         noise3(vec3(at + vec2(17.3, 4.1) + warpDrift.yx, 9.2 + warpDrift.x)));
        uv += push * warpAmount * vec2(1.0, -warpScale.x / max(warpScale.y, 0.0001));
    }
    // Out of focus, it wears a blurrier copy of its picture: the farther
    // from the focus, the more.
    float defocus = aperture > 0.0 ? clamp(aperture * abs(length(toEye) - focus) / max(focus, 0.1), 0.0, 6.0) : 0.0;
    vec4 seen = texture(picture, uv, defocus);
    float alpha = seen.a * fade;
    vec3 colour = seen.rgb;
    // Only with lights about: most worlds have none.
    if (lightCount > 0.5) {
        vec3 n = length(surface) > 0.001 ? normalize(surface) : normalize(cross(dFdx(worldPos), dFdy(worldPos)));
        if (dot(n, toEye) < 0.0)
            n = -n;
        colour += lights(seen.rgb, n, worldPos, toEye);
    }
    FRAGCOLOR = vec4(mix(colour, hazeColour.rgb, haze) * alpha, alpha);
}
