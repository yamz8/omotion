VARYING vec2 pictureUv;
VARYING vec4 own;
VARYING float worn;
VARYING vec3 facingNormal;
VARYING vec3 toEye;
VARYING vec3 worldPos;

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
    for (int k = 0; k < 48; ++k) {
        float t = mix(enter, leave, (float(k) + 0.5) / 48.0);
        vec3 at = (a + d * t) * 0.5 + 0.5;
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
    return (albedo * diffuse + vec3(highlight)) * colour * fall * shadowed(p, at);
}

vec3 lights(vec3 albedo, vec3 n, vec3 p, vec3 eye)
{
    return oneLight(albedo, n, p, eye, lightAt0, lightColour0, lightReach0)
         + oneLight(albedo, n, p, eye, lightAt1, lightColour1, lightReach1)
         + oneLight(albedo, n, p, eye, lightAt2, lightColour2, lightReach2)
         + oneLight(albedo, n, p, eye, lightAt3, lightColour3, lightReach3);
}

// Where the picture saw the shape it wears the picture; elsewhere its own
// colour, lit at its edges by the light around it. Farther than the picture
// had it, it fades into the air. Solid, with a hard edge, so it hides its
// own far side.
void MAIN()
{
    float defocus = aperture > 0.0 ? clamp(aperture * abs(length(toEye) - focus) / max(focus, 0.1), 0.0, 6.0) : 0.0;
    vec4 seen = texture(picture, pictureUv, defocus);
    float alpha = mix(own.a, seen.a, worn);
    if (alpha < 0.5)
        discard;
    vec3 colour = mix(own.rgb, seen.rgb, worn);
    float facing = abs(dot(normalize(facingNormal), normalize(toEye)));
    float rim = pow(1.0 - facing, 3.0) * (1.0 - worn);
    colour = mix(colour, wrap.rgb, clamp(rim * 0.8, 0.0, 1.0));
    if (lightCount > 0.5) {
        vec3 n = normalize(facingNormal);
        if (dot(n, toEye) < 0.0)
            n = -n;
        colour += lights(colour, n, worldPos, toEye);
    }
    colour = mix(colour, hazeColour.rgb, haze);
    FRAGCOLOR = vec4(colour, 1.0);
}
